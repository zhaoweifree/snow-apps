#include "snow_shot/presentation/screenshotexportartifact.h"
#include "snowimageqtcodec.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"
#include "snow_shot/diagnostics/diagnostics.h"
#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>
#include <QMimeData>

#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "snow_draw_engine_qt/snow_canvas_image.h"
#include "../../test-support/virtualmemory.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QImage>
#include <QThread>
#include <QScopeGuard>
#include <QSemaphore>
#include <QColorSpace>
#include <QCryptographicHash>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>

#if defined(Q_OS_WIN) || defined(_WIN32)
#include <windows.h>
struct ScreenshotClipboardPayloadTestAccess {
    static QByteArray dib(const ScreenshotClipboardPayload& payload) {
        auto handle = static_cast<HGLOBAL>(payload.m_dibHandle);
        const auto* memory = static_cast<const char*>(GlobalLock(handle));
        QByteArray result = memory ? QByteArray(memory, GlobalSize(handle)) : QByteArray{};
        if (memory)
            GlobalUnlock(handle);
        return result;
    }
    static bool independent(const ScreenshotClipboardPayload& first,
                            const ScreenshotClipboardPayload& second) {
        return first.m_dibHandle != second.m_dibHandle && first.m_pngHandle != second.m_pngHandle;
    }
};
#endif

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void processUntil(const std::function<bool()>& predicate) {
    for (int iteration = 0; iteration < 1000 && !predicate(); ++iteration) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    require(predicate(), "timed out waiting for export artifact completion");
}

QImage testImage() {
    QImage image(QSize(37, 19), QImage::Format_RGBA8888);
    image.setColorSpace(QColorSpace::SRgb);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            image.setPixelColor(x, y, QColor((x * 9) % 256, (y * 17) % 256, (x + y) % 256, 255));
        }
    }
    return image;
}

bool hasSamePixels(const QImage& actual, const QImage& expected) {
    if (actual.size() != expected.size())
        return false;
    const auto actualPixels = actual.convertToFormat(QImage::Format_RGBA8888);
    const auto expectedPixels = expected.convertToFormat(QImage::Format_RGBA8888);
    const auto rowBytes = static_cast<std::size_t>(expected.width()) * 4;
    for (int row = 0; row < expected.height(); ++row) {
        if (std::memcmp(actualPixels.constScanLine(row), expectedPixels.constScanLine(row),
                        rowBytes) != 0)
            return false;
    }
    return true;
}

ScreenshotImageRowSource rowSourceFor(const QImage& source, std::function<bool()> cancellation) {
    const QImage image = source.convertToFormat(QImage::Format_RGBA8888);
    const qsizetype rowBytes = static_cast<qsizetype>(image.width()) * 4;
    ScreenshotImageRowSource rows;
    rows.size = image.size();
    rows.cancellationRequested = std::move(cancellation);
    rows.readRows = [image, rowBytes](int firstRow, int rowCount, qsizetype stride,
                                      uchar* destination, qsizetype destinationSize) {
        if (firstRow < 0 || rowCount <= 0 || rowCount > image.height() - firstRow ||
            stride < rowBytes || destination == nullptr ||
            destinationSize < stride * (rowCount - 1) + rowBytes) {
            return false;
        }
        for (int row = 0; row < rowCount; ++row) {
            std::memcpy(destination + static_cast<qsizetype>(row) * stride,
                        image.constScanLine(firstRow + row), static_cast<std::size_t>(rowBytes));
        }
        return true;
    };
    return rows;
}

void clipboardUsesConfiguredCompression() {
    const QImage image = testImage();
    for (auto compression : {ScreenshotCompressionLevel::Low, ScreenshotCompressionLevel::Medium,
                             ScreenshotCompressionLevel::High}) {
        const int level =
            ScreenshotImageFileService::encodeOptions(
                ScreenshotImageFileFormat::Png, ScreenshotImageEncodingOptions{100, compression})
                .compression_level;
        const QByteArray expected =
            snow_shot::image_codec::encodePng(snow_shot::image_codec::srgbRowSource(image), level);
        ScreenshotExportArtifact artifact(ScreenshotExportSource::fromImage(image), compression);
        QObject receiver;
        QByteArray clipboard;
        require(artifact.requestClipboard(
                    &receiver,
                    [&](ScreenshotExportClipboardResult result) {
                        require(result.succeeded() && result.payload.pngBytes() == expected,
                                "clipboard encoding ignored the configured compression");
                        clipboard = result.payload.pngBytes();
                    }),
                "clipboard request rejected");
        processUntil([&] { return !clipboard.isEmpty(); });
        QByteArray cached;
        require(artifact.requestPng(
                    &receiver, compression,
                    [&](ScreenshotExportEncodingResult result) { cached = result.image.bytes(); }),
                "configured PNG request rejected");
        require(cached.constData() == clipboard.constData(),
                "clipboard did not retain its encoded PNG for subsequent outputs");
        QByteArray canonical;
        require(artifact.requestCanonicalPng(&receiver,
                                             [&](ScreenshotExportEncodingResult result) {
                                                 require(result.succeeded(),
                                                         "canonical encoding failed");
                                                 canonical = result.image.bytes();
                                             }),
                "canonical request rejected after clipboard preparation");
        processUntil([&] { return !canonical.isEmpty(); });
        require(canonical == expected, "canonical PNG ignored the configured compression");
        require(canonical.constData() == clipboard.constData(),
                "matching PNG outputs did not share the same buffer");
    }
}

void clipboardSharesPendingConfiguredEncoding() {
    const QImage image = testImage();
    auto firstRead = std::make_shared<std::atomic_bool>(true);
    auto entered = std::make_shared<std::atomic_bool>(false);
    auto released = std::make_shared<std::atomic_bool>(false);
    const auto release = qScopeGuard([released] { released->store(true); });
    auto rows = rowSourceFor(image, {});
    rows.readRows = [read = rows.readRows, firstRead, entered, released](
                        int first, int count, qsizetype stride, uchar* target, qsizetype capacity) {
        if (firstRead->exchange(false)) {
            entered->store(true);
            while (!released->load())
                QThread::msleep(1);
        }
        return read(first, count, stride, target, capacity);
    };
    std::atomic_int encodings = 0;
    ScreenshotExportArtifact artifact(
        ScreenshotExportSource::fromProducer({},
                                             [rows](std::function<bool()> cancellation) mutable {
                                                 rows.cancellationRequested =
                                                     std::move(cancellation);
                                                 return rows;
                                             }),
        ScreenshotCompressionLevel::High, {64 * 1024 * 1024, [&] { ++encodings; }});
    QObject receiver;
    bool encoded = false;
    QByteArray canonical;
    require(artifact.requestCanonicalPng(&receiver,
                                         [&](ScreenshotExportEncodingResult result) {
                                             require(result.succeeded(),
                                                     "high-compression fixture failed");
                                             encoded = true;
                                             canonical = result.image.bytes();
                                         }),
            "high-compression fixture rejected");
    processUntil([&] { return entered->load(); });
    bool copied = false;
    require(artifact.requestClipboard(
                &receiver,
                [&](ScreenshotExportClipboardResult result) {
                    require(result.succeeded() &&
                                result.payload.pngBytes() ==
                                    snow_shot::image_codec::encodePng(
                                        snow_shot::image_codec::srgbRowSource(image), 9) &&
                                result.payload.pngBytes().constData() == canonical.constData(),
                            "clipboard did not share the configured PNG encoding");
                    copied = true;
                }),
            "clipboard request rejected during high-compression encoding");
    require(!copied, "clipboard completed before the configured encoding");
    released->store(true);
    processUntil([&] { return encoded && copied; });
    require(encodings == 1, "clipboard started a separate PNG encoding");
}

void clipboardReusesConfiguredPng() {
    const QImage image = testImage();
    for (auto compression : {ScreenshotCompressionLevel::Low, ScreenshotCompressionLevel::Medium,
                             ScreenshotCompressionLevel::High}) {
        ScreenshotExportArtifact artifact(ScreenshotExportSource::fromImage(image), compression);
        QObject receiver;
        QByteArray encoded;
        require(artifact.requestCanonicalPng(&receiver,
                                             [&](ScreenshotExportEncodingResult result) {
                                                 require(result.succeeded(),
                                                         "existing PNG encoding failed");
                                                 encoded = result.image.bytes();
                                             }),
                "existing PNG request rejected");
        processUntil([&] { return !encoded.isEmpty(); });
        bool copied = false;
        require(artifact.requestClipboard(
                    &receiver,
                    [&](ScreenshotExportClipboardResult result) {
                        require(result.succeeded() &&
                                    result.payload.pngBytes().constData() == encoded.constData(),
                                "clipboard re-encoded an already available PNG");
                        copied = true;
                    }),
                "clipboard reuse request rejected");
        processUntil([&] { return copied; });
    }
}

void clipboardPreparationFusesRowsAndClonesNativeOwnership() {
    auto image = testImage();
    image.setPixelColor(0, 0, QColor(255, 0, 0, 0));
    image.setPixelColor(1, 0, QColor(255, 0, 0, 127));
    image.setPixelColor(2, 0, QColor(0, 255, 0, 255));
    auto rows = snow_shot::image_codec::srgbRowSource(image);
    const auto read = rows.readRows;
    int readRows = 0;
    rows.readRows = [read, &readRows](int first, int count, qsizetype stride, uchar* target,
                                      qsizetype capacity) {
        readRows += count;
        return read(first, count, stride, target, capacity);
    };
    auto payload = ScreenshotClipboardService::prepare(rows);
    require(payload.isValid() && readRows == image.height() &&
                hasSamePixels(QImage::fromData(payload.pngBytes(), "PNG"), image),
            "clipboard PNG and native bitmap preparation consume every source row exactly once");
    auto clone = ScreenshotClipboardService::clonePrepared(payload);
    require(clone.isValid() && readRows == image.height() &&
                clone.pngBytes().constData() == payload.pngBytes().constData(),
            "native clipboard cloning shares immutable PNG bytes without reading image rows");
#if defined(Q_OS_WIN) || defined(_WIN32)
    require(ScreenshotClipboardPayloadTestAccess::independent(payload, clone),
            "cloned native clipboard handles have independent publication ownership");
    const auto dib = ScreenshotClipboardPayloadTestAccess::dib(payload);
    require(dib == ScreenshotClipboardPayloadTestAccess::dib(clone),
            "native cloning preserves DIB header, pixels and initialized allocation padding");
    const auto* header = reinterpret_cast<const BITMAPINFOHEADER*>(dib.constData());
    const auto stride = (image.width() * 3 + 3) & ~3;
    const auto* top = reinterpret_cast<const uchar*>(dib.constData() + sizeof(*header)) +
                      (image.height() - 1) * stride;
    require(header->biWidth == image.width() && header->biHeight == image.height() &&
                header->biBitCount == 24 && top[0] == 255 && top[1] == 255 && top[2] == 255 &&
                top[3] == 128 && top[4] == 128 && top[5] == 255 && top[6] == 0 && top[7] == 255 &&
                top[8] == 0,
            "fused DIB retains bottom-up BGR order and white-flattened alpha");
#endif
    payload = {};
    require(clone.isValid(), "cloned clipboard ownership outlives its prototype");
    bool cancelled = false;
    rows.cancellationRequested = [&] { return cancelled; };
    rows.readRows = [read, &cancelled](int first, int count, qsizetype stride, uchar* target,
                                       qsizetype capacity) {
        const bool result = read(first, count, stride, target, capacity);
        cancelled = true;
        return result;
    };
    require(!ScreenshotClipboardService::prepare(rows).isValid(),
            "cancellation during fused row preparation releases partial native allocations");
}

void coldClipboardRequestsShareCanonicalRowsAndRetryFailures() {
    const auto image = testImage();
    std::atomic_int rowsRead{0};
    std::atomic_int encodings{0};
    bool fail = false;
    auto rows = snow_shot::image_codec::srgbRowSource(image);
    const auto read = rows.readRows;
    rows.readRows = [read, &rowsRead, &fail](int first, int count, qsizetype stride, uchar* target,
                                             qsizetype capacity) {
        rowsRead.fetch_add(count);
        return !fail && read(first, count, stride, target, capacity);
    };
    ScreenshotExportArtifact artifact(
        ScreenshotExportSource::fromProducer({},
                                             [rows](std::function<bool()> cancellation) {
                                                 auto source = rows;
                                                 source.cancellationRequested =
                                                     std::move(cancellation);
                                                 return source;
                                             }),
        ScreenshotCompressionLevel::Low, {64 * 1024 * 1024, [&] { encodings.fetch_add(1); }});
    QObject receiver;
    ScreenshotClipboardPayload first;
    ScreenshotClipboardPayload second;
    QByteArray png;
    require(artifact.requestClipboard(&receiver,
                                      [&](ScreenshotExportClipboardResult result) {
                                          require(result.succeeded(),
                                                  "cold fused clipboard request");
                                          first = std::move(result.payload);
                                      }) &&
                artifact.requestClipboard(&receiver,
                                          [&](ScreenshotExportClipboardResult result) {
                                              require(result.succeeded(),
                                                      "concurrent fused clipboard request");
                                              second = std::move(result.payload);
                                          }) &&
                artifact.requestCanonicalPng(&receiver,
                                             [&](ScreenshotExportEncodingResult result) {
                                                 require(result.succeeded(),
                                                         "concurrent canonical PNG request");
                                                 png = result.image.bytes();
                                             }),
            "cold clipboard and canonical subscribers are admitted together");
    processUntil([&] { return first.isValid() && second.isValid() && !png.isEmpty(); });
    require(
        rowsRead == image.height() && encodings == 1 &&
            first.pngBytes().constData() == png.constData() &&
            second.pngBytes().constData() == png.constData(),
        "concurrent cold clipboard consumers share one source pass and the canonical PNG cache");
    artifact.cancel();
    require(first.isValid() && second.isValid(),
            "consumer-owned native payloads survive artifact cancellation");

    fail = true;
    ScreenshotExportArtifact retry(
        ScreenshotExportSource::fromProducer({}, [rows](auto cancellation) {
            auto source = rows;
            source.cancellationRequested = std::move(cancellation);
            return source;
        }));
    bool failed = false;
    require(retry.requestClipboard(
                &receiver,
                [&](ScreenshotExportClipboardResult result) {
                    require(!result.succeeded() && !result.error.isEmpty(),
                            "failed cold clipboard preparation reports its actual failure");
                    failed = true;
                }),
            "failed cold clipboard request admission");
    processUntil([&] { return failed; });
    fail = false;
    bool succeeded = false;
    require(retry.requestClipboard(
                &receiver,
                [&](ScreenshotExportClipboardResult result) {
                    require(result.succeeded(),
                            "cold clipboard can retry after a transient row failure");
                    succeeded = true;
                }),
            "retry cold clipboard request admission");
    processUntil([&] { return succeeded; });
}

void cancelledColdClipboardRetainsConsumerLeaseUntilWorkerRetires() {
    auto entered = std::make_shared<std::atomic_bool>(false);
    auto released = std::make_shared<std::atomic_bool>(false);
    const auto unblock = qScopeGuard([released] { released->store(true); });
    auto rows = snow_shot::image_codec::srgbRowSource(testImage());
    const auto read = rows.readRows;
    rows.readRows = [entered, released, read](int first, int count, qsizetype stride,
                                              uchar* destination, qsizetype capacity) {
        entered->store(true);
        while (!released->load())
            QThread::msleep(1);
        return read(first, count, stride, destination, capacity);
    };
    ScreenshotExportArtifact artifact(
        ScreenshotExportSource::fromProducer({}, [rows](std::function<bool()> cancellation) {
            auto source = rows;
            source.cancellationRequested = std::move(cancellation);
            return source;
        }));
    QObject receiver;
    auto lease = std::make_shared<int>(1);
    const std::weak_ptr<int> weak = lease;
    int callbacks = 0;
    require(artifact.requestClipboard(&receiver,
                                      [lease, &callbacks](ScreenshotExportClipboardResult) {
                                          Q_UNUSED(lease);
                                          ++callbacks;
                                      }),
            "cold clipboard cancellation fixture admission");
    lease.reset();
    processUntil([&] { return entered->load(); });
    artifact.cancel();
    require(!weak.expired(), "cancelling fused clipboard work retains the consumer resource lease "
                             "while rows are in use");
    released->store(true);
    processUntil([&] { return weak.expired(); });
    require(callbacks == 0, "cancelled fused clipboard work suppresses its pending callback");
}

