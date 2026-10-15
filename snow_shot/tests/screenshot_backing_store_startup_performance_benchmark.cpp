#include "snow_shot/presentation/screenshotoverlayeventsink.h"
#include "snow_shot/presentation/screenshotoverlaywindow.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "../../test-support/memorysnapshot.h"
#include "../src/presentation/capture/captureframeimage.h"

#include <CoreGraphics/CoreGraphics.h>

#include <QApplication>
#include <QBackingStore>
#include <QCommandLineParser>
#include <QDateTime>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPaintEvent>
#include <QScreen>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QTimer>
#include <QWindow>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <time.h>

namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

qint64 wallNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

qint64 guiCpuNs() {
    timespec value{};
    require(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) == 0, "GUI CPU clock unavailable");
    return static_cast<qint64>(value.tv_sec) * 1000000000 + value.tv_nsec;
}

double ms(qint64 ns) {
    return static_cast<double>(ns) / 1000000.0;
}

QJsonObject memoryJson(const snow::test_support::MemorySnapshot& value) {
    return {{"resident_bytes", static_cast<qint64>(value.residentBytes)},
            {"footprint_bytes", static_cast<qint64>(value.footprintBytes)},
            {"process_peak_resident_bytes", static_cast<qint64>(value.peakResidentBytes)}};
}

class OverlayEvents final : public ScreenshotOverlayEventSink {
  public:
    bool shouldHandleOverlayMouseEvent(const ScreenshotOverlayWindow*, const QPointF&,
                                       bool) const override {
        return false;
    }
    void handleOverlayMousePress(ScreenshotOverlayWindow*, const QPointF&) override {}
    void handleOverlayMouseMove(ScreenshotOverlayWindow*, const QPointF&) override {}
    void handleOverlayMouseRelease(ScreenshotOverlayWindow*, const QPointF&) override {}
    ScreenshotOverlayRightClickResult handleOverlayRightClick(ScreenshotOverlayWindow*,
                                                              const QPointF&) override {
        return ScreenshotOverlayRightClickResult::Ignored;
    }
    bool handleOverlayWheel(ScreenshotOverlayWindow*, const QWheelEvent&) override {
        return false;
    }
    bool shouldBlockUnhandledOverlayKeyInput() const override {
        return false;
    }
    void raiseToolbarForCanvasInteraction() override {}
};

struct Sample {
    ScreenshotOverlayWindow* overlay = nullptr;
    QEventLoop* loop = nullptr;
    QRectF initialSelection;
    QRgb expectedPixel = 0;
    qint64 startNs = 0;
    qint64 startCpuNs = 0;
    qint64 arrivalNs = 0;
    qint64 revealNs = 0;
    qint64 attachCpuNs = 0;
    qint64 firstPaintEndNs = 0;
    qint64 firstPaintCpuNs = 0;
    qint64 firstDispatchEndNs = 0;
    qint64 firstDispatchCpuNs = 0;
    qint64 firstFrameTotalCpuNs = 0;
    qint64 movementQueuedNs = 0;
    qint64 movementAppliedNs = 0;
    qint64 movingPaintEndNs = 0;
    qint64 movingDispatchEndNs = 0;
    qint64 totalCpuNs = 0;
    qint64 prepCpuNs = 0;
    qint64 prepWallNs = 0;
    qint64 maxPrepCpuNs = 0;
    qint64 maxPrepWallNs = 0;
    qint64 lastPulseNs = 0;
    qint64 maxPulseGapNs = 0;
    qint64 paints = 0;
    qint64 hiddenPaints = 0;
    qint64 prematureShows = 0;
    qint64 preparationEventsAfterArrival = 0;
    qint64 maximumFootprint = 0;
    qint64 maximumResident = 0;
    QSize firstStorePhysicalSize;
    bool acquired = false;
    bool active = true;
    bool firstPixelsFresh = false;
    bool geometryCorrect = true;
    snow::test_support::MemorySnapshot before, ready, firstFrame, final;

