// SPDX-License-Identifier: GPL-3.0-only
#include "snow_shot/presentation/screenshotclipboardcontent.h"
#include "snow_shot/storage/pinnedwindowrepository.h"
#include "pinned_animation_native_fixtures.h"
#include "snowimageqtcodec.h"
#include "../../test-support/memorysnapshot.h"

#include <QApplication>
#include <QBuffer>
#include <QColorSpace>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QUuid>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

struct Options final {
    QString scenario = QStringLiteral("saved-pin");
    QString mode = QStringLiteral("full");
    QSize size{3840, 2160};
    int count = 10;
};

Options options(const QStringList& arguments) {
    Options result;
    for (qsizetype index = 1; index < arguments.size(); ++index) {
        const auto& argument = arguments[index];
        if (argument == QStringLiteral("-platform")) {
            ++index;
            continue;
        }
        require(index + 1 < arguments.size(), "missing benchmark option value");
        const auto& value = arguments[++index];
        if (argument == QStringLiteral("--scenario")) {
            result.scenario = value;
            continue;
        }
        if (argument == QStringLiteral("--mode")) {
            result.mode = value;
            continue;
        }
        bool valid = false;
        const int number = value.toInt(&valid);
        require(valid && number > 0, "invalid numeric benchmark option");
        if (argument == QStringLiteral("--width"))
            result.size.setWidth(number);
        else if (argument == QStringLiteral("--height"))
            result.size.setHeight(number);
        else if (argument == QStringLiteral("--count"))
            result.count = number;
        else
            throw std::runtime_error("unknown benchmark option");
    }
    require(result.scenario == QStringLiteral("saved-pin") ||
                result.scenario == QStringLiteral("animated-file"),
            "unknown import benchmark scenario");
    require(result.mode == QStringLiteral("full") || result.mode == QStringLiteral("optimized"),
            "unknown import benchmark mode");
    return result;
}

class MemorySampler final {
  public:
    MemorySampler()
        : m_thread([this] {
              while (!m_stop.load()) {
                  const auto memory = snow::test_support::memorySnapshot();
                  m_resident.store(std::max(m_resident.load(), quint64(memory.residentBytes)));
                  m_footprint.store(std::max(m_footprint.load(), quint64(memory.footprintBytes)));
                  std::this_thread::sleep_for(std::chrono::milliseconds(2));
              }
          }) {}
    ~MemorySampler() {
        m_stop.store(true);
        m_thread.join();
    }
    quint64 resident() const {
        return m_resident.load();
    }
    quint64 footprint() const {
        return m_footprint.load();
    }

  private:
    std::atomic_bool m_stop{false};
    std::atomic<quint64> m_resident{0};
    std::atomic<quint64> m_footprint{0};
    std::thread m_thread;
};

QByteArray animatedPng(QSize size) {
    using namespace pinned_animation_native_fixtures::detail;
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    require(!image.isNull(), "allocate import fixture");
    image.fill(QColor(200, 80, 40));
    image.setColorSpace(QColorSpace(QColorSpace::DisplayP3));
    QByteArray encoded;
    QBuffer buffer(&encoded);
    require(buffer.open(QIODevice::WriteOnly) && image.save(&buffer, "PNG"),
            "encode import fixture");
    Png source;
    for (qsizetype position = 8; position <= encoded.size() - 12;) {
        const auto length = static_cast<qsizetype>(read32(encoded.constData() + position));
        const auto* type = encoded.constData() + position + 4;
        if (std::memcmp(type, "IHDR", 4) == 0)
            source.header = encoded.mid(position + 8, length);
        else if (std::memcmp(type, "IDAT", 4) == 0)
            source.data.append(encoded.mid(position + 8, length));
        else if (std::memcmp(type, "IEND", 4) != 0)
            source.common.append(encoded.mid(position, length + 12));
        position += length + 12;
    }
    QByteArray result("\x89PNG\r\n\x1a\n", 8);
    chunk(result, "IHDR", source.header);
    result.append(source.common);
    QByteArray control;
    append32(control, 2);
    append32(control, 0);
    chunk(result, "acTL", control);
    quint32 sequence = 0;
    for (int index = 0; index < 2; ++index) {
        QByteArray frame;
        append32(frame, sequence++);
        append32(frame, static_cast<quint32>(size.width()));
        append32(frame, static_cast<quint32>(size.height()));
        append32(frame, 0);
        append32(frame, 0);
        frame.append(QByteArray::fromHex("0001000a0000"));
        chunk(result, "fcTL", frame);
        for (const auto& data : source.data) {
            if (index == 0) {
                chunk(result, "IDAT", data);
            } else {
                QByteArray payload;
                append32(payload, sequence++);
                payload.append(data);
                chunk(result, "fdAT", payload);
            }
        }
    }
    chunk(result, "IEND", {});
    return result;
}