void latePngSubscriberRetainsLeaseDuringFusedClipboardCancellation() {
    auto entered = std::make_shared<std::atomic_bool>(false);
    auto released = std::make_shared<std::atomic_bool>(false);
    const auto unblock = qScopeGuard([released] { released->store(true); });
    auto rows = snow_shot::image_codec::srgbRowSource(testImage());
    const auto read = rows.readRows;
    rows.readRows = [entered, released, read](int first, int count, qsizetype stride,
                                              uchar* destination, qsizetype capacity) {
        entered->store(true);
        while (!released->load())
            QThread::msleep(1);
        return read(first, count, stride, destination, capacity);
    };
    ScreenshotExportArtifact artifact(
        ScreenshotExportSource::fromProducer({}, [rows](std::function<bool()> cancellation) {
            auto source = rows;
            source.cancellationRequested = std::move(cancellation);
            return source;
        }));
    QObject receiver;
    int callbacks = 0;
    require(
        artifact.requestClipboard(&receiver, [&](ScreenshotExportClipboardResult) { ++callbacks; }),
        "late canonical PNG lease fixture clipboard admission");
    processUntil([&] { return entered->load(); });

    // The clipboard worker has already captured its mode and started reading. A PNG
    // consumer joining now must share the lease holder retained by that same worker.
    auto lease = std::make_shared<int>(1);
    const std::weak_ptr<int> weak = lease;
    require(artifact.requestCanonicalPng(&receiver,
                                         [lease, &callbacks](ScreenshotExportEncodingResult) {
                                             Q_UNUSED(lease);
                                             ++callbacks;
                                         }),
            "late canonical PNG subscriber joins the running fused clipboard encode");
    lease.reset();
    artifact.cancel();
    require(!weak.expired(), "cancellation retains a late PNG consumer lease while the fused "
                             "clipboard worker is still reading");
    released->store(true);
    processUntil([&] { return weak.expired(); });
    require(callbacks == 0, "cancelled fused encoding suppresses clipboard and late PNG callbacks");
}

void clipboardIgnoresCachedPngWithDifferentCompression() {
    const QImage image = testImage();
    for (auto configured : {ScreenshotCompressionLevel::Low, ScreenshotCompressionLevel::Medium,
                            ScreenshotCompressionLevel::High}) {
        for (auto cached : {ScreenshotCompressionLevel::Low, ScreenshotCompressionLevel::Medium,
                            ScreenshotCompressionLevel::High}) {
            if (cached == configured)
                continue;
            std::atomic_int encodings = 0;
            ScreenshotExportArtifact artifact(ScreenshotExportSource::fromImage(image), configured,
                                              {64 * 1024 * 1024, [&] { ++encodings; }});
            QObject receiver;
            QByteArray otherPng;
            require(artifact.requestPng(&receiver, cached,
                                        [&](ScreenshotExportEncodingResult result) {
                                            require(result.succeeded(),
                                                    "cached PNG fixture failed");
                                            otherPng = result.image.bytes();
                                        }),
                    "cached PNG fixture rejected");
            processUntil([&] { return !otherPng.isEmpty(); });
            QByteArray clipboard;
            require(artifact.requestClipboard(&receiver,
                                              [&](ScreenshotExportClipboardResult result) {
                                                  require(result.succeeded(),
                                                          "clipboard preparation failed");
                                                  clipboard = result.payload.pngBytes();
                                              }),
                    "clipboard request rejected with a different cached compression");
            processUntil([&] { return !clipboard.isEmpty(); });
            const int level = ScreenshotImageFileService::encodeOptions(
                                  ScreenshotImageFileFormat::Png, {100, configured})
                                  .compression_level;
            require(clipboard == snow_shot::image_codec::encodePng(
                                     snow_shot::image_codec::srgbRowSource(image), level) &&
                        clipboard != otherPng && encodings == 2,
                    "cached PNG with different settings overrode clipboard compression");
            require(artifact.requestCanonicalPng(
                        &receiver,
                        [&](ScreenshotExportEncodingResult result) {
                            require(
                                result.succeeded() &&
                                    result.image.bytes().constData() == clipboard.constData(),
                                "configured clipboard PNG was not shared with canonical output");
                        }),
                    "canonical request rejected after clipboard encoding");
        }
    }
}

void failedPngRequestsCanRetryWithoutInvalidatingOtherLevels() {
    const QImage image = testImage();
    auto fail = std::make_shared<std::atomic_bool>(false);
    auto rows = rowSourceFor(image, {});
    rows.readRows = [read = rows.readRows, fail](int first, int count, qsizetype stride,
                                                 uchar* target, qsizetype capacity) {
        return !fail->load() && read(first, count, stride, target, capacity);
    };
    ScreenshotExportArtifact artifact(ScreenshotExportSource::fromProducer(
                                          {},
                                          [rows](std::function<bool()> cancellation) mutable {
                                              rows.cancellationRequested = std::move(cancellation);
                                              return rows;
                                          }),
                                      ScreenshotCompressionLevel::High);
    QObject receiver;
    QByteArray otherPng;
    require(artifact.requestPng(&receiver, ScreenshotCompressionLevel::Low,
                                [&](ScreenshotExportEncodingResult result) {
                                    require(result.succeeded(), "low-compression fixture failed");
                                    otherPng = result.image.bytes();
                                }),
            "low-compression fixture rejected");
    processUntil([&] { return !otherPng.isEmpty(); });
    fail->store(true);
    int callbacks = 0;
    require(artifact.requestCanonicalPng(
                &receiver,
                [&](ScreenshotExportEncodingResult result) {
                    require(!result.succeeded() && !result.error.isEmpty(),
                            "failed high-compression PNG did not report its error");
                    ++callbacks;
                }),
            "failing PNG request rejected");
    processUntil([&] { return callbacks == 1; });
    fail->store(false);
    QByteArray clipboard;
    require(artifact.requestClipboard(
                &receiver,
                [&](ScreenshotExportClipboardResult result) {
                    require(result.succeeded(),
                            "file compression failure poisoned clipboard encoding");
                    clipboard = result.payload.pngBytes();
                    ++callbacks;
                }),
            "clipboard request rejected after file encoding failure");
    processUntil([&] { return callbacks == 2; });
    require(artifact.requestCanonicalPng(
                &receiver,
                [&](ScreenshotExportEncodingResult result) {
                    require(result.succeeded() &&
                                result.image.bytes() ==
                                    snow_shot::image_codec::encodePng(
                                        snow_shot::image_codec::srgbRowSource(image), 9),
                            "a new PNG request could not retry a transient failure");
                    ++callbacks;
                }),
            "retry PNG request rejected");
    processUntil([&] { return callbacks == 3; });
    require(artifact.requestPng(
                &receiver, ScreenshotCompressionLevel::Low,
                [&](ScreenshotExportEncodingResult result) {
                    require(result.succeeded() &&
                                result.image.bytes().constData() == otherPng.constData(),
                            "retrying the configured encoding invalidated another cached level");
                    ++callbacks;
                }),
            "cached low-compression PNG request rejected");
    require(callbacks == 4, "unrelated cached encoding was invalidated");
}

void matchingRequestsSurviveSubscriberDestruction() {
    std::function<void(QImage)> finishLoad;
    ScreenshotExportArtifact artifact(ScreenshotExportSource::fromImageLoader(
                                          [&](QObject*, std::function<void(QImage)> callback) {
                                              finishLoad = std::move(callback);
                                              return true;
                                          }),
                                      ScreenshotCompressionLevel::Low);
    QObject receiver;
    auto destroyed = std::make_unique<QObject>();
    int callbacks = 0;
    QByteArray first;
    QByteArray second;
    require(artifact.requestPng(destroyed.get(), ScreenshotCompressionLevel::Low,
                                [](ScreenshotExportEncodingResult) {
                                    require(false, "destroyed PNG subscriber called");
                                }) &&
                artifact.requestCanonicalPng(&receiver,
                                             [&](ScreenshotExportEncodingResult result) {
                                                 require(result.succeeded(),
                                                         "surviving PNG subscriber failed");
                                                 first = result.image.bytes();
                                                 ++callbacks;
                                             }) &&
                artifact.requestClipboard(&receiver,
                                          [&](ScreenshotExportClipboardResult result) {
                                              require(result.succeeded(),
                                                      "surviving clipboard subscriber failed");
                                              second = result.payload.pngBytes();
                                              ++callbacks;
                                          }),
            "concurrent PNG requests rejected");
    destroyed.reset();
    finishLoad(testImage());
    processUntil([&] { return callbacks == 2; });
    require(first.constData() == second.constData(),
            "subscriber destruction cancelled or duplicated the shared PNG encoding");
}

void manualPngSavesUseTheirRequestedCompression() {
    const QImage image = testImage();
    ScreenshotExportArtifact artifact(ScreenshotExportSource::fromImage(image),
                                      ScreenshotCompressionLevel::High);
    QObject receiver;
    QTemporaryDir directory;
    for (auto compression : {ScreenshotCompressionLevel::Low, ScreenshotCompressionLevel::Medium,
                             ScreenshotCompressionLevel::High}) {
        QByteArray png;
        QString path;
        int callbacks = 0;
        require(artifact.requestPng(&receiver, compression,
                                    [&](ScreenshotExportEncodingResult result) {
                                        require(result.succeeded(),
                                                "requested PNG encoding failed");
                                        png = result.image.bytes();
                                        ++callbacks;
                                    }),
                "requested PNG encoding rejected");
        require(
            artifact.requestSaveToPath(&receiver, directory.filePath(QStringLiteral("manual.png")),
                                       ScreenshotImageFileFormat::Png,
                                       ScreenshotImageEncodingOptions{100, compression},
                                       [&](ScreenshotExportTaskResult result) {
                                           require(result.succeeded(), "manual PNG save failed");
                                           path = result.savedPath;
                                           ++callbacks;
                                       }),
            "manual PNG save rejected");
        processUntil([&] { return callbacks == 2; });
        const int level =
            ScreenshotImageFileService::encodeOptions(
                ScreenshotImageFileFormat::Png, ScreenshotImageEncodingOptions{100, compression})
                .compression_level;
        QFile file(path);
        require(file.open(QIODevice::ReadOnly) && file.readAll() == png &&
                    png == snow_shot::image_codec::encodePng(
                               snow_shot::image_codec::srgbRowSource(image), level),
                "manual PNG save ignored its requested compression");
        require(artifact.requestPng(&receiver, compression,
                                    [&](ScreenshotExportEncodingResult result) {
                                        require(result.image.bytes().constData() == png.constData(),
                                                "repeated PNG request re-encoded cached bytes");
                                        ++callbacks;
                                    }),
                "cached PNG request rejected");
        require(callbacks == 3, "cached PNG was not delivered immediately");
    }
}

void clipboardFilesUseTemporaryStorageAndKeepExportOptions() {
    const QImage image = testImage();
    QObject receiver;
    QTemporaryDir output;
    require(output.isValid(), "clipboard file export fixture unavailable");
    const QDateTime requestedAt =
        QDateTime::fromString(QStringLiteral("2026-10-04T12:34:56"), Qt::ISODate);
    for (bool streaming : {false, true}) {
        for (const auto format : {ScreenshotImageFileFormat::Png, ScreenshotImageFileFormat::Jpeg,
                                  ScreenshotImageFileFormat::Bmp, ScreenshotImageFileFormat::Pdf}) {
            auto artifact = std::make_unique<ScreenshotExportArtifact>(
                ScreenshotExportSource::fromImage(image), ScreenshotCompressionLevel::Low,
                ScreenshotExportArtifact::PngCachePolicy{streaming ? 1 : 64 * 1024 * 1024, {}});
            const ScreenshotImageEncodingOptions encoding{35, ScreenshotCompressionLevel::High};
            const QString filename = QStringLiteral("clipboard_{yyyy}-{MM}-{dd}_{HH}-{mm}-{ss}");
            std::unique_ptr<QMimeData> mime;
            bool copied = false;
            require(artifact->requestClipboardFile(
                        &receiver, format, filename, encoding,
                        [&](std::unique_ptr<QMimeData> result, QString error) {
                            require(result && error.isEmpty(), "temporary clipboard file failed");
                            mime = std::move(result);
                            copied = true;
                        },
                        {}, requestedAt),
                    "temporary clipboard file request rejected");
            processUntil([&] { return copied; });
            require(mime->urls().size() == 1 && mime->urls().front().isLocalFile() &&
                        !mime->hasImage(),
                    "file copy must publish one local URL without image data");
            const QString path = mime->urls().front().toLocalFile();
            require(QDir::fromNativeSeparators(path).startsWith(
                        QDir::fromNativeSeparators(QDir::tempPath()) + QLatin1Char('/')) &&
                        QFileInfo(path).fileName() ==
                            ScreenshotImageFileService::suggestedBaseName(filename, requestedAt) +
                                QLatin1Char('.') + ScreenshotImageFileService::extension(format) &&
                        QDir(output.path()).entryList(QDir::Files).isEmpty(),
                    "clipboard files must use temporary storage and the requested filename");
            artifact.reset();
            QFile file(path);
            require(file.open(QIODevice::ReadOnly),
                    "clipboard files must outlive their source artifact");
            const QByteArray bytes = file.readAll();
            file.close();
            if (format == ScreenshotImageFileFormat::Pdf) {
                require(bytes.startsWith("%PDF-") && bytes.contains("/DCTDecode"),
                        "temporary PDF must honor the requested quality");
            } else {
                const QString reference = output.filePath(
                    QStringLiteral("reference.") + ScreenshotImageFileService::extension(format));
                require(
                    ScreenshotImageFileService::write(image, reference, format, {}, {}, encoding)
                        .succeeded(),
                    "clipboard file encoding reference failed");
                QFile referenceFile(reference);
                require(referenceFile.open(QIODevice::ReadOnly) && referenceFile.readAll() == bytes,
                        "temporary file must retain the requested format and encoding settings");
                referenceFile.close();
                require(QFile::remove(reference), "reference file cleanup failed");
            }
            mime.reset();
            require(QFileInfo::exists(path),
                    "discarding a clipboard payload must preserve the exported file");
            require(QDir(QFileInfo(path).path()).removeRecursively(),
                    "clipboard file fixture cleanup failed");
        }
    }
}