    void observeMemory() {
        const auto value = snow::test_support::memorySnapshot();
        maximumFootprint = std::max(maximumFootprint, static_cast<qint64>(value.footprintBytes));
        maximumResident = std::max(maximumResident, static_cast<qint64>(value.residentBytes));
    }
};

class ObservedApplication final : public QApplication {
  public:
    using QApplication::QApplication;
    Sample* sample = nullptr;

    bool notify(QObject* receiver, QEvent* event) override {
        Sample* current = sample;
        if (current == nullptr || !current->active || current->overlay == nullptr)
            return QApplication::notify(receiver, event);
        const bool canvasPaint =
            receiver == current->overlay->canvas() && event->type() == QEvent::Paint;
        const bool nativeDispatch =
            (receiver == current->overlay || receiver == current->overlay->windowHandle()) &&
            (event->type() == QEvent::Expose || event->type() == QEvent::UpdateRequest);
        const bool preparation = receiver == current->overlay && event->type() >= QEvent::User;
        const qint64 started = (canvasPaint || nativeDispatch || preparation) ? wallNs() : 0;
        const qint64 cpu = started ? guiCpuNs() : 0;
        if (receiver == current->overlay && event->type() == QEvent::Show && !current->acquired)
            ++current->prematureShows;
        if (preparation && current->acquired)
            ++current->preparationEventsAfterArrival;
        const bool handled = QApplication::notify(receiver, event);
        if (canvasPaint || nativeDispatch || preparation) {
            const qint64 finished = wallNs();
            const qint64 elapsedCpu = guiCpuNs() - cpu;
            if (preparation) {
                current->prepCpuNs += elapsedCpu;
                current->prepWallNs += finished - started;
                current->maxPrepCpuNs = std::max(current->maxPrepCpuNs, elapsedCpu);
                current->maxPrepWallNs = std::max(current->maxPrepWallNs, finished - started);
            }
            if (canvasPaint) {
                ++current->paints;
                if (!current->acquired)
                    ++current->hiddenPaints;
                current->geometryCorrect &= current->overlay->captureGeometry().size() ==
                                            current->overlay->canvas()->size();
                if (current->firstPaintEndNs == 0 && current->acquired) {
                    current->firstPaintEndNs = finished;
                    current->firstPaintCpuNs = elapsedCpu;
                    // The parent backing-store paint remains active around the child paint.
                    // Inspect one interior pixel without another render, beginPaint, or flush.
                    QBackingStore* store = current->overlay->backingStore();
                    const QPaintDevice* device = store ? store->paintDevice() : nullptr;
                    if (device != nullptr && device->devType() == QInternal::Image) {
                        const auto* image = static_cast<const QImage*>(device);
                        current->firstStorePhysicalSize = image->size();
                        const QPoint point =
                            (current->overlay->canvas()->mapTo(
                                 current->overlay, current->overlay->canvas()->rect().center()) *
                             current->overlay->devicePixelRatioF());
                        if (image->rect().contains(point))
                            current->firstPixelsFresh =
                                image->pixel(point) == current->expectedPixel;
                    }
                }
                if (current->movementAppliedNs > 0 && current->movingPaintEndNs == 0)
                    current->movingPaintEndNs = finished;
            }
            if (nativeDispatch && current->firstPaintEndNs > 0 &&
                current->firstDispatchEndNs == 0) {
                current->firstDispatchEndNs = finished;
                current->firstDispatchCpuNs = elapsedCpu;
                current->firstFrameTotalCpuNs = guiCpuNs() - current->startCpuNs;
                current->firstFrame = snow::test_support::memorySnapshot();
                current->movementQueuedNs = wallNs();
                QMetaObject::invokeMethod(
                    this,
                    [current] {
                        current->movementAppliedNs = wallNs();
                        current->overlay->setScreenshotSelection(
                            current->initialSelection.translated(3, 2), false, 0);
                        current->overlay->canvas()->update();
                    },
                    Qt::QueuedConnection);
            }
            if (nativeDispatch && current->movingPaintEndNs > 0 &&
                current->movingDispatchEndNs == 0) {
                current->movingDispatchEndNs = finished;
                current->totalCpuNs = guiCpuNs() - current->startCpuNs;
                current->active = false;
                current->loop->quit();
            }
        }
        return handled;
    }
};