quint64 checksum(const QImage& image) {
    quint64 result = 1469598103934665603ULL;
    for (int y = 0; y < image.height(); ++y) {
        const auto* row = image.constScanLine(y);
        for (int x = 0; x < image.width() * image.depth() / 8; ++x) {
            result ^= row[x];
            result *= 1099511628211ULL;
        }
    }
    return result;
}
} // namespace

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    try {
        const auto config = options(application.arguments());
        QTemporaryDir directory;
        require(directory.isValid(), "create import fixture directory");
        snow_shot::storage::PinnedWindowRepository repository(directory.path(), true, 30000);
        QString id;
        std::optional<ScreenshotClipboardLocalImage> file;
        if (config.scenario == QStringLiteral("saved-pin")) {
            snow_shot::storage::PinnedWindowRecord record;
            record.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            id = record.id;
            record.image = QImage(config.size, QImage::Format_ARGB32_Premultiplied);
            require(!record.image.isNull(), "allocate saved-pin fixture");
            record.image.fill(QColor(200, 80, 40));
            record.nativeGeometry = QRect(QPoint(), config.size);
            record.canvasSourceRect = QRectF(record.nativeGeometry);
            record.contentCanvasRect = record.canvasSourceRect;
            record.surfaceCanvasRect = record.canvasSourceRect;
            record.initialWindowSize = config.size;
            record.canvasSession = QByteArray(32 * 1024 * 1024, 'c');
            record.recognitionResults = QByteArray(16 * 1024 * 1024, 'r');
            require(repository.upsert(std::move(record)).success && repository.flush().success,
                    "persist saved-pin fixture");
        } else {
            const QString path = directory.filePath(QStringLiteral("source.apng"));
            const auto encoded = animatedPng(config.size);
            QFile output(path);
            require(output.open(QIODevice::WriteOnly) && output.write(encoded) == encoded.size(),
                    "persist animated-file fixture");
            output.close();
            const auto files = ScreenshotClipboardContentReader::snapshotLocalFiles({path});
            require(files.size() == 1, "snapshot animated-file fixture");
            file = files.first();
        }
        qint64 admitted = 0;
        const auto admit = [&](qint64 bytes) {
            admitted = std::max(admitted, bytes);
            return true;
        };
        const auto decode = [&]() -> QImage {
            if (!id.isEmpty()) {
                if (config.mode == QStringLiteral("optimized")) {
                    const auto source = repository.loadPreviewSource(id, admit);
                    require(source.has_value(), "load source-only import fixture");
                    return source->image;
                }
                const auto source = repository.loadRecord(id, admit);
                require(source.has_value(), "load saved-pin import fixture");
                return source->image;
            }
            if (config.mode == QStringLiteral("optimized")) {
                const auto image =
                    ScreenshotClipboardContentReader::decodeImageFile(*file, {}, admit);
                require(!image.isNull(), "decode first-frame import fixture");
                return image;
            }
            ScreenshotClipboardContentSnapshot snapshot;
            snapshot.localImage = file;
            const auto content = ScreenshotClipboardContentReader::decode(snapshot, {}, admit);
            require(content.has_value(), "decode animated-file import fixture");
            return content->image;
        };
        // Each invocation measures one fixture. Warmup initializes the codec;
        // alternating fresh baseline/head processes avoids cross-scenario caches.
        const auto reference = decode();
        const quint64 referenceChecksum = checksum(reference);
        std::vector<double> samples;
        samples.reserve(static_cast<size_t>(config.count));
        const auto baseline = snow::test_support::memorySnapshot();
        MemorySampler sampler;
        for (int index = 0; index < config.count; ++index) {
            QElapsedTimer timer;
            timer.start();
            const auto image = decode();
            samples.push_back(double(timer.nsecsElapsed()) / 1000000);
            require(image.size() == config.size && checksum(image) == referenceChecksum,
                    "import benchmark must preserve source pixels");
        }
        std::sort(samples.begin(), samples.end());
        const auto percentile = [&](double fraction) {
            const auto index = static_cast<size_t>(std::ceil(fraction * samples.size()) - 1);
            return samples[std::min(index, samples.size() - 1)];
        };
        const auto after = snow::test_support::memorySnapshot();
        const quint64 resident = std::max(
            {sampler.resident(), quint64(baseline.residentBytes), quint64(after.residentBytes)});
        const quint64 footprint = std::max(
            {sampler.footprint(), quint64(baseline.footprintBytes), quint64(after.footprintBytes)});
        const QJsonObject report{{QStringLiteral("scenario"), config.scenario},
                                 {QStringLiteral("implementation"), config.mode},
                                 {QStringLiteral("width"), config.size.width()},
                                 {QStringLiteral("height"), config.size.height()},
                                 {QStringLiteral("samples"), config.count},
                                 {QStringLiteral("p50_ms"), percentile(.50)},
                                 {QStringLiteral("p95_ms"), percentile(.95)},
                                 {QStringLiteral("peak_admitted_bytes"), double(admitted)},
                                 {QStringLiteral("peak_resident_delta_bytes"),
                                  double(resident - baseline.residentBytes)},
                                 {QStringLiteral("peak_footprint_delta_bytes"),
                                  double(footprint - baseline.footprintBytes)},
                                 {QStringLiteral("checksum"), QString::number(referenceChecksum)}};
        std::cout << QJsonDocument(report).toJson(QJsonDocument::Compact).constData() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