void windowNamesSurviveAsynchronousFileExports() {
    QTemporaryDir directory;
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    require(directory.isValid() &&
                storage.initialize({directory.path(), directory.path(), 60000}).success,
            "window variable settings must initialize");
    const auto shutdown = qScopeGuard([&] { storage.shutdown(); });
    const snow_shot::storage::ScreenshotSettings settings;
    const QString folder = directory.filePath(QStringLiteral("{AN}/{YYYY}"));
    const QString filename = QStringLiteral("{WN}_{HH-mm-ss}");
    require(settings.setImageSaveDirectory(folder) && settings.setAutoSaveFilenameFormat(filename),
            "window templates must persist");
    const QDateTime timestamp =
        QDateTime::fromString(QStringLiteral("2026-10-16T12:34:56"), Qt::ISODate);
    QObject receiver;
    for (const bool quick : {false, true}) {
        std::function<void(QImage)> deliver;
        ScreenshotExportArtifact artifact(
            ScreenshotExportSource::fromImageLoader([&](QObject*, auto callback) {
                deliver = std::move(callback);
                return true;
            }));
        artifact.setWindowNames({QStringLiteral("Browser"), QStringLiteral("Tab {YYYY}/A")});
        std::optional<ScreenshotExportTaskResult> result;
        const auto complete = [&](ScreenshotExportTaskResult value) { result = std::move(value); };
        require(quick ? artifact.requestQuickSave(&receiver, complete, timestamp)
                      : artifact.requestAutomaticSaveToConfiguredDirectory(
                            &receiver, folder, ScreenshotImageFileFormat::Png, filename, {},
                            complete, {}, timestamp),
                "window metadata export must be accepted");
        // A pending save owns its names even if its artifact is reused before image delivery.
        artifact.setWindowNames({QStringLiteral("Changed"), QStringLiteral("Changed")});
        processUntil([&] { return static_cast<bool>(deliver); });
        deliver(testImage());
        processUntil([&] { return result.has_value(); });
        require(result->succeeded() &&
                    result->savedPath.contains(QStringLiteral("/Browser/2026/")) &&
                    QFileInfo(result->savedPath)
                        .fileName()
                        .startsWith(QStringLiteral("Tab {YYYY}_A_12-34-56")),
                "automatic and quick saves must keep request metadata and literal title braces");
    }
    ScreenshotExportArtifact clipboard(ScreenshotExportSource::fromImage(testImage()));
    clipboard.setWindowNames({QStringLiteral("App"), QStringLiteral("Window")});
    bool copied = false;
    require(clipboard.requestClipboardFile(
                &receiver, ScreenshotImageFileFormat::Png, QStringLiteral("{AN}_{WN}"), {},
                [&](std::unique_ptr<QMimeData> mime, QString error) {
                    require(mime && error.isEmpty(), "clipboard file export must succeed");
                    const QString path = mime->urls().constFirst().toLocalFile();
                    require(QFileInfo(path).fileName() == QStringLiteral("App_Window.png"),
                            "clipboard files must use the same naming context");
                    require(QDir(QFileInfo(path).path()).removeRecursively(),
                            "clipboard fixture cleanup failed");
                    copied = true;
                }),
            "clipboard request must be accepted");
    processUntil([&] { return copied; });
    ScreenshotExportArtifact later(ScreenshotExportSource::fromImage(testImage()));
    std::optional<ScreenshotExportTaskResult> result;
    require(later.requestQuickSave(
                &receiver, [&](ScreenshotExportTaskResult value) { result = std::move(value); },
                timestamp),
            "an export without live metadata must still save");
    processUntil([&] { return result.has_value(); });
    require(result->succeeded() &&
                result->savedPath.endsWith(QStringLiteral("/Unknown/2026/Unknown_12-34-56.png")),
            "later exports must use Unknown rather than current desktop names");
}