void settle(int milliseconds) {
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

QJsonObject runSample(ObservedApplication& app, SnowCanvasRuntime& runtime, OverlayEvents& events,
                      std::unique_ptr<ScreenshotOverlayWindow>& overlay, const QImage& image,
                      QSize physicalSize, int delay, int iteration, bool recreated,
                      ScreenshotBackingStorePreparationMode mode, bool nativeCapture) {
    Sample sample;
    sample.before = snow::test_support::memorySnapshot();
    sample.startNs = wallNs();
    sample.startCpuNs = guiCpuNs();
    sample.lastPulseNs = sample.startNs;
    app.sample = &sample;
    QEventLoop loop;
    sample.loop = &loop;
    std::atomic<qint64> workerReadyNs{0};
    QImage acquiredImage = image;
    QString acquisitionError;
    std::thread producer([&] {
        const auto deadline =
            std::chrono::steady_clock::time_point(std::chrono::nanoseconds(sample.startNs)) +
            std::chrono::milliseconds(delay);
        std::this_thread::sleep_until(deadline);
        if (nativeCapture) {
            SnowCaptureDesktopSessionConfig config{};
            config.capture_retry_count = 3;
            config.capture_backend = SNOW_CAPTURE_BACKEND_SCREEN_CAPTURE_KIT;
            config.pixel_format = SNOW_CAPTURE_PIXEL_FORMAT_RGBA8;
            std::unique_ptr<SnowCaptureDesktopSession,
                            decltype(&snow_capture_desktop_session_destroy)>
                session(snow_capture_desktop_session_create(&config),
                        snow_capture_desktop_session_destroy);
            SnowCaptureScreenshotRequest request{};
            request.version = SNOW_CAPTURE_SCREENSHOT_REQUEST_VERSION;
            request.struct_size = sizeof(request);
            std::unique_ptr<SnowCaptureScreenshotResult,
                            decltype(&snow_capture_screenshot_result_destroy)>
                result(session ? snow_capture_desktop_session_capture(session.get(), &request)
                               : nullptr,
                       snow_capture_screenshot_result_destroy);
            acquiredImage = {};
            if (result) {
                for (size_t index = 0;
                     index < snow_capture_screenshot_result_display_count(result.get()); ++index) {
                    SnowCaptureFrameInfo info{};
                    if (!snow_capture_screenshot_result_display_info(result.get(), index, &info) ||
                        !info.is_primary)
                        continue;
                    acquiredImage = snow_shot::presentation::capture::imageFromFrameLease(
                        snow_capture_screenshot_result_display_retain(result.get(), index),
                        info.rgba_bytes, info.rgba_len, info.width, info.height, info.stride_bytes,
                        info.pixel_format,
                        snow_shot::presentation::capture::FrameAlphaMode::Opaque);
                    break;
                }
            }
            if (acquiredImage.isNull())
                acquisitionError = QString::fromUtf8(snow_capture_last_error_message());
            else if (acquiredImage.size() != physicalSize)
                acquisitionError =
                    QStringLiteral("native source size differs from requested physical size");
        }
        workerReadyNs.store(wallNs());
        QMetaObject::invokeMethod(
            &app,
            [&] {
                if (!acquisitionError.isEmpty() || acquiredImage.isNull()) {
                    loop.quit();
                    return;
                }
                sample.arrivalNs = wallNs();
                sample.acquired = true;
                overlay->cancelBackingStorePreparation();
                sample.ready = snow::test_support::memorySnapshot();
                const qint64 attachCpu = guiCpuNs();
                const QRectF canvasRect(QPointF(), QSizeF(overlay->canvas()->size()));
                overlay->setScreenshotImage(acquiredImage, canvasRect);
                overlay->setScreenshotSelection(sample.initialSelection, false, 0);
                const QPoint sourcePoint =
                    overlay->canvas()->rect().center() * overlay->devicePixelRatioF();
                sample.expectedPixel = acquiredImage.pixel(sourcePoint);
                sample.attachCpuNs = guiCpuNs() - attachCpu;
                sample.revealNs = wallNs();
                overlay->showPreparedFrame(true);
            },
            Qt::QueuedConnection);
    });
    if (!recreated)
        overlay = std::make_unique<ScreenshotOverlayWindow>(events, new SnowCanvasWidget(runtime));
    sample.overlay = overlay.get();
    snowSetWidgetScreen(overlay, QGuiApplication::primaryScreen());
    const qreal dpr = overlay->devicePixelRatioF();
    const QSize logicalSize(qRound(physicalSize.width() / dpr),
                            qRound(physicalSize.height() / dpr));
    overlay->setCaptureGeometry(
        QRect(QGuiApplication::primaryScreen()->geometry().topLeft(), logicalSize));
    overlay->restoreNativeSurface();
    overlay->setCaptureGeometry(
        QRect(QGuiApplication::primaryScreen()->geometry().topLeft(), logicalSize));
    overlay->canvas()->setViewportCamera(logicalSize.width() / 2.0, logicalSize.height() / 2.0,
                                         1.0);
    sample.initialSelection = QRectF(logicalSize.width() * 0.25, logicalSize.height() * 0.25,
                                     logicalSize.width() * 0.5, logicalSize.height() * 0.5);
    overlay->resetBackingStorePreparationCountersForTesting();
    overlay->setBackingStorePreparationModeForTesting(mode);
    overlay->prepareBackingStore();
    QTimer pulse;
    pulse.setTimerType(Qt::PreciseTimer);
    pulse.setInterval(1);
    QObject::connect(&pulse, &QTimer::timeout, &loop, [&] {
        const qint64 now = wallNs();
        sample.maxPulseGapNs = std::max(sample.maxPulseGapNs, now - sample.lastPulseNs);
        sample.lastPulseNs = now;
        sample.observeMemory();
    });
    pulse.start();
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, &loop, &QEventLoop::quit);
    watchdog.start(5000);
    loop.exec();
    producer.join();
    pulse.stop();
    sample.active = false;
    app.sample = nullptr;
    require(acquisitionError.isEmpty() && !acquiredImage.isNull(),
            acquisitionError.isEmpty() ? "native acquisition failed"
                                       : acquisitionError.toUtf8().constData());
    sample.final = snow::test_support::memorySnapshot();
    sample.observeMemory();
    for (const auto& checkpoint : {sample.before, sample.ready, sample.firstFrame, sample.final}) {
        sample.maximumFootprint =
            std::max(sample.maximumFootprint, static_cast<qint64>(checkpoint.footprintBytes));
        sample.maximumResident =
            std::max(sample.maximumResident, static_cast<qint64>(checkpoint.residentBytes));
    }
    sample.maxPulseGapNs =
        std::max(sample.maxPulseGapNs, sample.movingDispatchEndNs - sample.lastPulseNs);
    const auto counters = overlay->backingStorePreparationCountersForTesting();
    const bool pending = overlay->isBackingStorePreparationPendingForTesting();
    require(sample.firstDispatchEndNs > 0 && sample.movingDispatchEndNs > 0,
            "native first/moving frame dispatch timed out");
    const bool correct = sample.prematureShows == 0 && sample.hiddenPaints == 0 &&
                         sample.firstPixelsFresh && sample.geometryCorrect && !pending;
    const QJsonObject record{
        {"iteration", iteration},
        {"phase", recreated ? "recreated" : "cold"},
        {"process_first_capture", iteration == 0 && !recreated},
        {"width", physicalSize.width()},
        {"height", physicalSize.height()},
        {"dpr", dpr},
        {"delay_ms", delay},
        {"correct", correct},
        {"fresh_first_pixels", sample.firstPixelsFresh},
        {"source_format", static_cast<int>(acquiredImage.format())},
        {"actual_source_width", acquiredImage.width()},
        {"actual_source_height", acquiredImage.height()},
        {"first_store_physical_width", sample.firstStorePhysicalSize.width()},
        {"first_store_physical_height", sample.firstStorePhysicalSize.height()},
        {"worker_ready_after_start_ms", ms(workerReadyNs.load() - sample.startNs)},
        {"geometry_correct", sample.geometryCorrect},
        {"premature_shows", sample.prematureShows},
        {"hidden_canvas_paints", sample.hiddenPaints},
        {"preparation_events_after_arrival", sample.preparationEventsAfterArrival},
        {"preparation_pending_after_frame", pending},
        {"startup_to_first_dispatch_ms", ms(sample.firstDispatchEndNs - sample.startNs)},
        {"ready_to_first_dispatch_ms", ms(sample.firstDispatchEndNs - workerReadyNs.load())},
        {"ready_to_delivery_ms", ms(sample.arrivalNs - workerReadyNs.load())},
        {"reveal_to_first_dispatch_ms", ms(sample.firstDispatchEndNs - sample.revealNs)},
        {"first_paint_gui_cpu_ms", ms(sample.firstPaintCpuNs)},
        {"first_native_dispatch_gui_cpu_ms", ms(sample.firstDispatchCpuNs)},
        {"first_frame_total_gui_cpu_ms", ms(sample.firstFrameTotalCpuNs)},
        {"through_moving_frame_gui_cpu_ms", ms(sample.totalCpuNs)},
        {"movement_queue_to_dispatch_ms", ms(sample.movingDispatchEndNs - sample.movementQueuedNs)},
        {"source_attach_gui_cpu_ms", ms(sample.attachCpuNs)},
        {"preparation_gui_cpu_ms", ms(sample.prepCpuNs)},
        {"preparation_wall_ms", ms(sample.prepWallNs)},
        {"max_preparation_slice_gui_cpu_ms", ms(sample.maxPrepCpuNs)},
        {"max_preparation_slice_wall_ms", ms(sample.maxPrepWallNs)},
        {"max_event_loop_pulse_gap_ms", ms(sample.maxPulseGapNs)},
        {"canvas_paints", sample.paints},
        {"allocation_requests", static_cast<qint64>(counters.allocationRequests)},
        {"page_touch_slices", static_cast<qint64>(counters.pageTouchSlices)},
        {"touched_bytes", static_cast<qint64>(counters.touchedBytes)},
        {"page_writes", static_cast<qint64>(counters.pageWrites)},
        {"maximum_slice_bytes", static_cast<qint64>(counters.maximumSliceBytes)},
        {"completed_preparations", static_cast<qint64>(counters.completedPreparations)},
        {"canceled_preparations", static_cast<qint64>(counters.canceledPreparations)},
        {"memory_before", memoryJson(sample.before)},
        {"memory_at_delivery", memoryJson(sample.ready)},
        {"memory_first_frame", memoryJson(sample.firstFrame)},
        {"memory_final", memoryJson(sample.final)},
        {"maximum_sampled_footprint_bytes", sample.maximumFootprint},
        {"maximum_sampled_resident_bytes", sample.maximumResident}};
    overlay->resetScreenshotRendering();
    overlay->releaseNativeSurface();
    settle(30);
    return record;
}
} // namespace