void saveDirectoryTemplatesUseRequestTime() {
    QTemporaryDir directory;
    require(directory.isValid(), "time-directory fixture unavailable");
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    require(storage.initialize({directory.path(), directory.path(), 60000}).success,
            "time-directory settings unavailable");
    const auto shutdown = qScopeGuard([&] { storage.shutdown(); });
    const snow_shot::storage::ScreenshotSettings settings;
    const QString pattern = directory.filePath(QStringLiteral("output/{YYYY}"));
    require(settings.setImageSaveDirectory(pattern) &&
                settings.setAutoSaveFilenameFormat(QStringLiteral("{YYYY-MM-DD_HH-mm-ss}")),
            "time-directory settings must persist");
    const QDateTime before =
        QDateTime::fromString(QStringLiteral("2026-12-31T23:59:59"), Qt::ISODate);
    QObject receiver;
    for (const bool quick : {false, true}) {
        ScreenshotExportArtifact artifact(ScreenshotExportSource::fromImage(testImage()));
        for (const auto& timestamp : {before, before.addSecs(1)}) {
            std::optional<ScreenshotExportTaskResult> result;
            const auto completed = [&](ScreenshotExportTaskResult saved) {
                result = std::move(saved);
            };
            require(quick ? artifact.requestQuickSave(&receiver, completed, timestamp)
                          : artifact.requestAutomaticSaveToConfiguredDirectory(
                                &receiver, pattern, ScreenshotImageFileFormat::Png,
                                QStringLiteral("{YYYY-MM-DD_HH-mm-ss}"), {}, completed, {},
                                timestamp),
                    "dated save must be accepted");
            processUntil([&] { return result.has_value(); });
            const QString expectedDirectory = directory.filePath(
                QStringLiteral("output/") + timestamp.toString(QStringLiteral("yyyy")));
            require(
                result->succeeded() &&
                    QFileInfo(result->savedPath).absolutePath() == expectedDirectory &&
                    QFileInfo(result->savedPath)
                        .baseName()
                        .startsWith(timestamp.toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss"))),
                "worker and quick saves must use the captured time for both folder and filename");
        }
    }
    QFile blocker(directory.filePath(QStringLiteral("blocked")));
    require(blocker.open(QIODevice::WriteOnly), "dated quick-save blocker must open");
    blocker.close();
    require(settings.setImageSaveDirectory(blocker.fileName() + QStringLiteral("/{YYYY}")),
            "blocked template must persist");
    ScreenshotExportArtifact artifact(ScreenshotExportSource::fromImage(testImage()));
    std::optional<ScreenshotExportTaskResult> failed;
    require(artifact.requestQuickSave(
                &receiver, [&](ScreenshotExportTaskResult saved) { failed = std::move(saved); },
                before),
            "blocked dated quick save must be accepted for processing");
    processUntil([&] { return failed.has_value(); });
    require(!failed->succeeded() && failed->savedPath.isEmpty(),
            "dated quick saves must fail without falling back to another folder");
}

void automaticSavesUseRequestedEncoding() {
    const QImage image = testImage();
    QTemporaryDir directory;
    require(directory.isValid(), "automatic save fixture unavailable");
    QObject receiver;
    const auto save = [&](ScreenshotImageFileFormat format, int quality,
                          ScreenshotCompressionLevel compression) {
        ScreenshotExportArtifact artifact(ScreenshotExportSource::fromImage(image),
                                          ScreenshotCompressionLevel::Low);
        std::optional<ScreenshotExportTaskResult> result;
        require(artifact.requestAutomaticSave(
                    &receiver, {directory.path()}, format, QStringLiteral("automatic-file"),
                    {quality, compression},
                    [&](ScreenshotExportTaskResult saved) { result = std::move(saved); }),
                "automatic save was rejected");
        processUntil([&] { return result.has_value(); });
        require(result->succeeded(), "automatic save failed");
        QFile file(result->savedPath);
        require(file.open(QIODevice::ReadOnly), "automatic save output is unreadable");
        return file.readAll();
    };
    for (auto compression : {ScreenshotCompressionLevel::Low, ScreenshotCompressionLevel::High}) {
        const int level =
            ScreenshotImageFileService::encodeOptions(
                ScreenshotImageFileFormat::Png, ScreenshotImageEncodingOptions{35, compression})
                .compression_level;
        require(save(ScreenshotImageFileFormat::Png, 35, compression) ==
                    snow_shot::image_codec::encodePng(image, level),
                "automatic save PNG ignored the configured compression level");
    }
    const QString referencePath = directory.filePath(QStringLiteral("jpeg-reference.jpg"));
    require(ScreenshotImageFileService::write(
                image, referencePath, ScreenshotImageFileFormat::Jpeg, {}, {},
                ScreenshotImageEncodingOptions{35, ScreenshotCompressionLevel::High})
                .succeeded(),
            "automatic save JPEG reference failed");
    QFile reference(referencePath);
    require(reference.open(QIODevice::ReadOnly) &&
                save(ScreenshotImageFileFormat::Jpeg, 35, ScreenshotCompressionLevel::High) ==
                    reference.readAll(),
            "automatic save JPEG ignored the configured quality");
    require(save(ScreenshotImageFileFormat::Pdf, 35, ScreenshotCompressionLevel::High)
                .contains("/DCTDecode"),
            "automatic save PDF ignored the configured quality");
}

void configuredAutomaticDirectoryResolvesInTheSaveWorker() {
    QTemporaryDir directory;
    require(directory.isValid(), "configured directory fixture unavailable");
    QObject receiver;
    const QImage image = testImage();
    ScreenshotExportArtifact artifact(ScreenshotExportSource::fromImage(image),
                                      ScreenshotCompressionLevel::Medium);
    ScreenshotExportArtifact cancelled(ScreenshotExportSource::fromImage(image),
                                       ScreenshotCompressionLevel::Medium);
    const auto warm = [&](ScreenshotExportArtifact& source) {
        bool ready = false;
        require(source.requestCanonicalPng(&receiver,
                                           [&](ScreenshotExportEncodingResult result) {
                                               require(result.succeeded(),
                                                       "configured directory PNG fixture failed");
                                               ready = true;
                                           }),
                "configured directory PNG fixture rejected");
        processUntil([&] { return ready; });
        require(source.cachedPng(ScreenshotCompressionLevel::Medium).isValid(),
                "configured directory fixture must retain its encoded PNG");
    };
    warm(artifact);
    warm(cancelled);

    auto& coordinator = ScreenshotExportCoordinator::shared();
    processUntil([&] { return coordinator.pendingJobCount() == 0; });
    const int workerCount = std::clamp(QThread::idealThreadCount(), 1, 2);
    auto entered = std::make_shared<std::atomic_int>(0);
    auto release = std::make_shared<QSemaphore>();
    const auto unblock = qScopeGuard([release, workerCount] { release->release(workerCount); });
    for (int index = 0; index < workerCount; ++index) {
        require(coordinator
                    .submit(
                        &receiver, ScreenshotExportCoordinator::Priority::Foreground,
                        [entered, release](const ScreenshotExportCancellation&) {
                            entered->fetch_add(1, std::memory_order_release);
                            release->acquire();
                            return ScreenshotExportTaskResult{};
                        },
                        [](ScreenshotExportTaskResult) {})
                    .isValid(),
                "configured directory worker blocker rejected");
    }
    processUntil([&] { return entered->load(std::memory_order_acquire) == workerCount; });

    const QString configured = directory.filePath(QStringLiteral("configured"));
    const QString explicitDirectory = directory.filePath(QStringLiteral("explicit"));
    const QString cancelledDirectory = directory.filePath(QStringLiteral("cancelled"));
    QString requestedDirectory = configured;
    std::optional<ScreenshotExportTaskResult> saved;
    std::optional<ScreenshotExportTaskResult> explicitSaved;
    bool cancelledCallback = false;
    require(!QFileInfo::exists(configured) && !QFileInfo::exists(explicitDirectory),
            "configured directory fixture must start without output folders");
    require(artifact.requestAutomaticSaveToConfiguredDirectory(
                &receiver, requestedDirectory, ScreenshotImageFileFormat::Png,
                QStringLiteral("configured-output"), {},
                [&](ScreenshotExportTaskResult result) { saved = std::move(result); }),
            "configured directory save rejected");
    requestedDirectory = directory.filePath(QStringLiteral("later-settings"));
    require(artifact.requestAutomaticSave(
                &receiver, {explicitDirectory}, ScreenshotImageFileFormat::Png,
                QStringLiteral("explicit-output"), {},
                [&](ScreenshotExportTaskResult result) { explicitSaved = std::move(result); }),
            "explicit directory save rejected");
    require(cancelled.requestAutomaticSaveToConfiguredDirectory(
                &receiver, cancelledDirectory, ScreenshotImageFileFormat::Png,
                QStringLiteral("cancelled-output"), {},
                [&](ScreenshotExportTaskResult) { cancelledCallback = true; }),
            "cancelled directory save fixture rejected");
    // Cached rows and PNG bytes let the GUI finish scheduling while every export
    // worker is occupied. Eligibility must remain unresolved until the save runs.
    processUntil([&] { return coordinator.pendingJobCount() == workerCount + 3; });
    require(!saved && !explicitSaved && !QFileInfo::exists(configured) &&
                !QFileInfo::exists(explicitDirectory),
            "queued saves must not create directories on the GUI thread");
    cancelled.cancel();
    require(coordinator.pendingJobCount() == workerCount + 2,
            "cancelled configured save must release its queued worker slot");
    require(!cancelled.requestAutomaticSaveToConfiguredDirectory(
                &receiver, cancelledDirectory, ScreenshotImageFileFormat::Png,
                QStringLiteral("rejected-output"), {},
                [&](ScreenshotExportTaskResult) { cancelledCallback = true; }) &&
                !artifact.requestAutomaticSaveToConfiguredDirectory(
                    nullptr, cancelledDirectory, ScreenshotImageFileFormat::Png,
                    QStringLiteral("rejected-output"), {},
                    [&](ScreenshotExportTaskResult) { cancelledCallback = true; }) &&
                !artifact.requestAutomaticSaveToConfiguredDirectory(
                    &receiver, cancelledDirectory, ScreenshotImageFileFormat::Png,
                    QStringLiteral("rejected-output"), {}, {}),
            "invalid configured save requests must be rejected without output work");
    require(QDir().mkpath(configured), "configured output folder fixture could not be created");
    release->release(workerCount);
    processUntil([&] {
        return saved.has_value() && explicitSaved.has_value() && coordinator.pendingJobCount() == 0;
    });
    QCoreApplication::processEvents();
    const auto removeOutputs = qScopeGuard([&] {
        if (saved)
            static_cast<void>(QFile::remove(saved->savedPath));
        if (explicitSaved)
            static_cast<void>(QFile::remove(explicitSaved->savedPath));
    });
    require(saved->succeeded() &&
                saved->savedPath ==
                    QDir(configured).filePath(QStringLiteral("configured-output.png")) &&
                QImage(saved->savedPath).convertToFormat(QImage::Format_RGBA8888) == image,
            "configured save must resolve its captured directory inside the save worker");
    require(explicitSaved->succeeded() &&
                explicitSaved->savedPath ==
                    QDir(explicitDirectory).filePath(QStringLiteral("explicit-output.png")),
            "explicit directory saves must retain their missing-folder creation behavior");
    require(!cancelledCallback && !QFileInfo::exists(cancelledDirectory) &&
                !QFileInfo::exists(requestedDirectory),
            "cancelled, rejected, and later directory values must not produce output");
}

void clipboardAndSavesShareConfiguredEncoding(ScreenshotCompressionLevel saveCompression,
                                              bool clipboardFirst) {
    const QImage image = testImage();
    for (bool rowBacked : {false, true}) {
        std::atomic_int materializations = 0;
        std::atomic_int rowFactories = 0;
        ScreenshotExportSource::RowSourceFactory factory;
        if (rowBacked) {
            factory = [&rowFactories, image](std::function<bool()> cancellation) {
                ++rowFactories;
                return rowSourceFor(image, std::move(cancellation));
            };
        }
        ScreenshotExportArtifact artifact(
            ScreenshotExportSource::fromProducer(
                [&materializations, image](const ScreenshotExportCancellation&) {
                    ++materializations;
                    return image;
                },
                factory),
            saveCompression);
        QTemporaryDir directory;
        require(directory.isValid(), "temporary save directory unavailable");
        QObject receiver;
        int callbacks = 0;
        QByteArray canonical;
        QByteArray clipboard;
        QString path;
        QString systemPath;
        const auto requestCanonical = [&] {
            require(artifact.requestCanonicalPng(&receiver,
                                                 [&](ScreenshotExportEncodingResult result) {
                                                     require(result.succeeded(),
                                                             "canonical PNG failed");
                                                     canonical = result.image.bytes();
                                                     ++callbacks;
                                                 }),
                    "canonical request rejected");
        };
        if (!clipboardFirst)
            requestCanonical();
        require(artifact.requestClipboard(&receiver,
                                          [&](ScreenshotExportClipboardResult result) {
                                              require(result.succeeded(),
                                                      "clipboard preparation failed");
                                              clipboard = result.payload.pngBytes();
                                              ++callbacks;
                                          }),
                "clipboard request rejected");
        if (clipboardFirst)
            requestCanonical();
        require(artifact.requestAutomaticSave(
                    &receiver, {directory.path()}, ScreenshotImageFileFormat::Png,
                    QStringLiteral("shared"), {100, saveCompression},
                    [&](ScreenshotExportTaskResult result) {
                        require(result.succeeded(), "automatic PNG save failed");
                        path = result.savedPath;
                        ++callbacks;
                    }),
                "save request rejected");
        require(artifact.requestSaveToPath(
                    &receiver, directory.filePath(QStringLiteral("system.png")),
                    ScreenshotImageFileFormat::Png,
                    ScreenshotImageEncodingOptions{100, saveCompression},
                    [&](ScreenshotExportTaskResult result) {
                        require(result.succeeded(), "system-dialog PNG save failed");
                        systemPath = result.savedPath;
                        ++callbacks;
                    }),
                "system-dialog save request rejected");
        processUntil([&] { return callbacks == 4; });
        const int level = ScreenshotImageFileService::encodeOptions(ScreenshotImageFileFormat::Png,
                                                                    {100, saveCompression})
                              .compression_level;
        require(clipboard == snow_shot::image_codec::encodePng(
                                 snow_shot::image_codec::srgbRowSource(image), level),
                "clipboard did not use the configured PNG encoding");
        require(canonical.constData() == clipboard.constData(),
                "concurrent clipboard/save/history requests encoded the same PNG more than once");
        QFile saved(path);
        require(saved.open(QIODevice::ReadOnly) && saved.readAll() == canonical,
                "saved PNG differs from history encoding");
        QFile systemSaved(systemPath);
        require(systemSaved.open(QIODevice::ReadOnly) && systemSaved.readAll() == canonical,
                "system-dialog PNG save did not reuse the canonical history bytes");
        require(materializations == (rowBacked ? 0 : 1),
                "image source was materialized more than once");
        require(rowFactories == (rowBacked ? 1 : 0),
                "PNG consumers did not reuse the cached row source");
        require(artifact.requestClipboard(
                    &receiver,
                    [&](ScreenshotExportClipboardResult result) {
                        require(result.succeeded() &&
                                    result.payload.pngBytes().constData() == clipboard.constData(),
                                "repeated clipboard request changed the configured PNG encoding");
                        ++callbacks;
                    }),
                "cached clipboard request rejected");
        processUntil([&] { return callbacks == 5; });
        require(rowFactories == (rowBacked ? 1 : 0),
                "cached clipboard request recreated the row source");
        require(artifact.requestSaveToPath(
                    &receiver, directory.filePath(QStringLiteral("compatible.bmp")),
                    ScreenshotImageFileFormat::Bmp,
                    ScreenshotImageEncodingOptions{100, saveCompression},
                    [&](ScreenshotExportTaskResult result) {
                        QFile file(result.savedPath);
                        require(result.succeeded() && file.open(QIODevice::ReadOnly) &&
                                    file.readAll().startsWith("BM") &&
                                    hasSamePixels(QImage(result.savedPath), image),
                                "non-PNG output reused incompatible canonical PNG bytes");
                        ++callbacks;
                    }),
                "non-PNG path save request rejected");
        processUntil([&] { return callbacks == 6; });
    }
}

void nonPngSaveReadsPixelsWithoutEncodingPng() {
    const QImage image = testImage();
    for (bool rowBacked : {false, true}) {
        std::atomic_int imageCalls = 0;
        std::atomic_int rowCalls = 0;
        ScreenshotExportSource::RowSourceFactory factory;
        if (rowBacked) {
            factory = [&rowCalls, image](std::function<bool()> cancellation) {
                ++rowCalls;
                return rowSourceFor(image, std::move(cancellation));
            };
        }
        ScreenshotExportArtifact artifact(ScreenshotExportSource::fromProducer(
            [&imageCalls, image](const ScreenshotExportCancellation&) {
                ++imageCalls;
                return image;
            },
            factory));
        QTemporaryDir directory;
        QObject receiver;
        int callbacks = 0;
        require(artifact.requestAutomaticSave(
                    &receiver, {directory.path()}, ScreenshotImageFileFormat::Webp,
                    QStringLiteral("pixels"), {},
                    [&](ScreenshotExportTaskResult result) {
                        require(result.succeeded() && QFile::exists(result.savedPath),
                                "non-PNG save failed");
                        ++callbacks;
                    }),
                "non-PNG save request rejected");
        processUntil([&] { return callbacks == 1; });
        require(rowCalls == (rowBacked ? 1 : 0) && imageCalls == (rowBacked ? 0 : 1),
                "non-PNG save performed an extra encoding or materialization");
        require(artifact.requestAutomaticSave(
                    &receiver, {directory.path()}, ScreenshotImageFileFormat::Png,
                    QStringLiteral("bad/name"), {},
                    [&](ScreenshotExportTaskResult result) {
                        require(!result.succeeded() && !result.error.isEmpty(),
                                "invalid save filename did not report failure");
                        ++callbacks;
                    }),
                "invalid save request must complete with an error");
        processUntil([&] { return callbacks == 2; });
    }
}

void fileEncodingsCoalesceAndNormalizeOptions() {
    const QImage image = testImage();
    QTemporaryDir directory;
    QObject receiver;
    for (auto format : {ScreenshotImageFileFormat::Png, ScreenshotImageFileFormat::Jpeg,
                        ScreenshotImageFileFormat::Bmp, ScreenshotImageFileFormat::Webp,
                        ScreenshotImageFileFormat::Jxl, ScreenshotImageFileFormat::Avif}) {
        std::atomic_int encodings = 0;
        ScreenshotExportArtifact::PngCachePolicy policy;
        policy.maximumBytes = format == ScreenshotImageFileFormat::Png ? 1 : 64 * 1024 * 1024;
        policy.fileEncodingStarted = [&](ScreenshotImageFileFormat encodedFormat) {
            require(encodedFormat == format, "file encoding observer received the wrong format");
            ++encodings;
        };
        ScreenshotExportArtifact artifact(ScreenshotExportSource::fromImage(image),
                                          ScreenshotCompressionLevel::Low, policy);
        int completed = 0;
        QByteArray expected;
        const auto saved = [&](ScreenshotExportTaskResult result) {
            QFile file(result.savedPath);
            require(result.succeeded() && file.open(QIODevice::ReadOnly),
                    "shared file output failed");
            const auto bytes = file.readAll();
            require(result.encodedByteCount == bytes.size() &&
                        result.encodedSha256 ==
                            QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex(),
                    "shared output lost its encoded digest");
            if (expected.isEmpty())
                expected = bytes;
            else
                require(expected == bytes, "matching outputs did not reuse identical bytes");
            ++completed;
        };
        const ScreenshotImageEncodingOptions encoding{90, ScreenshotCompressionLevel::Low};
        require(artifact.requestSaveToPath(&receiver, directory.filePath(QStringLiteral("manual")),
                                           format, encoding, saved) &&
                    artifact.requestAutomaticSave(&receiver, {directory.path()}, format,
                                                  QStringLiteral("automatic"), encoding, saved),
                "shared file requests rejected");
        processUntil([&] { return completed == 2; });
        require(encodings == 1, "concurrent file saves performed duplicate encodings");
        auto compatible = encoding;
        if (format == ScreenshotImageFileFormat::Bmp || format == ScreenshotImageFileFormat::Png)
            compatible.quality = -10;
        if (format == ScreenshotImageFileFormat::Bmp || format == ScreenshotImageFileFormat::Jpeg)
            compatible.compressionLevel = ScreenshotCompressionLevel::High;
        require(artifact.requestSaveToPath(&receiver,
                                           directory.filePath(QStringLiteral("repeated")), format,
                                           compatible, saved),
                "cached file save rejected");
        processUntil([&] { return completed == 3; });
        require(encodings == 1, "ineffective encoding options invalidated cached output");
        auto different = encoding;
        if (ScreenshotImageFileService::supportsQuality(format))
            different.quality = 75;
        else if (ScreenshotImageFileService::supportsCompressionLevel(format))
            different.compressionLevel = ScreenshotCompressionLevel::High;
        else
            continue;
        require(artifact.requestSaveToPath(
                    &receiver, directory.filePath(QStringLiteral("changed")), format, different,
                    [&](ScreenshotExportTaskResult result) {
                        require(result.succeeded(), "changed file encoding failed");
                        ++completed;
                    }),
                "changed file request rejected");
        processUntil([&] { return completed == 4; });
        require(encodings == 2, "effective encoding options reused incompatible output");
    }
}

void pdfPayloadReusePreservesDestinationMetadata() {
    QTemporaryDir directory;
    QObject receiver;
    std::atomic_int encodings = 0;
    ScreenshotExportArtifact::PngCachePolicy policy;
    policy.fileEncodingStarted = [&](ScreenshotImageFileFormat) { ++encodings; };
    ScreenshotExportArtifact artifact(ScreenshotExportSource::fromImage(testImage()),
                                      ScreenshotCompressionLevel::Medium, policy);
    int completed = 0;
    QByteArray portrait;
    QByteArray landscape;
    auto save = [&](const QString& name, ScreenshotPdfPageSize page, QByteArray& bytes) {
        require(artifact.requestSaveToPath(
                    &receiver, directory.filePath(name), ScreenshotImageFileFormat::Pdf, {},
                    [&bytes, &completed](ScreenshotExportTaskResult result) {
                        QFile file(result.savedPath);
                        require(result.succeeded() && file.open(QIODevice::ReadOnly),
                                "PDF reuse failed");
                        bytes = file.readAll();
                        ++completed;
                    },
                    ScreenshotPdfOptions{page}),
                "PDF reuse request rejected");
    };
    save(QStringLiteral("portrait"), ScreenshotPdfPageSize::PortraitA4, portrait);
    save(QStringLiteral("landscape"), ScreenshotPdfPageSize::LandscapeA4, landscape);
    processUntil([&] { return completed == 2; });
    require(encodings == 1 && portrait != landscape &&
                portrait.contains("0070006f007200740072006100690074") &&
                landscape.contains("006c0061006e006400730063006100700065"),
            "PDF layout or filename changes re-encoded tiles or lost document metadata");
}

void failedFileEncodingCanRetry() {
    QTemporaryDir directory;
    QObject receiver;
    const QImage image = testImage();
    std::atomic_bool fail = true;
    std::atomic_int encodings = 0;
    auto rows = rowSourceFor(image, {});
    const auto read = rows.readRows;
    rows.readRows = [&](int first, int count, qsizetype stride, uchar* pixels, qsizetype bytes) {
        return !fail.load() && read(first, count, stride, pixels, bytes);
    };
    ScreenshotExportArtifact::PngCachePolicy policy;
    policy.fileEncodingStarted = [&](ScreenshotImageFileFormat) { ++encodings; };
    ScreenshotExportArtifact artifact(
        ScreenshotExportSource::fromProducer({},
                                             [rows](std::function<bool()> cancelled) mutable {
                                                 rows.cancellationRequested = std::move(cancelled);
                                                 return rows;
                                             }),
        ScreenshotCompressionLevel::Medium, policy);
    int completed = 0;
    require(artifact.requestSaveToPath(&receiver, directory.filePath(QStringLiteral("retry.bmp")),
                                       ScreenshotImageFileFormat::Bmp, {},
                                       [&](ScreenshotExportTaskResult result) {
                                           require(!result.succeeded(),
                                                   "failed rows published a file");
                                           ++completed;
                                       }),
            "failing file request rejected");
    processUntil([&] { return completed == 1; });
    fail = false;
    require(artifact.requestSaveToPath(&receiver, directory.filePath(QStringLiteral("retry.bmp")),
                                       ScreenshotImageFileFormat::Bmp, {},
                                       [&](ScreenshotExportTaskResult result) {
                                           require(result.succeeded(),
                                                   "failed file entry poisoned the retry");
                                           ++completed;
                                       }),
            "file retry rejected");
    processUntil([&] { return completed == 2; });
    require(encodings == 2, "failed encoding was incorrectly retained");
}

void largeBmpUsesDirectStreamingAndDiskEntriesRespectBudget() {
    QTemporaryDir directory;
    QObject receiver;
    const QImage image = testImage();
    std::atomic_int encodings = 0;
    ScreenshotExportArtifact::PngCachePolicy policy;
    policy.maximumBytes = 1;
    policy.fileEncodingStarted = [&](ScreenshotImageFileFormat) { ++encodings; };
    ScreenshotExportArtifact bmp(ScreenshotExportSource::fromImage(image),
                                 ScreenshotCompressionLevel::Medium, policy);
    int completed = 0;
    require(bmp.requestSaveToPath(&receiver, directory.filePath(QStringLiteral("stream.bmp")),
                                  ScreenshotImageFileFormat::Bmp, {},
                                  [&](ScreenshotExportTaskResult result) {
                                      require(result.succeeded() &&
                                                  hasSamePixels(QImage(result.savedPath), image),
                                              "direct BMP streaming changed pixels");
                                      ++completed;
                                  }),
            "direct BMP request rejected");
    processUntil([&] { return completed == 1; });
    require(encodings == 0, "large uncompressed BMP unnecessarily spooled an encoded artifact");
    policy.maximumFileBytes = 0;
    ScreenshotExportArtifact png(ScreenshotExportSource::fromImage(image),
                                 ScreenshotCompressionLevel::Medium, policy);
    const auto save = [&](ScreenshotExportTaskResult result) {
        require(result.succeeded(), "unretained disk output failed");
        ++completed;
    };
    require(png.requestSaveToPath(&receiver, directory.filePath(QStringLiteral("first.png")),
                                  ScreenshotImageFileFormat::Png, {}, save) &&
                png.requestSaveToPath(&receiver, directory.filePath(QStringLiteral("second.png")),
                                      ScreenshotImageFileFormat::Png, {}, save),
            "unretained disk requests rejected");
    processUntil([&] { return completed == 3; });
    require(encodings == 1, "unretained in-flight disk encoding did not coalesce");
    require(png.requestSaveToPath(&receiver, directory.filePath(QStringLiteral("third.png")),
                                  ScreenshotImageFileFormat::Png, {}, save),
            "unretained disk retry rejected");
    processUntil([&] { return completed == 4; });
    require(encodings == 2, "zero disk budget retained an encoded artifact");
}

void imageRequestsShareOneAsyncLoad() {
    int loadCount = 0;
    std::function<void(QImage)> finishLoad;
    ScreenshotExportArtifact artifact(ScreenshotExportSource::fromImageLoader(
        [&loadCount, &finishLoad](QObject*, std::function<void(QImage)> callback) {
            ++loadCount;
            finishLoad = std::move(callback);
            return true;
        }));
    QObject firstReceiver;
    QObject secondReceiver;
    int callbacks = 0;
    require(artifact.requestImage(&firstReceiver,
                                  [&callbacks](ScreenshotExportImageResult result) {
                                      require(result.succeeded(),
                                              "first shared image request failed");
                                      ++callbacks;
                                  }) &&
                artifact.requestImage(&secondReceiver,
                                      [&callbacks](ScreenshotExportImageResult result) {
                                          require(result.succeeded(),
                                                  "second shared image request failed");
                                          ++callbacks;
                                      }),
            "shared image requests were rejected");
    require(loadCount == 1 && finishLoad, "concurrent image requests started duplicate loads");
    finishLoad(testImage());
    require(callbacks == 2, "shared image result did not fan out to both subscribers");
}

void canonicalEncodingCoalescesAndPreservesBufferIdentity() {
    const QImage image = testImage();
    std::atomic_int rowFactoryCount = 0;
    std::atomic_int imageProducerCount = 0;
    ScreenshotExportArtifact artifact(ScreenshotExportSource::fromProducer(
        [&imageProducerCount, image](const ScreenshotExportCancellation&) {
            ++imageProducerCount;
            return image;
        },
        [&rowFactoryCount, image](std::function<bool()> cancellation) {
            ++rowFactoryCount;
            return rowSourceFor(image, std::move(cancellation));
        }));
    QObject receiver;
    int callbacks = 0;
    const QByteArray* firstBuffer = nullptr;
    const QByteArray* secondBuffer = nullptr;
    require(artifact.requestCanonicalPng(
                &receiver,
                [&callbacks, &firstBuffer](ScreenshotExportEncodingResult result) {
                    require(result.succeeded(), "first canonical encoding failed");
                    firstBuffer = result.image.sharedBytes().get();
                    ++callbacks;
                }) &&
                artifact.requestCanonicalPng(
                    &receiver,
                    [&callbacks, &secondBuffer](ScreenshotExportEncodingResult result) {
                        require(result.succeeded(), "second canonical encoding failed");
                        secondBuffer = result.image.sharedBytes().get();
                        ++callbacks;
                    }),
            "canonical encoding requests were rejected");
    processUntil([&callbacks]() { return callbacks == 2; });
    require(rowFactoryCount == 1, "exact canonical encoding requests were not coalesced");
    require(imageProducerCount == 0,
            "row-backed canonical encoding materialized the image unnecessarily");
    require(firstBuffer != nullptr && firstBuffer == secondBuffer,
            "canonical encoding subscribers did not receive the same immutable buffer");
}

void encodingFailureFansOutOnce() {
    std::atomic_int rowFactoryCount = 0;
    ScreenshotExportArtifact artifact(ScreenshotExportSource::fromProducer(
        {}, [&rowFactoryCount](std::function<bool()> cancellation) {
            ++rowFactoryCount;
            ScreenshotImageRowSource rows;
            rows.size = QSize(16, 8);
            rows.cancellationRequested = std::move(cancellation);
            rows.readRows = [](int, int, qsizetype, uchar*, qsizetype) { return false; };
            return rows;
        }));
    QObject receiver;
    int callbacks = 0;
    const auto completion = [&callbacks](ScreenshotExportEncodingResult result) {
        require(!result.succeeded() && !result.error.isEmpty(),
                "failed encoding was reported as successful");
        ++callbacks;
    };
    require(artifact.requestCanonicalPng(&receiver, completion) &&
                artifact.requestCanonicalPng(&receiver, completion),
            "failed canonical encoding requests were rejected prematurely");
    require(artifact.requestClipboard(&receiver,
                                      [&](ScreenshotExportClipboardResult result) {
                                          require(
                                              !result.succeeded(),
                                              "failed PNG encoding produced a clipboard payload");
                                          ++callbacks;
                                      }),
            "clipboard failure request rejected");
    processUntil([&callbacks]() { return callbacks == 3; });
    require(rowFactoryCount == 1, "encoding failure was recomputed for each subscriber");
}

void rowRequestsCoalesceAndReuseBackingImage() {
    const QImage image = testImage();
    std::atomic_int imageCalls = 0;
    ScreenshotExportArtifact artifact(ScreenshotExportSource::fromProducer(
        [&imageCalls, image](const ScreenshotExportCancellation&) {
            ++imageCalls;
            return image;
        }));
    QObject firstReceiver;
    QObject secondReceiver;
    int callbacks = 0;
    const uchar* firstPixels = nullptr;
    const uchar* secondPixels = nullptr;
    require(artifact.requestRowSource(&firstReceiver,
                                      [&](ScreenshotImageRowSource source, QString error) {
                                          require(error.isEmpty() && source.isValid() &&
                                                      !source.backingImage.isNull(),
                                                  "first cached row request failed");
                                          firstPixels = source.backingImage.constBits();
                                          ++callbacks;
                                      }) &&
                artifact.requestRowSource(&secondReceiver,
                                          [&](ScreenshotImageRowSource source, QString error) {
                                              require(error.isEmpty() && source.isValid() &&
                                                          !source.backingImage.isNull(),
                                                      "second cached row request failed");
                                              secondPixels = source.backingImage.constBits();
                                              ++callbacks;
                                          }),
            "concurrent row requests were rejected");
    processUntil([&] { return callbacks == 2; });
    require(imageCalls == 1 && firstPixels == image.constBits() && firstPixels == secondPixels,
            "compatible row requests must reuse the original immutable backing image");

    require(artifact.requestRowSource(
                &firstReceiver,
                [&](ScreenshotImageRowSource source, QString error) {
                    require(error.isEmpty() && source.backingImage.constBits() == firstPixels,
                            "ready row request did not reuse the cached source");
                    ++callbacks;
                }),
            "ready row request was rejected");
    require(callbacks == 3 && imageCalls == 1, "ready row result was recomputed");
}

void convertedRowSourcesRetainOriginalPixelsWithoutMaterializing() {
    using snow::test_support::virtualMemoryMapped;
    for (const auto format :
         {QImage::Format_ARGB32_Premultiplied, QImage::Format_RGBA64_Premultiplied}) {
        QImage image = snowCanvasAllocateImage(QSize(1031, 517), format);
        require(!image.isNull(), "converted row lifetime fixture must allocate");
        image.fill(QColor(31, 117, 211, 127));
        image.setPixelColor(0, 0, QColor(249, 3, 61, 0));
        image.setPixelColor(1030, 516, QColor(7, 247, 81, 253));
        image.setColorSpace(QColorSpace::DisplayP3);
        const QImage expected = image.copy()
                                    .convertedToColorSpace(QColorSpace::SRgb)
                                    .convertToFormat(QImage::Format_RGBA8888);
        const uchar* middle = image.constBits() + image.sizeInBytes() / 2;
        auto rows = snow_shot::image_codec::srgbRowSource(image);
        require(rows.isValid() && rows.backingImage.isNull(),
                "converted row sources must avoid a full converted backing raster");
        image.fill(Qt::red);
        image = {};
        require(virtualMemoryMapped(middle),
                "lazy converted rows must own the original immutable source after caller release");
        QImage actual(rows.size, QImage::Format_RGBA8888);
        for (int first = 0; first < rows.size.height(); first += 37) {
            const int count = qMin(37, rows.size.height() - first);
            require(rows.readRows(first, count, actual.bytesPerLine(), actual.scanLine(first),
                                  actual.sizeInBytes() - first * actual.bytesPerLine()),
                    "lazy color conversion must support independent row ranges");
        }
        require(
            hasSamePixels(actual, expected),
            "lazy converted rows must retain Qt color and alpha rounding after source mutation");
        const QByteArray png = snow_shot::image_codec::encodePng(rows, 0);
        require(!png.isEmpty() && hasSamePixels(QImage::fromData(png, "PNG"), expected),
                "encoding lazy converted rows must preserve the expected sRGB pixels");
        auto copy = rows;
        rows = {};
        require(virtualMemoryMapped(middle), "copying a row callback must retain source ownership");
        copy = {};
        require(!virtualMemoryMapped(middle),
                "last converted row callback release must return original managed pages");
    }
    QImage untagged = testImage();
    untagged.setColorSpace({});
    const auto untaggedRows = snow_shot::image_codec::srgbRowSource(untagged);
    require(untaggedRows.backingImage.constBits() == untagged.constBits(),
            "untagged RGBA8 pixels assumed to be sRGB must not detach for metadata retagging");
    QImage actual(untagged.size(), QImage::Format_RGBA8888);
    require(untaggedRows.readRows(0, actual.height(), actual.bytesPerLine(), actual.bits(),
                                  actual.sizeInBytes()) &&
                hasSamePixels(actual, untagged),
            "untagged compatible rows must preserve every source pixel");
    require(!snow_shot::image_codec::srgbRowSource({}).isValid(),
            "null images must not create valid lazy row sources");
}

void lazyResizeRowsAndRegionsPreserveFullImagePixels() {
    for (const auto format : {QImage::Format_RGBA8888, QImage::Format_ARGB32_Premultiplied,
                              QImage::Format_RGBA64_Premultiplied, QImage::Format_Mono}) {
        QImage original(QSize(33, 257), format);
        if (format == QImage::Format_Mono)
            original.setColorTable({qRgb(30, 20, 80), qRgb(210, 220, 100)});
        for (int y = 0; y < original.height(); ++y)
            for (int x = 0; x < original.width(); ++x) {
                if (format == QImage::Format_Mono)
                    original.setPixel(x, y, (x + y) & 1);
                else
                    original.setPixelColor(x, y,
                                           QColor((x * 31 + y) % 256, (y * 17 + x) % 256,
                                                  (x * 7 + y * 11) % 256,
                                                  (x + y) % 5 == 0 ? 79 : 255));
            }
        original.setColorSpace(QColorSpace::DisplayP3);
        const auto originalPalette = original.colorTable();
        const auto source = snow_shot::image_codec::srgbRowSource(original);
        // Qt's const indexed color transform mutates its shallow source palette.
        // Give the full-image reference independent metadata and retain its
        // precision/alpha representation through color conversion.
        const QImage converted = original.copy()
                                     .convertedToColorSpace(QColorSpace::SRgb)
                                     .convertToFormat(QImage::Format_RGBA8888);
        const QRect sourceRegion(7, 13, 19, 9);
        QImage cropped(sourceRegion.size(), QImage::Format_RGBA8888);
        const bool regionRead =
            source.readRegion && source.readRegion(sourceRegion, cropped.bytesPerLine(),
                                                   cropped.bits(), cropped.sizeInBytes());
        const auto expectedCrop = converted.copy(sourceRegion);
        if (!regionRead || !hasSamePixels(cropped, expectedCrop)) {
            std::cerr << "sRGB ROI mismatch: format=" << int(format) << " read=" << regionRead
                      << " converted=" << converted.width() << 'x' << converted.height()
                      << " expectedCrop=" << expectedCrop.width() << 'x' << expectedCrop.height()
                      << '\n';
            for (int y = 0; regionRead && y < cropped.height() && y < expectedCrop.height(); ++y) {
                bool found = false;
                for (int x = 0; x < cropped.width() && x < expectedCrop.width(); ++x) {
                    const auto actual = cropped.pixelColor(x, y);
                    const auto expected = expectedCrop.pixelColor(x, y);
                    if (actual == expected)
                        continue;
                    std::cerr << "source=" << sourceRegion.x() + x << ',' << sourceRegion.y() + y
                              << " actual=" << actual.red() << ',' << actual.green() << ','
                              << actual.blue() << ',' << actual.alpha()
                              << " expected=" << expected.red() << ',' << expected.green() << ','
                              << expected.blue() << ',' << expected.alpha() << '\n';
                    found = true;
                    break;
                }
                if (found)
                    break;
            }
        }
        require(regionRead && hasSamePixels(cropped, expectedCrop),
                "sRGB ROI rows preserve ICC/alpha/mono rounding and nonzero crop coordinates");
        require(!source.readRegion(QRect(-1, 0, 1, 1), 4, cropped.bits(), cropped.sizeInBytes()) &&
                    !source.readRegion(sourceRegion, 1, cropped.bits(), cropped.sizeInBytes()) &&
                    !source.readRegion(sourceRegion, sourceRegion.width() * 4,
                                       const_cast<uchar*>(original.constBits()),
                                       original.sizeInBytes()),
                "sRGB ROI callbacks reject invalid ranges, stride and source overlap");

        std::atomic_bool concurrentPixelsMatch = true;
        const auto copiedSource = source;
        const auto readConcurrently = [&](bool regions) {
            QImage tile(QSize(19, 1), QImage::Format_RGBA8888);
            QImage row(QSize(33, 1), QImage::Format_RGBA8888);
            if (tile.isNull() || row.isNull()) {
                concurrentPixelsMatch = false;
                return;
            }
            for (int index = 0; index < 12; ++index) {
                const int y = index * 17;
                if (regions) {
                    if (!copiedSource.readRegion(QRect(7, y, 19, 1), tile.bytesPerLine(),
                                                 tile.bits(), tile.sizeInBytes()) ||
                        std::memcmp(tile.constBits(), converted.constScanLine(y) + 7 * 4, 19 * 4) !=
                            0)
                        concurrentPixelsMatch = false;
                } else if (!source.readRows(y, 1, row.bytesPerLine(), row.bits(),
                                            row.sizeInBytes()) ||
                           std::memcmp(row.constBits(), converted.constScanLine(y), 33 * 4) != 0)
                    concurrentPixelsMatch = false;
            }
        };
        std::thread rowsThread(readConcurrently, false);
        std::thread regionsThread(readConcurrently, true);
        rowsThread.join();
        regionsThread.join();
        require(concurrentPixelsMatch,
                "copied row and ROI callbacks share a synchronized bounded conversion reader");

        const QSize output(13, 193);
        QImage reference(output, QImage::Format_RGBA8888);
        QString error;
        require(snow_shot::image_codec::resizeToRgba8(source, output, reference.bits(),
                                                      reference.bytesPerLine(),
                                                      reference.sizeInBytes(), &error),
                "produce full resize reference");
        auto lazy = snow_shot::image_codec::resizedRowSource(source, output, &error);
        require(lazy.isValid() && lazy.backingImage.isNull() && lazy.readRegion && error.isEmpty(),
                "lazy resizing prepares row filters without a complete output raster");
        require(original.colorTable() == originalPalette,
                "reference and sRGB ROI/resize reads must preserve immutable source palettes");
        original.fill(Qt::red);
        QImage actual(output, QImage::Format_RGBA8888);
        for (int first = output.height() - 1; first >= 0; --first)
            require(lazy.readRows(first, 1, actual.bytesPerLine(), actual.scanLine(first),
                                  actual.sizeInBytes() - first * actual.bytesPerLine()),
                    "lazy resize supports reverse-order reads through its bounded tile cache");
        require(
            hasSamePixels(actual, reference),
            "lazy resize rows must be bit-identical to full-image resize after caller mutation");
        const QRect region(3, 71, 7, 11);
        QImage tile(region.size(), QImage::Format_RGBA8888);
        require(lazy.readRegion(region, tile.bytesPerLine(), tile.bits(), tile.sizeInBytes()) &&
                    hasSamePixels(tile, reference.copy(region)),
                "lazy resize ROI uses global filter coordinates");
        const auto resources = snow_shot::image_codec::resizeResources(source.size, output);
        const auto synchronous = snow_shot::image_codec::resizeWorkingBytes(source.size, output);
        require(resources && synchronous && resources->retainedBytes > 0 &&
                    *synchronous <= resources->retainedBytes + resources->scratchBytes,
                "resize admission separates retained plan/cache bytes from shared read scratch");
    }
    QImage opaque(QSize(19, 17), QImage::Format_RGB32);
    opaque.fill(Qt::green);
    auto source = snow_shot::image_codec::srgbRowSource(opaque);
    require(source.isKnownOpaque &&
                !snow_shot::image_codec::srgbRowSource(testImage()).isKnownOpaque,
            "source opacity hints follow structural alpha rather than assuming RGBA is opaque");
    bool cancelled = false;
    source.cancellationRequested = [&] { return cancelled; };
    int reservations = 0;
    source.reserveResources = [&](qint64, qint64, QString*) -> std::shared_ptr<void> {
        ++reservations;
        return std::make_shared<int>(1);
    };
    auto lazy = snow_shot::image_codec::resizedRowSource(source, QSize(11, 9));
    require(lazy.isValid() && lazy.isKnownOpaque && lazy.reserveResources && reservations == 0,
            "lazy rows propagate admission/opacity contracts without double reservation");
    auto rowOnly = source;
    rowOnly.backingImage = {};
    auto rowOnlyResize = snow_shot::image_codec::resizedRowSource(rowOnly, QSize(11, 9));
    auto* wrappedDestination =
        reinterpret_cast<uchar*>(std::numeric_limits<std::uintptr_t>::max() - 7U);
    require(rowOnlyResize.isValid() && !rowOnlyResize.readRows(0, 1, 44, wrappedDestination, 44),
            "row-only lazy resize rejects a wrapped destination before reading source pixels");
    cancelled = true;
    QImage output(lazy.size, QImage::Format_RGBA8888);
    require(!lazy.readRows(0, output.height(), output.bytesPerLine(), output.bits(),
                           output.sizeInBytes()) &&
                !snow_shot::image_codec::resizedRowSource(source, QSize(7, 5)).isValid(),
            "lazy resize honors cancellation during both reads and preparation");
}

void translucentWideGamutRowsKeepStraightAlphaThroughResizeAndEncoding() {
    for (const auto format :
         {QImage::Format_ARGB32_Premultiplied, QImage::Format_RGBA64_Premultiplied}) {
        QImage original(QSize(19, 11), format);
        original.fill(QColor(190, 40, 20, 110));
        original.setColorSpace(QColorSpace::DisplayP3);
        const QImage reference = original.copy()
                                     .convertedToColorSpace(QColorSpace::SRgb)
                                     .convertToFormat(QImage::Format_RGBA8888);
        const QColor expected = reference.pixelColor(0, 0);
        require(expected.alpha() == 110 && expected.red() > expected.alpha(),
                "translucent P3 fixture distinguishes straight red from premultiplied red");
        const auto source = snow_shot::image_codec::srgbRowSource(original);
        QImage rows(source.size, QImage::Format_RGBA8888);
        require(
            source.readRows(0, rows.height(), rows.bytesPerLine(), rows.bits(),
                            rows.sizeInBytes()) &&
                hasSamePixels(rows, reference),
            "P3 row conversion publishes straight alpha channels without double premultiplication");
        const auto png = snow_shot::image_codec::encodePng(source, 0);
        require(!png.isEmpty() && hasSamePixels(QImage::fromData(png, "PNG"), reference),
                "P3 row encoding preserves straight translucent RGB and alpha");

        const QSize target(7, 5);
        QImage expectedResize(target, QImage::Format_RGBA8888);
        expectedResize.fill(expected);
        QImage actual(target, QImage::Format_RGBA8888);
        require(snow_shot::image_codec::resizeToRgba8(
                    source, target, actual.bits(), actual.bytesPerLine(), actual.sizeInBytes()) &&
                    hasSamePixels(actual, expectedResize),
                "native P3 resize keeps a flat translucent field's straight channels");
        const auto lazy = snow_shot::image_codec::resizedRowSource(source, target);
        require(lazy.isValid() &&
                    lazy.readRegion(QRect(QPoint(), target), actual.bytesPerLine(), actual.bits(),
                                    actual.sizeInBytes()) &&
                    hasSamePixels(actual, expectedResize),
                "lazy P3 resize ROI keeps straight alpha throughout filtering");
    }
}

void croppedRowsDoNotRejectDestinationsAfterTheirLastPixel() {
    for (const auto format : {QImage::Format_RGBA8888, QImage::Format_RGBA64_Premultiplied}) {
        constexpr int width = 7;
        constexpr int height = 3;
        constexpr qsizetype sourceStride = 128;
        QByteArray storage(sourceStride * height + width * height * 4, char(0));
        QImage source(reinterpret_cast<uchar*>(storage.data()), width, height, sourceStride,
                      format);
        source.setColorSpace(QColorSpace::SRgb);
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
                source.setPixelColor(x, y, QColor(20 + x * 10, 40 + y * 20, 80, 127));
        const auto expected = source.convertToFormat(QImage::Format_RGBA8888).copy();
        const qsizetype lastPixel = (height - 1) * sourceStride + width * source.depth() / 8;
        auto* destination = reinterpret_cast<uchar*>(storage.data()) + lastPixel;
        auto rows = snow_shot::image_codec::srgbRowSource(source);
        require(rows.readRows(0, height, width * 4, destination, width * height * 4),
                "cropped strided rows permit output after their last source pixel");
        QImage actual(destination, width, height, width * 4, QImage::Format_RGBA8888);
        require(hasSamePixels(actual, expected) &&
                    hasSamePixels(source.convertToFormat(QImage::Format_RGBA8888), expected),
                "adjacent output writes preserve every crop pixel and correct RGBA conversion");
    }
}

void pngCacheBudgetEvictsLeastRecentlyUsedResults() {
    const QImage image = testImage();
    const auto rows = snow_shot::image_codec::srgbRowSource(image);
    const auto low = snow_shot::image_codec::encodePng(rows, 0);
    const auto medium = snow_shot::image_codec::encodePng(
        rows, ScreenshotImageFileService::encodeOptions(
                  ScreenshotImageFileFormat::Png,
                  ScreenshotImageEncodingOptions{100, ScreenshotCompressionLevel::Medium})
                  .compression_level);
    const auto high = snow_shot::image_codec::encodePng(rows, 9);
    ScreenshotExportArtifact artifact(ScreenshotExportSource::fromImage(image),
                                      ScreenshotCompressionLevel::Low,
                                      ScreenshotExportArtifact::PngCachePolicy{
                                          low.size() + qMax(medium.size(), high.size()), {}});
    QObject receiver;
    auto request = [&](ScreenshotCompressionLevel level) {
        QByteArray result;
        require(artifact.requestPng(&receiver, level,
                                    [&](ScreenshotExportEncodingResult encoded) {
                                        require(encoded.succeeded(),
                                                "bounded cache encoding failed");
                                        result = encoded.image.bytes();
                                    }),
                "bounded cache request rejected");
        processUntil([&] { return !result.isEmpty(); });
        return result;
    };
    const auto first = request(ScreenshotCompressionLevel::Low);
    const auto second = request(ScreenshotCompressionLevel::Medium);
    require(artifact.cachedPng(ScreenshotCompressionLevel::Low).bytes().constData() ==
                first.constData(),
            "cached low PNG lost its buffer");
    const auto third = request(ScreenshotCompressionLevel::High);
    require(!artifact.cachedPng(ScreenshotCompressionLevel::Medium).isValid() &&
                artifact.cachedPng(ScreenshotCompressionLevel::Low).isValid() &&
                artifact.cachedPng(ScreenshotCompressionLevel::High).isValid(),
            "PNG cache did not evict the least recently used encoding");
    require(second == medium && third == high && first == low,
            "cache eviction invalidated an outstanding consumer's bytes");
    require(request(ScreenshotCompressionLevel::Medium) == medium,
            "evicted PNG could not be encoded again");
}

void cachedPngSavesReuseBytesBeyondRawCacheBudget() {
    QImage image(1024, 1024, QImage::Format_RGBA8888);
    image.fill(Qt::white);
    image.setColorSpace(QColorSpace::SRgb);
    QTemporaryDir directory;
    require(directory.isValid(), "cached PNG fixture directory unavailable");
    QObject receiver;
    for (const auto compression :
         {ScreenshotCompressionLevel::Medium, ScreenshotCompressionLevel::High}) {
        std::atomic_int rowReads = 0;
        std::atomic_int pngEncodings = 0;
        std::atomic_int fileEncodings = 0;
        auto rows = rowSourceFor(image, {});
        const auto read = rows.readRows;
        rows.readRows = [&, read](int first, int count, qsizetype stride, uchar* pixels,
                                  qsizetype capacity) {
            ++rowReads;
            return read(first, count, stride, pixels, capacity);
        };
        ScreenshotExportArtifact::PngCachePolicy policy;
        policy.maximumBytes = 16 * 1024;
        policy.encodingStarted = [&] { ++pngEncodings; };
        policy.fileEncodingStarted = [&](ScreenshotImageFileFormat) { ++fileEncodings; };
        ScreenshotExportArtifact artifact(
            ScreenshotExportSource::fromProducer({},
                                                 [rows](std::function<bool()> cancelled) {
                                                     auto source = rows;
                                                     source.cancellationRequested =
                                                         std::move(cancelled);
                                                     return source;
                                                 }),
            compression, policy);
        QByteArray canonical;
        require(artifact.requestCanonicalPng(&receiver,
                                             [&](ScreenshotExportEncodingResult result) {
                                                 require(result.succeeded(),
                                                         "canonical PNG fixture encoding failed");
                                                 canonical = result.image.bytes();
                                             }),
                "canonical PNG fixture request rejected");
        processUntil([&] { return !canonical.isEmpty(); });
        require(!artifact.shouldCachePng(image.size()) && artifact.cachedPng(compression).isValid(),
                "fixture must retain its PNG beyond the raw pixel budget");
        const int readsBeforeSave = rowReads.load();
        int completed = 0;
        const auto saved = [&](ScreenshotExportTaskResult result) {
            QFile file(result.savedPath);
            require(result.succeeded() && file.open(QIODevice::ReadOnly) &&
                        file.readAll() == canonical,
                    "cached PNG save changed its encoded bytes");
            require(result.encodedByteCount == canonical.size() &&
                        result.encodedSha256 ==
                            QCryptographicHash::hash(canonical, QCryptographicHash::Sha256).toHex(),
                    "cached PNG save lost its encoded digest");
            ++completed;
        };
        const ScreenshotImageEncodingOptions encoding{100, compression};
        require(artifact.requestSaveToPath(&receiver,
                                           directory.filePath(QStringLiteral("cached-manual.png")),
                                           ScreenshotImageFileFormat::Png, encoding, saved) &&
                    artifact.requestAutomaticSave(
                        &receiver, {directory.path()}, ScreenshotImageFileFormat::Png,
                        QStringLiteral("cached-automatic"), encoding, saved),
                "cached PNG save requests rejected");
        processUntil([&] { return completed == 2; });
        require(pngEncodings == 1 && fileEncodings == 0 && rowReads == readsBeforeSave,
                "PNG saves must reuse retained bytes before applying the raw pixel budget");
    }
}

void fileOutputsStreamBeyondCacheBudget() {
    const QImage image = testImage();
    QTemporaryDir directory;
    require(directory.isValid(), "streaming fixture directory unavailable");
    QObject receiver;
    for (auto compression : {ScreenshotCompressionLevel::Low, ScreenshotCompressionLevel::Medium,
                             ScreenshotCompressionLevel::High}) {
        ScreenshotExportArtifact artifact(ScreenshotExportSource::fromImage(image), compression,
                                          ScreenshotExportArtifact::PngCachePolicy{1, {}});
        int callbacks = 0;
        QString automaticPath;
        const QString manualPath = directory.filePath(QStringLiteral("manual.png"));
        require(artifact.requestSaveToPath(&receiver, manualPath, ScreenshotImageFileFormat::Png,
                                           ScreenshotImageEncodingOptions{100, compression},
                                           [&](ScreenshotExportTaskResult result) {
                                               require(result.succeeded(),
                                                       "streaming manual save failed");
                                               ++callbacks;
                                           }) &&
                    artifact.requestAutomaticSave(
                        &receiver, {directory.path()}, ScreenshotImageFileFormat::Png,
                        QStringLiteral("automatic"), {100, compression},
                        [&](ScreenshotExportTaskResult result) {
                            require(result.succeeded(), "streaming automatic save failed");
                            automaticPath = result.savedPath;
                            ++callbacks;
                        }),
                "streaming file request rejected");
        processUntil([&] { return callbacks == 2; });
        const int level =
            ScreenshotImageFileService::encodeOptions(
                ScreenshotImageFileFormat::Png, ScreenshotImageEncodingOptions{100, compression})
                .compression_level;
        const auto expected =
            snow_shot::image_codec::encodePng(snow_shot::image_codec::srgbRowSource(image), level);
        for (const auto& path : {manualPath, automaticPath}) {
            QFile file(path);
            require(file.open(QIODevice::ReadOnly) && file.readAll() == expected,
                    "streaming file changed the requested PNG encoding");
        }
        require(!artifact.cachedPng(compression).isValid(),
                "streaming file output populated the in-memory cache");
    }
}

void oversizedPngStillFansOutWithoutRetention() {
    ScreenshotExportArtifact artifact(ScreenshotExportSource::fromImage(testImage()),
                                      ScreenshotCompressionLevel::Low,
                                      ScreenshotExportArtifact::PngCachePolicy{1, {}});
    QObject receiver;
    int callbacks = 0;
    QByteArray first;
    QByteArray second;
    for (auto* bytes : {&first, &second}) {
        require(artifact.requestPng(&receiver, ScreenshotCompressionLevel::Low,
                                    [&, bytes](ScreenshotExportEncodingResult result) {
                                        require(result.succeeded(), "oversized PNG failed");
                                        *bytes = result.image.bytes();
                                        ++callbacks;
                                    }),
                "oversized PNG request rejected");
    }
    processUntil([&] { return callbacks == 2; });
    require(first.constData() == second.constData() &&
                !artifact.cachedPng(ScreenshotCompressionLevel::Low).isValid(),
            "oversized PNG was retained or concurrent requests did not share its bytes");
}

void rowRequestFailureAndCancellationFanOut() {
    std::atomic_bool failureEntered = false;
    std::atomic_bool releaseFailure = false;
    std::atomic_int factories = 0;
    ScreenshotExportArtifact failed(ScreenshotExportSource::fromProducer(
        {}, [&failureEntered, &releaseFailure, &factories](std::function<bool()>) {
            ++factories;
            failureEntered.store(true, std::memory_order_release);
            while (!releaseFailure.load(std::memory_order_acquire))
                QThread::msleep(1);
            return ScreenshotImageRowSource{};
        }));
    QObject firstReceiver;
    QObject secondReceiver;
    int failures = 0;
    const auto failedCallback = [&failures](ScreenshotImageRowSource source, QString error) {
        require(!source.isValid() && !error.isEmpty(),
                "failed row-source request produced an invalid result contract");
        ++failures;
    };
    require(failed.requestRowSource(&firstReceiver, failedCallback) &&
                failed.requestRowSource(&secondReceiver, failedCallback),
            "failed row-source subscribers were rejected");
    processUntil([&] { return failureEntered.load(std::memory_order_acquire); });
    releaseFailure.store(true, std::memory_order_release);
    processUntil([&] { return failures == 2; });
    require(factories == 1, "failed row-source subscribers did not share one factory call");

    std::atomic_bool cancellationEntered = false;
    std::atomic_bool cancellationObserved = false;
    ScreenshotExportArtifact cancelled(ScreenshotExportSource::fromProducer(
        {}, [&cancellationEntered, &cancellationObserved](std::function<bool()> cancellation) {
            cancellationEntered.store(true, std::memory_order_release);
            while (!(cancellation && cancellation()))
                QThread::msleep(1);
            cancellationObserved.store(true, std::memory_order_release);
            return ScreenshotImageRowSource{};
        }));
    int cancelledCallbacks = 0;
    require(cancelled.requestRowSource(&firstReceiver,
                                       [&cancelledCallbacks](ScreenshotImageRowSource, QString) {
                                           ++cancelledCallbacks;
                                       }) &&
                cancelled.requestRowSource(
                    &secondReceiver, [&cancelledCallbacks](ScreenshotImageRowSource,
                                                           QString) { ++cancelledCallbacks; }),
            "cancelled row-source subscribers were rejected");
    processUntil([&] { return cancellationEntered.load(std::memory_order_acquire); });
    cancelled.cancel();
    processUntil([&] { return cancellationObserved.load(std::memory_order_acquire); });
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    require(cancelledCallbacks == 0 && cancelled.isCancelled(),
            "cancelled row-source request delivered pending subscribers");
}

void cancellationSuppressesPendingCallbacks() {
    std::function<void(QImage)> finishLoad;
    ScreenshotExportArtifact artifact(ScreenshotExportSource::fromImageLoader(
        [&finishLoad](QObject*, std::function<void(QImage)> callback) {
            finishLoad = std::move(callback);
            return true;
        }));
    QObject receiver;
    int callbacks = 0;
    require(artifact.requestImage(&receiver,
                                  [&callbacks](ScreenshotExportImageResult) { ++callbacks; }),
            "cancellation image request was rejected");
    require(artifact.requestPng(&receiver, ScreenshotCompressionLevel::Low,
                                [&](ScreenshotExportEncodingResult) { ++callbacks; }) &&
                artifact.requestPng(&receiver, ScreenshotCompressionLevel::High,
                                    [&](ScreenshotExportEncodingResult) { ++callbacks; }) &&
                artifact.requestClipboard(&receiver,
                                          [&](ScreenshotExportClipboardResult) { ++callbacks; }),
            "pending PNG/clipboard cancellation requests were rejected");
    artifact.cancel();
    require(static_cast<bool>(finishLoad),
            "cancellation fixture did not retain its loader callback");
    finishLoad(testImage());
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    require(callbacks == 0 && artifact.isCancelled(),
            "cancelled artifact delivered a pending callback");
}

void cancelledEncodersRetainEveryConsumerLease() {
    QTemporaryDir directory;
    QObject receiver;
    for (const auto format : {ScreenshotImageFileFormat::Png, ScreenshotImageFileFormat::Bmp,
                              ScreenshotImageFileFormat::Jpeg}) {
        const auto entered = std::make_shared<std::atomic_bool>(false);
        const auto release = std::make_shared<std::atomic_bool>(false);
        const auto unblock = qScopeGuard([release] { release->store(true); });
        auto rows = rowSourceFor(testImage(), {});
        const auto read = rows.readRows;
        rows.readRows = [entered, release, read](int first, int count, qsizetype stride,
                                                 uchar* target, qsizetype capacity) {
            entered->store(true);
            while (!release->load())
                QThread::msleep(1);
            return read(first, count, stride, target, capacity);
        };
        auto artifact = std::make_unique<ScreenshotExportArtifact>(
            ScreenshotExportSource::fromProducer({}, [rows](std::function<bool()> cancellation) {
                auto source = rows;
                source.cancellationRequested = std::move(cancellation);
                return source;
            }));
        auto firstLease = std::make_shared<int>(1);
        auto secondLease = std::make_shared<int>(2);
        const std::weak_ptr<int> first = firstLease;
        const std::weak_ptr<int> second = secondLease;
        int callbacks = 0;
        const auto complete = [&](ScreenshotExportTaskResult) { ++callbacks; };
        require(artifact->requestSaveToPath(&receiver, directory.filePath(QStringLiteral("first")),
                                            format, {}, complete, {}, firstLease),
                "first leased encoding starts");
        processUntil([&] { return entered->load(); });
        require(artifact->requestSaveToPath(&receiver, directory.filePath(QStringLiteral("second")),
                                            format, {}, complete, {}, secondLease),
                "second leased consumer joins running encoding");
        firstLease.reset();
        secondLease.reset();
        artifact->cancel();
        artifact.reset();
        require(!first.expired() && !second.expired(),
                "cancelled encoder released a consumer lease before worker completion");
        release->store(true);
        processUntil([&] { return first.expired() && second.expired(); });
        require(callbacks == 0, "cancelled encoder delivered a consumer callback");
    }
}

void pinnedViewportSourceRendersExpectedPixels() {
    SnowCanvasRuntime runtime;
    const QByteArray session = runtime.serializeDocumentSession();
    require(!session.isEmpty(), "runtime session could not be serialized");
    QImage background(QSize(64, 48), QImage::Format_ARGB32_Premultiplied);
    background.fill(QColor(12, 24, 36, 255));
    ScreenshotPinnedViewportExportSource source{
        session, std::move(background), QRectF(0.0, 0.0, 64.0, 48.0), QSize(64, 48), {}, {}, 1.0,
    };
    ScreenshotExportArtifact artifact(
        ScreenshotExportSource::fromPinnedViewport(std::move(source)));
    require(!artifact.clipboardPlacement(),
            "pinned-window copies must not acquire screenshot placement");
    QObject receiver;
    QImage rendered;
    require(artifact.requestImage(&receiver,
                                  [&rendered](ScreenshotExportImageResult result) {
                                      require(result.succeeded(),
                                              "pinned viewport artifact render failed");
                                      rendered = std::move(result.image);
                                  }),
            "pinned viewport artifact render was not scheduled");
    processUntil([&rendered]() { return !rendered.isNull(); });
    require(rendered.size() == QSize(64, 48) &&
                rendered.pixelColor(rendered.width() / 2, rendered.height() / 2) ==
                    QColor(12, 24, 36, 255),
            "pinned viewport artifact did not return the rendered pixels");
}

void emptyPinnedSourceMatchesCanvasProjection() {
    SnowCanvasRuntime runtime;
    QImage background(37, 19, QImage::Format_RGBA8888);
    for (int y = 0; y < background.height(); ++y)
        for (int x = 0; x < background.width(); ++x)
            background.setPixelColor(x, y,
                                     QColor(x * 5, y * 11, (x + y) * 4, (x * 7 + y * 3) % 256));
    background.setColorSpace(QColorSpace::SRgb);
    QObject receiver;
    for (const QSize size : {QSize(37, 19), QSize(13, 7), QSize(74, 38)}) {
        const QRectF rect(-17.25, 4.5, 37.25, 19.75);
        QImage expected = runtime.renderToImage(rect, size, {{background, rect}});
        expected.setColorSpace(background.colorSpace());
        ScreenshotPinnedViewportExportSource source{{}, background, rect, size, {}, {}, 1.0};
        ScreenshotExportArtifact artifact(
            ScreenshotExportSource::fromPinnedViewport(std::move(source)));
        QImage rendered;
        require(artifact.requestImage(&receiver,
                                      [&](ScreenshotExportImageResult result) {
                                          require(result.succeeded(), "empty pinned export failed");
                                          rendered = result.image;
                                      }),
                "empty pinned export rejected");
        processUntil([&] { return !rendered.isNull(); });
        require(hasSamePixels(rendered, expected) && rendered.colorSpace() == expected.colorSpace(),
                "empty document fast path changed projection, alpha, or color metadata");
    }
}

void pinnedViewportExportsReleaseImportedSnapshots() {
    QTemporaryDir directory;
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    require(directory.isValid() &&
                storage.initialize({directory.filePath(QStringLiteral("bin")), directory.path(), 0})
                    .success,
            "snapshot lifetime fixture initializes isolated storage on the main thread");
    const auto cleanup = qScopeGuard([&] { storage.shutdown(); });
    for (const bool serializedDocument : {false, true}) {
        std::weak_ptr<const SnowCanvasSmartEraseSnapshot::Data> snapshot;
        QObject receiver;
        int completed = 0;
        {
            SnowCanvasRuntime runtime;
            auto smartErase = runtime.smartEraseSnapshot();
            require(smartErase.data != nullptr, "snapshot lifetime fixture must own data");
            snapshot = smartErase.data;
            QImage background(64, 48, QImage::Format_ARGB32_Premultiplied);
            background.fill(Qt::green);
            ScreenshotPinnedViewportExportSource source{
                serializedDocument ? runtime.serializeDocumentSession() : QByteArray{},
                std::move(background),
                QRectF(0, 0, 64, 48),
                QSize(64, 48),
                {},
                std::move(smartErase),
                1.0,
            };
            const auto exportSource = ScreenshotExportSource::fromPinnedViewport(std::move(source));
            ScreenshotExportArtifact artifact(exportSource);
            ScreenshotExportArtifact otherArtifact(exportSource);
            QImage firstImage;
            QImage secondImage;
            require(artifact.requestImage(&receiver,
                                          [&](ScreenshotExportImageResult result) {
                                              require(result.succeeded(),
                                                      "lifetime export succeeds");
                                              firstImage = std::move(result.image);
                                              ++completed;
                                          }),
                    "snapshot lifetime export is scheduled");
            require(otherArtifact.requestImage(&receiver,
                                               [&](ScreenshotExportImageResult result) {
                                                   require(result.succeeded(),
                                                           "copied pinned source succeeds");
                                                   secondImage = std::move(result.image);
                                                   ++completed;
                                               }),
                    "copied pinned source is scheduled");
            processUntil([&] { return completed == 2; });
            require(firstImage.constBits() == secondImage.constBits(),
                    "copied pinned sources share immutable prepared pixels");
            processUntil([&] { return snapshot.expired(); });
        }
        processUntil([&] { return snapshot.expired(); });
        require(ScreenshotExportCoordinator::shared().pendingJobCount() == 0,
                "completed exports release snapshots while the shared pool remains available");
    }
}

void quickSaveUsesOnlyConfiguredOutput() {
    QTemporaryDir directory;
    require(directory.isValid(), "quick save fixture unavailable");
    const QString executable = directory.filePath(QStringLiteral("bin"));
    require(QDir().mkpath(executable), "quick save fixture bin unavailable");
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    require(storage.initialize({executable, directory.path(), 60000}).success,
            "quick save isolated settings unavailable");
    const snow_shot::storage::ScreenshotSettings settings;
    const QString output = directory.filePath(QStringLiteral("new/nested"));
    require(settings.setImageSaveDirectory(output) &&
                settings.setAutoSaveFilenameFormat(QStringLiteral("Quick_output")) &&
                settings.setLastManualSaveDirectory(directory.path()) &&
                settings.setLastManualSaveFormat(QStringLiteral("jpeg")) &&
                settings.setCompressionLevel(QStringLiteral("high")) && settings.setImageQuality(0),
            "quick save settings setup failed");
    require(settings.setPdfPageSize(QStringLiteral("a4_landscape")),
            "PDF page setting must persist");
    QObject receiver;
    const QImage image = testImage();
    const auto save = [&](ScreenshotExportArtifact& artifact) {
        std::optional<ScreenshotExportTaskResult> result;
        require(
            artifact.requestQuickSave(
                &receiver, [&](ScreenshotExportTaskResult saved) { result = std::move(saved); }),
            "quick save request was rejected");
        processUntil([&] { return result.has_value(); });
        return *result;
    };
    for (const QString& format :
         {QStringLiteral("png"), QStringLiteral("jpeg"), QStringLiteral("bmp"),
          QStringLiteral("webp"), QStringLiteral("jxl"), QStringLiteral("avif"),
          QStringLiteral("pdf")}) {
        require(settings.setImageFormat(format), "quick save format setup failed");
        ScreenshotExportArtifact artifact(ScreenshotExportSource::fromImage(image),
                                          ScreenshotCompressionLevel::Low);
        const auto first = save(artifact);
        require(first.succeeded() && QFileInfo(first.savedPath).absolutePath() == output &&
                    QFileInfo(first.savedPath).baseName() == QStringLiteral("Quick_output") &&
                    !first.savedPath.contains(QLatin1Char('{')) &&
                    QFileInfo(first.savedPath).suffix() ==
                        ScreenshotImageFileService::extension(
                            ScreenshotImageFileService::formatForKey(format)),
                "quick save must create the configured directory and use format/name settings");
        const auto second = save(artifact);
        require(second.succeeded() && first.savedPath != second.savedPath &&
                    QFileInfo(second.savedPath).baseName().endsWith(QStringLiteral("_1")) &&
                    QFileInfo::exists(first.savedPath),
                "quick save must preserve existing files on collision");
        if (format == QStringLiteral("pdf")) {
            QFile file(first.savedPath);
            require(file.open(QIODevice::ReadOnly), "quick save PDF must be readable");
            const QByteArray pdf = file.readAll();
            require(pdf.startsWith("%PDF-1.7") &&
                        pdf.contains("/MediaBox [0 0 841.88976378 595.27559055]") &&
                        pdf.contains("/DCTDecode"),
                    "quick save must honor the PDF page size and configured quality");
        }
        if (format == QStringLiteral("png")) {
            QFile file(first.savedPath);
            require(file.open(QIODevice::ReadOnly) &&
                        file.readAll() == snow_shot::image_codec::encodePng(image, 9) &&
                        QImage(first.savedPath).convertToFormat(QImage::Format_RGBA8888) == image,
                    "quick save must preserve pixels and honor global PNG compression");
        }
        if (format == QStringLiteral("jpeg") || format == QStringLiteral("webp") ||
            format == QStringLiteral("jxl") || format == QStringLiteral("avif")) {
            const auto fileFormat = ScreenshotImageFileService::formatForKey(format);
            const QString referencePath = directory.filePath(
                QStringLiteral("reference.") + ScreenshotImageFileService::extension(fileFormat));
            require(ScreenshotImageFileService::write(
                        image, referencePath, fileFormat, {}, {},
                        ScreenshotImageEncodingOptions{0, ScreenshotCompressionLevel::High})
                        .succeeded(),
                    "quick-save reference could not be encoded");
            QFile saved(first.savedPath);
            QFile reference(referencePath);
            require(saved.open(QIODevice::ReadOnly) && reference.open(QIODevice::ReadOnly) &&
                        saved.readAll() == reference.readAll(),
                    "quick save must honor the configured image quality");
        }
    }
    require(settings.lastManualSaveDirectory() == directory.path() &&
                settings.lastManualSaveFormat() == QStringLiteral("jpeg"),
            "quick save must not update manual-save settings");

    // Hold source acquisition so both saves remain pending across a settings change.
    {
        std::function<void(QImage)> completeImage;
        ScreenshotExportArtifact pending(ScreenshotExportSource::fromImageLoader(
            [&](QObject*, std::function<void(QImage)> callback) {
                completeImage = std::move(callback);
                return true;
            }));
        std::optional<ScreenshotExportTaskResult> first;
        std::optional<ScreenshotExportTaskResult> second;
        require(settings.setImageFormat(QStringLiteral("jpeg")) && settings.setImageQuality(35) &&
                    settings.setAutoSaveFilenameFormat(QStringLiteral("Pending_first")),
                "pending quick-save settings setup failed");
        require(
            pending.requestQuickSave(
                &receiver, [&](ScreenshotExportTaskResult result) { first = std::move(result); }),
            "first pending quick save was rejected");
        require(settings.setImageQuality(85) &&
                    settings.setAutoSaveFilenameFormat(QStringLiteral("Pending_second")),
                "pending quick-save settings change failed");
        require(
            pending.requestQuickSave(
                &receiver, [&](ScreenshotExportTaskResult result) { second = std::move(result); }),
            "second pending quick save was rejected");
        processUntil([&] { return static_cast<bool>(completeImage); });
        require(!first && !second, "pending quick saves completed without their source");
        completeImage(image);
        processUntil([&] { return first.has_value() && second.has_value(); });
        require(first->succeeded() && second->succeeded(), "pending quick save failed");
        QByteArray firstBytes;
        for (bool isFirst : {true, false}) {
            const auto& result = isFirst ? *first : *second;
            const QString expectedName =
                isFirst ? QStringLiteral("Pending_first") : QStringLiteral("Pending_second");
            require(QFileInfo(result.savedPath).baseName() == expectedName,
                    "pending quick save lost its filename settings");
            const QString referencePath =
                directory.filePath(QStringLiteral("pending-reference.jpg"));
            require(ScreenshotImageFileService::write(
                        image, referencePath, ScreenshotImageFileFormat::Jpeg, {}, {},
                        {isFirst ? 35 : 85, ScreenshotCompressionLevel::High})
                        .succeeded(),
                    "pending quick-save reference failed");
            QFile saved(result.savedPath);
            QFile reference(referencePath);
            require(saved.open(QIODevice::ReadOnly) && reference.open(QIODevice::ReadOnly),
                    "pending quick-save files are unreadable");
            const QByteArray bytes = saved.readAll();
            require(bytes == reference.readAll(), "pending quick save lost its requested quality");
            if (isFirst)
                firstBytes = bytes;
            else
                require(bytes != firstBytes,
                        "different requested qualities produced the same file");
        }
        require(settings.setAutoSaveFilenameFormat(QStringLiteral("Quick_output")),
                "quick-save filename restore failed");
    }

    require(settings.setImageFormat(QStringLiteral("png")), "streaming format setup failed");
    ScreenshotExportArtifact rows(
        ScreenshotExportSource::fromProducer({}, [image](std::function<bool()> cancellation) {
            return rowSourceFor(image, std::move(cancellation));
        }));
    const auto rowSave = save(rows);
    require(rowSave.succeeded() &&
                QImage(rowSave.savedPath).convertToFormat(QImage::Format_RGBA8888) == image,
            "row-backed quick-save must export the complete source without losing pixels");
    // Fill the bounded queue while retaining already encoded pixels so rejection tests the save
    // job.
    processUntil([] { return ScreenshotExportCoordinator::shared().pendingJobCount() == 0; });
    auto release = std::make_shared<std::atomic_bool>(false);
    std::vector<ScreenshotExportJobHandle> blockers;
    for (int i = 0; i < 64; ++i) {
        auto job = ScreenshotExportCoordinator::shared().submit(
            &receiver, ScreenshotExportCoordinator::Priority::Background,
            [release](const ScreenshotExportCancellation& cancellation) {
                while (!release->load() && !cancellation.isCancellationRequested())
                    QThread::msleep(1);
                return ScreenshotExportTaskResult{};
            },
            [](ScreenshotExportTaskResult) {});
        if (!job.isValid())
            break;
        blockers.push_back(job);
    }
    std::optional<ScreenshotExportTaskResult> rejected;
    const bool accepted = rows.requestQuickSave(
        &receiver, [&](ScreenshotExportTaskResult result) { rejected = std::move(result); });
    release->store(true);
    processUntil([] { return ScreenshotExportCoordinator::shared().pendingJobCount() == 0; });
    require(accepted && rejected && !rejected->succeeded() &&
                rejected->failureStage == ScreenshotExportFailureStage::Queue,
            "a full export queue must report a save failure");
    require(save(rows).succeeded(), "a rejected quick-save must allow retry");
    QFile blocker(directory.filePath(QStringLiteral("blocked")));
    require(blocker.open(QIODevice::WriteOnly) && blocker.write("unchanged") == 9,
            "quick save failure fixture unavailable");
    blocker.close();
    for (const QString& invalid : {QString(), blocker.fileName() + QStringLiteral("/child")}) {
        require(settings.setImageSaveDirectory(invalid), "invalid directory setup failed");
        ScreenshotExportArtifact artifact(ScreenshotExportSource::fromImage(image));
        const auto failed = save(artifact);
        require(!failed.succeeded() && failed.savedPath.isEmpty() && !failed.error.isEmpty(),
                "an unusable configured directory must fail without saving to a fallback");
    }
    ScreenshotExportArtifact cancelled(ScreenshotExportSource::fromImage(image));
    int callbacks = 0;
    require(cancelled.requestQuickSave(&receiver, [&](ScreenshotExportTaskResult) { ++callbacks; }),
            "empty-directory request was rejected");
    cancelled.cancel();
    QCoreApplication::processEvents();
    require(callbacks == 0 &&
                !cancelled.requestQuickSave(&receiver, [](ScreenshotExportTaskResult) {}),
            "cancelled quick saves must reject new requests and suppress pending callbacks");
    storage.shutdown();
}
} // namespace

void clipboardPlacementIsAnImmutableArtifactProperty() {
    ScreenshotClipboardPlacement placement;
    placement.placement = {QStringLiteral("display"), {}, QPointF(50, 60), QSize(20, 10)};
    placement.windowRect = QRect(50, 60, 20, 10);
    placement.rasterSize = QSize(40, 20);
    placement.displays = {{QStringLiteral("display"),
                           {},
                           QRect(0, 0, 200, 200),
                           QRect(0, 0, 200, 200),
                           QRect(0, 0, 200, 200),
                           1}};
    QImage pixels(40, 20, QImage::Format_ARGB32);
    pixels.fill(Qt::red);
    ScreenshotClipboardAppearance appearance;
    appearance.rasterSize = pixels.size();
    appearance.borderAppearance =
        snow_shot::storage::PinnedBorderAppearance{QSize(20, 10), QRectF(2, 2, 16, 6), 2, true, {}};
    appearance.showBorder = false;
    ScreenshotExportArtifact artifact(
        ScreenshotExportSource::fromImage(pixels, placement, appearance),
        ScreenshotCompressionLevel::Low);
    appearance.checkerboardEnabled = true;
    placement.windowRect.moveTopLeft(QPoint(100, 110));
    QObject receiver;
    bool completed = false;
    require(artifact.requestClipboard(
                &receiver,
                [&](ScreenshotExportClipboardResult result) {
                    require(result.succeeded(), "artifact clipboard preparation must succeed");
                    const auto metadata =
                        decodeScreenshotClipboardPlacement(result.payload.placementBytes());
                    require(metadata && metadata->windowRect == QRect(50, 60, 20, 10) &&
                                metadata->rasterSize == pixels.size(),
                            "artifact observes later caller placement");
                    const auto copiedAppearance =
                        decodeScreenshotClipboardAppearance(result.payload.appearanceBytes());
                    require(copiedAppearance && !copiedAppearance->checkerboardEnabled &&
                                copiedAppearance->rasterSize == pixels.size() &&
                                copiedAppearance->showBorder == false &&
                                copiedAppearance->borderAppearance == appearance.borderAppearance,
                            "artifact observes later caller appearance");
                    completed = true;
                }),
            "artifact clipboard request must schedule");
    processUntil([&] { return completed; });
    QTemporaryDir directory;
    bool saved = false;
    require(artifact.requestSaveToPath(
                &receiver, directory.filePath(QStringLiteral("saved.png")),
                ScreenshotImageFileFormat::Png,
                ScreenshotImageEncodingOptions{100, ScreenshotCompressionLevel::Low},
                [&](ScreenshotExportTaskResult result) {
                    require(result.succeeded(), "metadata artifact file save must succeed");
                    const QFileInfo info(result.savedPath);
                    QMimeData mime;
                    artifact.setClipboardFileMetadata(mime, result.savedPath);
                    const auto metadata = readScreenshotClipboardPlacement(&mime);
                    const auto fileAppearance = readScreenshotClipboardAppearance(&mime);
                    require(
                        metadata && metadata->windowRect == QRect(50, 60, 20, 10) &&
                            metadata->matchesFile(result.savedPath, info.size(),
                                                  info.lastModified().toUTC().toMSecsSinceEpoch()),
                        "file-copy save loses the original artifact placement");
                    require(fileAppearance &&
                                fileAppearance->borderAppearance == appearance.borderAppearance &&
                                fileAppearance->rasterSize == pixels.size() &&
                                fileAppearance->matchesFile(
                                    result.savedPath, info.size(),
                                    info.lastModified().toUTC().toMSecsSinceEpoch()),
                            "file-copy save loses the original artifact appearance");
                    saved = true;
                }),
            "metadata artifact file save must schedule");
    processUntil([&] { return saved; });
    require(artifact.clipboardPlacement()->windowRect == QRect(50, 60, 20, 10),
            "file-save subscriber must share the same immutable placement");
    ScreenshotExportArtifact imported(ScreenshotExportSource::fromImage(pixels),
                                      ScreenshotCompressionLevel::Low);
    require(!imported.clipboardPlacement() && !imported.clipboardAppearance(),
            "ordinary image artifacts invent clipboard metadata");
}