int main(int argc, char** argv) {
    ObservedApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({"mode", "disabled,allocation-only,page-touch", "mode", "page-touch"});
    parser.addOption({"width", "Physical source width", "pixels", "3840"});
    parser.addOption({"height", "Physical source height", "pixels", "2160"});
    parser.addOption({"delay-ms", "Controlled acquisition delay", "milliseconds", "20"});
    parser.addOption({"pairs", "Cold/recreated sample pairs", "count", "4"});
    parser.addOption({"output", "JSON output", "path"});
    parser.addOption({"native-capture", "Use a new ScreenCaptureKit session for every sample"});
    parser.process(app);
    try {
        require(QGuiApplication::platformName() == QStringLiteral("cocoa"),
                "this native benchmark requires the Cocoa platform");
        const QString modeName = parser.value("mode");
        require(modeName == "disabled" || modeName == "allocation-only" || modeName == "page-touch",
                "unknown preparation mode");
        const auto mode = modeName == "disabled" ? ScreenshotBackingStorePreparationMode::Disabled
                          : modeName == "allocation-only"
                              ? ScreenshotBackingStorePreparationMode::AllocationOnly
                              : ScreenshotBackingStorePreparationMode::PageTouch;
        const QSize size(parser.value("width").toInt(), parser.value("height").toInt());
        const int delay = parser.value("delay-ms").toInt();
        const int pairs = parser.value("pairs").toInt();
        const bool nativeCapture = parser.isSet("native-capture");
        require(!nativeCapture || CGPreflightScreenCaptureAccess(),
                "native capture permission is unavailable; benchmark does not request permission");
        require(size.width() > 0 && size.height() > 0 && size.width() <= 7680 &&
                    size.height() <= 4320 && delay >= 0 && delay <= 1000 && pairs > 0 &&
                    pairs <= 100,
                "invalid benchmark configuration");
        QTemporaryDir storageDirectory;
        auto& storage = snow_shot::storage::ApplicationStorage::instance();
        require(storageDirectory.isValid() &&
                    storage.initialize({storageDirectory.path(), storageDirectory.path(), 60000})
                        .success,
                "isolated storage unavailable");
        snow_shot::presentation::styles::ThemeManager::instance();
        SnowCanvasRuntime runtime;
        OverlayEvents events;
        std::unique_ptr<ScreenshotOverlayWindow> overlay;
        QImage images[2];
        if (!nativeCapture) {
            images[0] = QImage(size, QImage::Format_RGBA8888);
            images[1] = QImage(size, QImage::Format_RGBA8888);
            images[0].fill(QColor(20, 173, 109));
            images[1].fill(QColor(194, 33, 71));
        }
        settle(50);
        QJsonArray records;
        bool allCorrect = true;
        for (int pair = 0; pair < pairs; ++pair) {
            overlay.reset();
            for (bool recreated : {false, true}) {
                const auto record =
                    runSample(app, runtime, events, overlay, images[recreated ? 1 : 0], size, delay,
                              pair, recreated, mode, nativeCapture);
                allCorrect &= record.value("correct").toBool();
                records.append(record);
            }
        }
        const QJsonObject report{
            {"schema_version", 1},
            {"benchmark", "screenshot_backing_store_startup"},
            {"generated_utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
            {"mode", modeName},
            {"qt", QString::fromLatin1(qVersion())},
            {"native_capture", nativeCapture},
            {"source_scope",
             nativeCapture ? "Actual primary-display acquisition with a new ScreenCaptureKit "
                             "session for every sample; includes native capture bandwidth and "
                             "session startup. No captured image is saved."
                           : "Controlled worker deadline and preallocated opaque RGBA source."},
            {"platform", QSysInfo::prettyProductName()},
            {"all_correct", allCorrect},
            {"scope",
             "Current production overlay/canvas; acquisition source is described in source_scope. "
             "No compositor fence. Native dispatch end includes QWidget backing-store flush. "
             "GUI CPU includes native creation, preparation, attachment and queued movement. "
             "1ms memory/pulse timer and bounded notify clocks are shared instrumentation. "
             "Native dispatch CPU includes nested canvas CPU; do not add those values."},
            {"memory_scope",
             "Mach physical footprint and resident checkpoints with a 1ms sampler; "
             "process peak RSS includes fixtures, Qt initialization and all samples. "
             "Controlled source images are preallocated; native source growth is measured."},
            {"records", records}};
        const QByteArray bytes = QJsonDocument(report).toJson(QJsonDocument::Indented);
        if (parser.isSet("output")) {
            QFile output(parser.value("output"));
            require(output.open(QIODevice::WriteOnly | QIODevice::Truncate), "cannot write report");
            output.write(bytes);
        } else {
            std::cout << bytes.constData();
        }
        overlay.reset();
        storage.shutdown();
        return allCorrect ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