void boundedExportRowsTests();
int runExportRowBenchmark(const QStringList& arguments);

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    if (application.arguments().contains(QStringLiteral("--row-export-benchmark")))
        return runExportRowBenchmark(application.arguments());
    try {
        QTemporaryDir logs;
        snow_shot::diagnostics::DiagnosticsOptions logging;
        logging.directories = {QFileInfo(logs.path()).canonicalFilePath()};
        logging.enableCrashCapture = false;
        logging.mirrorToConsole = false;
        auto& diagnostics = snow_shot::diagnostics::DiagnosticsService::instance();
        require(diagnostics.initialize(logging), "export diagnostics must initialize");
        if (application.arguments().contains(QStringLiteral("--window-variables-only"))) {
            windowNamesSurviveAsynchronousFileExports();
            diagnostics.shutdown();
            return EXIT_SUCCESS;
        }
        clipboardPlacementIsAnImmutableArtifactProperty();
        {
            QObject receiver;
            ScreenshotExportArtifact artifact(ScreenshotExportSource::fromImageLoader(
                [](QObject*, std::function<void(QImage)> callback) {
                    callback({});
                    return true;
                }));
            bool finished = false;
            require(artifact.requestImage(&receiver,
                                          [&](ScreenshotExportImageResult result) {
                                              require(!result.succeeded(),
                                                      "empty export must report failure");
                                              finished = true;
                                          }),
                    "failed image load must be accepted asynchronously");
            processUntil([&] { return finished; });
            require(diagnostics.flush(), "export diagnostics must flush");
            QFile file(diagnostics.status().currentFile);
            require(file.open(QIODevice::ReadOnly), "export log must be readable");
            bool found = false;
            for (const auto& line : file.readAll().split('\n')) {
                const auto record = QJsonDocument::fromJson(line).object();
                if (record.value(QStringLiteral("event")) !=
                    QStringLiteral("export.image_finished"))
                    continue;
                const auto fields = record.value(QStringLiteral("fields")).toObject();
                found = fields.value(QStringLiteral("operation")) == artifact.diagnosticId() &&
                        fields.value(QStringLiteral("outcome")) == QStringLiteral("failed") &&
                        !record.value(QStringLiteral("message")).toString().isEmpty();
            }
            require(found, "export failure must preserve its cause and correlation identifier");
            ScreenshotExportArtifact cancelled(ScreenshotExportSource::fromImageLoader(
                [](QObject*, std::function<void(QImage)>) { return true; }));
            require(cancelled.requestImage(
                        &receiver,
                        [](ScreenshotExportImageResult) {
                            require(false, "cancelled exports must not deliver callbacks");
                        }),
                    "pending cancellation fixture must accept image loading");
            cancelled.cancel();
            cancelled.cancel();
            require(diagnostics.flush(), "cancellation diagnostics must flush");
            file.seek(0);
            int cancellations = 0;
            for (const auto& line : file.readAll().split('\n')) {
                const auto record = QJsonDocument::fromJson(line).object();
                if (record.value(QStringLiteral("event")) == QStringLiteral("export.cancelled") &&
                    record.value(QStringLiteral("fields"))
                            .toObject()
                            .value(QStringLiteral("operation")) == cancelled.diagnosticId())
                    ++cancellations;
            }
            require(cancellations == 1, "a pending export must log cancellation exactly once");
        }
        diagnostics.shutdown();
        if (application.arguments().contains(QStringLiteral("--print-snapshots-only"))) {
            pinnedViewportSourceRendersExpectedPixels();
            emptyPinnedSourceMatchesCanvasProjection();
            pinnedViewportExportsReleaseImportedSnapshots();
            return EXIT_SUCCESS;
        }
        if (application.arguments().contains(QStringLiteral("--pinned-lifetime-only"))) {
            pinnedViewportExportsReleaseImportedSnapshots();
            return EXIT_SUCCESS;
        }
        if (application.arguments().contains(QStringLiteral("--quick-save-only"))) {
            quickSaveUsesOnlyConfiguredOutput();
            return EXIT_SUCCESS;
        }
        if (application.arguments().contains(QStringLiteral("--configured-directory-only"))) {
            configuredAutomaticDirectoryResolvesInTheSaveWorker();
            saveDirectoryTemplatesUseRequestTime();
            windowNamesSurviveAsynchronousFileExports();
            return EXIT_SUCCESS;
        }
        if (application.arguments().contains(QStringLiteral("--row-source-only"))) {
            rowRequestsCoalesceAndReuseBackingImage();
            convertedRowSourcesRetainOriginalPixelsWithoutMaterializing();
            lazyResizeRowsAndRegionsPreserveFullImagePixels();
            translucentWideGamutRowsKeepStraightAlphaThroughResizeAndEncoding();
            croppedRowsDoNotRejectDestinationsAfterTheirLastPixel();
            boundedExportRowsTests();
            return EXIT_SUCCESS;
        }
        if (application.arguments().contains(QStringLiteral("--clipboard-fusion-only"))) {
            clipboardPreparationFusesRowsAndClonesNativeOwnership();
            coldClipboardRequestsShareCanonicalRowsAndRetryFailures();
            cancelledColdClipboardRetainsConsumerLeaseUntilWorkerRetires();
            latePngSubscriberRetainsLeaseDuringFusedClipboardCancellation();
            clipboardSharesPendingConfiguredEncoding();
            clipboardReusesConfiguredPng();
            cancellationSuppressesPendingCallbacks();
            return EXIT_SUCCESS;
        }
        if (application.arguments().contains(QStringLiteral("--clipboard-files-only"))) {
            clipboardFilesUseTemporaryStorageAndKeepExportOptions();
            return EXIT_SUCCESS;
        }
        quickSaveUsesOnlyConfiguredOutput();
        clipboardFilesUseTemporaryStorageAndKeepExportOptions();
        automaticSavesUseRequestedEncoding();
        configuredAutomaticDirectoryResolvesInTheSaveWorker();
        saveDirectoryTemplatesUseRequestTime();
        clipboardUsesConfiguredCompression();
        manualPngSavesUseTheirRequestedCompression();
        clipboardSharesPendingConfiguredEncoding();
        clipboardReusesConfiguredPng();
        clipboardPreparationFusesRowsAndClonesNativeOwnership();
        coldClipboardRequestsShareCanonicalRowsAndRetryFailures();
        cancelledColdClipboardRetainsConsumerLeaseUntilWorkerRetires();
        latePngSubscriberRetainsLeaseDuringFusedClipboardCancellation();
        clipboardIgnoresCachedPngWithDifferentCompression();
        failedPngRequestsCanRetryWithoutInvalidatingOtherLevels();
        matchingRequestsSurviveSubscriberDestruction();
        for (auto compression :
             {ScreenshotCompressionLevel::Low, ScreenshotCompressionLevel::Medium,
              ScreenshotCompressionLevel::High}) {
            for (bool clipboardFirst : {false, true})
                clipboardAndSavesShareConfiguredEncoding(compression, clipboardFirst);
        }
        nonPngSaveReadsPixelsWithoutEncodingPng();
        fileEncodingsCoalesceAndNormalizeOptions();
        pdfPayloadReusePreservesDestinationMetadata();
        failedFileEncodingCanRetry();
        largeBmpUsesDirectStreamingAndDiskEntriesRespectBudget();
        imageRequestsShareOneAsyncLoad();
        canonicalEncodingCoalescesAndPreservesBufferIdentity();
        encodingFailureFansOutOnce();
        rowRequestsCoalesceAndReuseBackingImage();
        convertedRowSourcesRetainOriginalPixelsWithoutMaterializing();
        lazyResizeRowsAndRegionsPreserveFullImagePixels();
        translucentWideGamutRowsKeepStraightAlphaThroughResizeAndEncoding();
        croppedRowsDoNotRejectDestinationsAfterTheirLastPixel();
        boundedExportRowsTests();
        pngCacheBudgetEvictsLeastRecentlyUsedResults();
        oversizedPngStillFansOutWithoutRetention();
        cachedPngSavesReuseBytesBeyondRawCacheBudget();
        fileOutputsStreamBeyondCacheBudget();
        rowRequestFailureAndCancellationFanOut();
        cancellationSuppressesPendingCallbacks();
        cancelledEncodersRetainEveryConsumerLease();
        pinnedViewportSourceRendersExpectedPixels();
        emptyPinnedSourceMatchesCanvasProjection();
        pinnedViewportExportsReleaseImportedSnapshots();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
