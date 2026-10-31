#include "snow_shot/presentation/screenshotanimationplayback.h"
#include "../src/image/snowimageqtcodec.h"
#include "pinned_animation_fixtures.h"
#include "pinned_animation_optimization_fixtures.h"
#include "snow_draw_engine_qt/snow_canvas_image.h"

#include <QGuiApplication>
#include <QColorSpace>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QPainter>

#include <cstdlib>
#include <algorithm>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <psapi.h>
#elif defined(Q_OS_MACOS)
#include <mach/mach.h>
#endif

namespace {
quint64 residentBytes() {
#ifdef Q_OS_WIN
    PROCESS_MEMORY_COUNTERS counters{};
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
        return static_cast<quint64>(counters.WorkingSetSize);
#elif defined(Q_OS_MACOS)
    task_basic_info_data_t info{};
    mach_msg_type_number_t count = TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info),
                  &count) == KERN_SUCCESS)
        return static_cast<quint64>(info.resident_size);
#endif
    return 0;
}

quint64 privateBytes() {
#ifdef Q_OS_WIN
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (K32GetProcessMemoryInfo(GetCurrentProcess(),
                                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                sizeof(counters)))
        return static_cast<quint64>(counters.PrivateUsage);
#endif
    return 0;
}

struct GuiProbe {
    QTimer timer;
    QElapsedTimer clock;
    qint64 previous = 0;
    std::vector<qint64> lateness;
    quint64 peakResident = residentBytes();
    GuiProbe(bool enabled, std::function<void()> sample = {}) {
        clock.start();
        QObject::connect(&timer, &QTimer::timeout, &timer, [this, sample = std::move(sample)] {
            const qint64 now = clock.nsecsElapsed();
            lateness.push_back(std::max<qint64>(0, now - previous - 16'000'000));
            previous = now;
            if (lateness.size() % 8 == 0)
                peakResident = std::max(peakResident, residentBytes());
            if (sample)
                sample();
        });
        timer.setTimerType(Qt::PreciseTimer);
        if (enabled)
            timer.start(16);
    }
    QJsonObject report() {
        timer.stop();
        std::sort(lateness.begin(), lateness.end());
        const qint64 maximum = lateness.empty() ? 0 : lateness.back();
        const qint64 percentile = lateness.empty() ? 0 : lateness[(lateness.size() - 1) * 95 / 100];
        return {{QStringLiteral("gui_heartbeat_samples"), static_cast<qint64>(lateness.size())},
                {QStringLiteral("gui_heartbeat_max_lateness_ms"),
                 static_cast<double>(maximum) / 1'000'000},
                {QStringLiteral("gui_heartbeat_p95_lateness_ms"),
                 static_cast<double>(percentile) / 1'000'000},
                {QStringLiteral("peak_observed_process_resident_bytes"),
                 static_cast<qint64>(std::max(peakResident, residentBytes()))}};
    }
};
double processCpuMilliseconds() {
#ifdef Q_OS_WIN
    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        const auto ticks = [](FILETIME value) {
            return (quint64(value.dwHighDateTime) << 32) | value.dwLowDateTime;
        };
        return static_cast<double>(ticks(kernel) + ticks(user)) / 10'000;
    }
#elif defined(Q_OS_MACOS)
    task_thread_times_info_data_t info{};
    mach_msg_type_number_t count = TASK_THREAD_TIMES_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_THREAD_TIMES_INFO, reinterpret_cast<task_info_t>(&info),
                  &count) == KERN_SUCCESS)
        return (info.user_time.seconds + info.system_time.seconds) * 1000.0 +
               (info.user_time.microseconds + info.system_time.microseconds) / 1000.0;
#endif
    return 0;
}

void pump(int milliseconds) {
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

struct Pin {
    std::unique_ptr<ScreenshotAnimationPlayback> playback;
    QImage first;
    QImage second;
    QImage surface;
    quint64 paints = 0;
    void paint(const QImage& transformed, quint64 token) {
        if (surface.size() != transformed.size())
            surface =
                snowCanvasAllocateImage(transformed.size(), QImage::Format_ARGB32_Premultiplied);
        if (surface.isNull()) {
            std::cerr << "Benchmark paint surface allocation failed\n";
            std::exit(1);
        }
        {
            QPainter painter(&surface);
            painter.setCompositionMode(QPainter::CompositionMode_Source);
            painter.drawImage(QPoint(), transformed);
        }
        ++paints;
        playback->framePainted(token);
    }
    Pin(std::shared_ptr<const ScreenshotAnimationSource> animation, const QTransform& transform,
        int speed = 100, bool playing = true) {
        ScreenshotAnimationPlayback::Hooks hooks;
        hooks.frameReady = [this](const QImage&, const QImage& transformed, int, quint64 token) {
            paint(transformed, token);
        };
        hooks.error = [](const QString& error) {
            std::cerr << error.toStdString() << '\n';
            std::exit(1);
        };
        playback = std::make_unique<ScreenshotAnimationPlayback>(std::move(hooks));
        playback->setTransform(transform);
        const QImage normalized =
            snowCanvasConvertImage(animation->firstFrame, QImage::Format_ARGB32_Premultiplied);
        const QImage initial =
            transform.isIdentity()
                ? normalized
                : snowCanvasTransformImage(normalized, transform, Qt::SmoothTransformation);
        playback->setSource(std::move(animation), playing, speed, initial);
        playback->setSuspended(false);
    }
    Pin(qint64 durationNs) {
        first = QImage(320, 180, QImage::Format_ARGB32_Premultiplied);
        second = QImage(first.size(), first.format());
        first.fill(Qt::red);
        second.fill(Qt::blue);
        ScreenshotAnimationPlayback::Hooks hooks;
        hooks.frameReady = [this](const QImage&, const QImage& transformed, int, quint64 token) {
            paint(transformed, token);
        };
        ScreenshotAnimationPlayback::Options options;
        options.decode = [this](const auto& request, const auto& completion) {
            const QImage image = request.frameIndex == 0 ? first : second;
            completion(image, image, {});
        };
        playback =
            std::make_unique<ScreenshotAnimationPlayback>(std::move(hooks), std::move(options));
        auto source = std::make_shared<ScreenshotAnimationSource>();
        source->canvasSize = first.size();
        source->firstFrame = first;
        source->frameDurationsNs = {durationNs, durationNs};
        source->totalPlayCount = 0;
        playback->setSource(std::move(source));
        playback->setSuspended(false);
    }
};

QJsonObject measure(const char* name, int count, qint64 sourceDuration, bool suspended, bool paused,
                    const std::shared_ptr<const ScreenshotAnimationSource>& animation = {},
                    QTransform transform = {}, int speed = 100) {
    std::vector<std::unique_ptr<Pin>> pins;
    pins.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index)
        pins.push_back(animation ? std::make_unique<Pin>(animation, transform, speed)
                                 : std::make_unique<Pin>(sourceDuration));
    pump(100);
    for (const auto& pin : pins) {
        if (suspended)
            pin->playback->setSuspended(true);
        if (paused)
            pin->playback->pause();
    }
    pump(25);
    std::vector<ScreenshotAnimationPlayback::Diagnostics> before;
    std::vector<quint64> paintsBefore;
    before.reserve(pins.size());
    paintsBefore.reserve(pins.size());
    for (const auto& pin : pins)
        before.push_back(pin->playback->diagnostics());
    for (const auto& pin : pins)
        paintsBefore.push_back(pin->paints);
    qint64 peakCache = before.front().cachedBytes;
    qint64 peakReserved = before.front().reservedWorkingBytes;
    GuiProbe probe(!(suspended || paused), [&] {
        const auto current = pins.front()->playback->diagnostics();
        peakCache = std::max(peakCache, current.cachedBytes);
        peakReserved = std::max(peakReserved, current.reservedWorkingBytes);
    });
    const double cpuBefore = processCpuMilliseconds();
    QElapsedTimer wall;
    wall.start();
    pump(750);
    const double wallMs = static_cast<double>(wall.nsecsElapsed()) / 1'000'000;
    const double cpuMs = processCpuMilliseconds() - cpuBefore;
    quint64 wakes = 0;
    quint64 decodes = 0;
    quint64 publications = 0;
    quint64 paints = 0;
    quint64 completions = 0;
    quint64 cacheHits = 0;
    quint64 cacheMisses = 0;
    qint64 decodeLatency = 0;
    qint64 maximumDecodeLatency = 0;
    qint64 preparationLead = 0;
    for (std::size_t index = 0; index < pins.size(); ++index) {
        const auto after = pins[index]->playback->diagnostics();
        wakes += after.deadlineWakeups - before[index].deadlineWakeups;
        decodes += after.decodeRequests - before[index].decodeRequests;
        publications += after.publishedFrames - before[index].publishedFrames;
        paints += pins[index]->paints - paintsBefore[index];
        completions += after.completedDecodes - before[index].completedDecodes;
        decodeLatency += after.decodeLatencyNs - before[index].decodeLatencyNs;
        maximumDecodeLatency = std::max(maximumDecodeLatency, after.maximumDecodeLatencyNs);
        preparationLead = std::max(preparationLead, after.preparationLeadNs);
        cacheHits += after.cacheHits - before[index].cacheHits;
        cacheMisses += after.cacheMisses - before[index].cacheMisses;
        peakCache = std::max(peakCache, after.cachedBytes);
        peakReserved = std::max(peakReserved, after.reservedWorkingBytes);
    }
    const auto shared = pins.front()->playback->diagnostics();
    const quint64 coordinatorWakes = shared.coordinatorWakeups - before.front().coordinatorWakeups;
    const bool idlePassed = !(suspended || paused) ||
                            (wakes == 0 && coordinatorWakes == 0 && shared.activeDeadlines == 0 &&
                             decodes == 0 && publications == 0 && paints == 0);
    if (!idlePassed) {
        std::cerr << "Suspended animation consumed playback work\n";
        std::exit(1);
    }
    if (animation && animation->canvasSize.width() >= 1920 && !(suspended || paused) &&
        paints == 0) {
        std::cerr << "Large visible animation did not advance during measurement\n";
        std::exit(1);
    }
    auto result = probe.report();
    const QJsonObject measurements{
        {QStringLiteral("scenario"), QString::fromLatin1(name)},
        {QStringLiteral("pin_count"), count},
        {QStringLiteral("wall_ms"), wallMs},
        {QStringLiteral("process_cpu_ms"), cpuMs},
        {QStringLiteral("controller_deadline_callbacks"), static_cast<qint64>(wakes)},
        {QStringLiteral("shared_coordinator_wakes"), static_cast<qint64>(coordinatorWakes)},
        {QStringLiteral("active_deadlines_at_end"), static_cast<qint64>(shared.activeDeadlines)},
        {QStringLiteral("decode_requests"), static_cast<qint64>(decodes)},
        {QStringLiteral("published_frames"), static_cast<qint64>(publications)},
        {QStringLiteral("raster_repaints"), static_cast<qint64>(paints)},
        {QStringLiteral("cache_hits"), static_cast<qint64>(cacheHits)},
        {QStringLiteral("cache_misses"), static_cast<qint64>(cacheMisses)},
        {QStringLiteral("mean_decode_submission_latency_ms"),
         completions
             ? static_cast<double>(decodeLatency) / static_cast<double>(completions) / 1'000'000
             : 0},
        {QStringLiteral("max_decode_submission_latency_ms_including_warmup"),
         static_cast<double>(maximumDecodeLatency) / 1'000'000},
        {QStringLiteral("adaptive_preparation_lead_ms"),
         static_cast<double>(preparationLead) / 1'000'000},
        {QStringLiteral("peak_optional_cache_bytes"), peakCache},
        {QStringLiteral("peak_reserved_working_bytes"), peakReserved},
        {QStringLiteral("idle_work_check_passed"), idlePassed}};
    for (auto it = measurements.begin(); it != measurements.end(); ++it)
        result.insert(it.key(), it.value());
    return result;
}

// A complete ordinary GIF stream, encoded with its bounded 4096-entry LZW
// dictionary. Full changing frames exercise composition, copies and raster cache
// pressure without clear-per-pixel streams which distort GIFLib decode costs.
QByteArray generatedGif(int width = 960, int height = 540, int frames = 12) {
    QByteArray gif("GIF89a");
    const auto word = [&gif](int value) {
        gif.append(static_cast<char>(value & 0xff));
        gif.append(static_cast<char>((value >> 8) & 0xff));
    };
    word(width);
    word(height);
    gif.append("\x80\x00\x00", 3);
    gif.append("\xff\x00\x00\x00\x00\xff", 6);
    gif.append("\x21\xff\x0b", 3);
    gif.append("NETSCAPE2.0", 11);
    gif.append("\x03\x01\x00\x00\x00", 5);
    for (int frame = 0; frame < frames; ++frame) {
        gif.append("\x21\xf9\x04\x04\x02\x00\x00\x00", 8);
        gif.append('\x2c');
        word(0);
        word(0);
        word(width);
        word(height);
        gif.append('\x00');
        gif.append('\x02');
        QByteArray pixels;
        pixels.reserve(width * height / 4);
        quint32 pending = 0;
        int bits = 0;
        int codeWidth = 3;
        int runningCode = 6;
        const auto code = [&pixels, &pending, &bits, &codeWidth, &runningCode](quint32 value) {
            pending |= value << bits;
            bits += codeWidth;
            while (bits >= 8) {
                pixels.append(static_cast<char>(pending & 0xffU));
                pending >>= 8;
                bits -= 8;
            }
            if (value == 4) {
                codeWidth = 3;
                runningCode = 6;
            } else if (runningCode < 4097) {
                ++runningCode;
                if (codeWidth < 12 && runningCode > (1 << codeWidth))
                    ++codeWidth;
            }
        };
        const auto color = [width, frame](int pixel) {
            return ((pixel % width + frame * 13) % 64) < 32 ? 0U : 1U;
        };
        std::unordered_map<quint32, quint32> dictionary;
        dictionary.reserve(4096);
        quint32 nextCode = 6;
        code(4);
        quint32 prefix = color(0);
        for (int pixel = 1; pixel < width * height; ++pixel) {
            const quint32 value = color(pixel);
            const quint32 key = (prefix << 8U) | value;
            const auto found = dictionary.find(key);
            if (found != dictionary.end()) {
                prefix = found->second;
                continue;
            }
            code(prefix);
            if (nextCode < 4096) {
                dictionary.emplace(key, nextCode++);
            } else {
                code(4);
                dictionary.clear();
                nextCode = 6;
            }
            prefix = value;
        }
        code(prefix);
        code(5);
        if (bits)
            pixels.append(static_cast<char>(pending & 0xffU));
        for (qsizetype start = 0; start < pixels.size(); start += 255) {
            const qsizetype size = std::min<qsizetype>(255, pixels.size() - start);
            gif.append(static_cast<char>(size));
            gif.append(pixels.constData() + start, size);
        }
        gif.append('\x00');
    }
    gif.append('\x3b');
    return gif;
}

std::shared_ptr<const ScreenshotAnimationSource> decodedSource(const QByteArray& bytes) {
    QString error;
    auto source = snow_shot::image_codec::decodeAnimationSource(bytes, &error);
    if (!source || !error.isEmpty()) {
        std::cerr << "Native benchmark fixture failed: " << error.toStdString() << '\n';
        std::exit(1);
    }
    return source;
}

void waitForStep(Pin& pin, int expected) {
    QEventLoop loop;
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (pin.playback->frameIndex() == expected && !pin.playback->busy())
            loop.quit();
    });
    poll.start(1);
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    loop.exec();
    if (pin.playback->frameIndex() != expected || pin.playback->busy()) {
        const auto diagnostics = pin.playback->diagnostics();
        std::cerr << "Manual native benchmark step failed to complete: expected=" << expected
                  << " displayed=" << pin.playback->frameIndex() << " busy=" << pin.playback->busy()
                  << " requested=" << pin.playback->requestedPlaying()
                  << " suspended=" << pin.playback->suspended()
                  << " decodes=" << diagnostics.decodeRequests
                  << " completed=" << diagnostics.completedDecodes
                  << " stale=" << diagnostics.staleCompletions
                  << " published=" << diagnostics.publishedFrames
                  << " painted=" << diagnostics.displayedFrames
                  << " cache_hits=" << diagnostics.cacheHits
                  << " cache_misses=" << diagnostics.cacheMisses
                  << " resets=" << diagnostics.decoderResets << '\n';
        std::exit(1);
    }
}

QJsonObject reverseCacheMiss() {
    constexpr int count = 40;
    constexpr int steps = count;
    const auto source = decodedSource(generatedGif(960, 540, count));
    if (source->frameDurationsNs.size() != count) {
        std::cerr << "Reverse benchmark frame count mismatch: expected=" << count
                  << " actual=" << source->frameDurationsNs.size() << '\n';
        std::exit(1);
    }
    Pin pin(source, {}, 100, false);
    for (int frame = 1; frame < count; ++frame) {
        pin.playback->nextFrame();
        waitForStep(pin, frame);
    }
    const auto before = pin.playback->diagnostics();
    const quint64 paints = pin.paints;
    GuiProbe probe(true);
    const double cpu = processCpuMilliseconds();
    QElapsedTimer wall;
    wall.start();
    std::vector<qint64> stepLatency;
    stepLatency.reserve(steps);
    // Canonical identity frames occupy one raster each. Traverse the entire sequence to
    // cross its 64 MiB cache budget, then wrap to a frame evicted by reverse traversal.
    for (int index = 0; index < steps; ++index) {
        const int frame = (count - 2 - index + count) % count;
        QElapsedTimer step;
        step.start();
        pin.playback->previousFrame();
        waitForStep(pin, frame);
        stepLatency.push_back(step.nsecsElapsed());
    }
    const auto after = pin.playback->diagnostics();
    if (after.decoderResets == before.decoderResets || after.cacheMisses == before.cacheMisses) {
        std::cerr << "Reverse benchmark must exercise a real cache-miss prefix replay\n";
        std::exit(1);
    }
    std::sort(stepLatency.begin(), stepLatency.end());
    auto result = probe.report();
    result.insert(QStringLiteral("scenario"), QStringLiteral("native-reverse-cache-miss"));
    result.insert(QStringLiteral("pin_count"), 1);
    result.insert(QStringLiteral("source_frames"), count);
    result.insert(QStringLiteral("wall_ms"), static_cast<double>(wall.nsecsElapsed()) / 1'000'000);
    result.insert(QStringLiteral("process_cpu_ms"), processCpuMilliseconds() - cpu);
    result.insert(QStringLiteral("reverse_steps"), steps);
    result.insert(QStringLiteral("raster_repaints"), static_cast<qint64>(pin.paints - paints));
    result.insert(QStringLiteral("cache_hits"),
                  static_cast<qint64>(after.cacheHits - before.cacheHits));
    result.insert(QStringLiteral("cache_misses"),
                  static_cast<qint64>(after.cacheMisses - before.cacheMisses));
    result.insert(QStringLiteral("decoder_resets"),
                  static_cast<qint64>(after.decoderResets - before.decoderResets));
    result.insert(QStringLiteral("composited_frames"),
                  static_cast<qint64>(after.compositedFrames - before.compositedFrames));
    result.insert(QStringLiteral("step_max_latency_ms"),
                  static_cast<double>(stepLatency.back()) / 1'000'000);
    result.insert(QStringLiteral("step_p95_latency_ms"),
                  static_cast<double>(stepLatency[(stepLatency.size() - 1) * 95 / 100]) /
                      1'000'000);
    result.insert(QStringLiteral("retained_optional_cache_bytes"), after.cachedBytes);
    result.insert(QStringLiteral("reserved_working_bytes"), after.reservedWorkingBytes);
    return result;
}

void optimizationRequire(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

QJsonObject optimizationSourceStorage(const ScreenshotAnimationSource& source) {
    const bool shared = source.firstFrame.constBits() == source.firstDisplayFrame.constBits();
    return {
        {QStringLiteral("encoded_bytes"), source.encoded.size()},
        {QStringLiteral("logical_initial_raster_bytes"),
         source.firstFrame.sizeInBytes() + source.firstDisplayFrame.sizeInBytes()},
        {QStringLiteral("distinct_initial_raster_bytes"),
         source.firstFrame.sizeInBytes() + (shared ? 0 : source.firstDisplayFrame.sizeInBytes())},
        {QStringLiteral("initial_rasters_share_storage"), shared}};
}

void validateOptimizationSource(const ScreenshotAnimationSource& source, QSize size,
                                bool profiled = false) {
    optimizationRequire(source.isAnimated() && source.canvasSize == size &&
                            !source.firstFrame.isNull() && !source.firstDisplayFrame.isNull(),
                        "Optimization source admission or initial rasters failed.");
    const QColorSpace srgb(QColorSpace::SRgb);
    const QImage expected =
        source.firstFrame.colorSpace().isValid() && source.firstFrame.colorSpace() != srgb
            ? snowImageConvertedToColorSpace(source.firstFrame, srgb,
                                             QImage::Format_ARGB32_Premultiplied)
            : source.firstFrame.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    optimizationRequire(source.firstDisplayFrame == expected,
                        "Optimization initial display changed normalized pixels.");
    if (profiled)
        optimizationRequire(source.firstFrame.colorSpace() == QColorSpace(QColorSpace::DisplayP3) &&
                                source.firstDisplayFrame.colorSpace() == srgb,
                            "Optimization source did not preserve and normalize Display P3.");
}

QJsonObject optimizationColdImport(const char* name, const QByteArray& encoded, QSize size) {
    QJsonArray samples;
    QJsonObject storage;
    for (int repetition = 0; repetition < 3; ++repetition) {
        quint64 imports = 0;
        std::shared_ptr<const ScreenshotAnimationSource> retained;
        const double cpu = processCpuMilliseconds();
        QElapsedTimer timer;
        timer.start();
        do {
            retained = decodedSource(encoded);
            optimizationRequire(retained->canvasSize == size &&
                                    retained->firstFrame.pixel(0, 0) == qRgb(255, 0, 0),
                                "Cold-import fixture composition changed.");
            ++imports;
        } while (timer.elapsed() < 1200);
        const double wallMs = static_cast<double>(timer.nsecsElapsed()) / 1'000'000;
        const double cpuMs = processCpuMilliseconds() - cpu;
        validateOptimizationSource(*retained, size);
        storage = optimizationSourceStorage(*retained);
        samples.append(
            QJsonObject{{QStringLiteral("imports"), static_cast<qint64>(imports)},
                        {QStringLiteral("wall_ms"), wallMs},
                        {QStringLiteral("process_cpu_ms"), cpuMs},
                        {QStringLiteral("mean_import_ms"), wallMs / static_cast<double>(imports)},
                        {QStringLiteral("resident_bytes"), static_cast<qint64>(residentBytes())},
                        {QStringLiteral("private_bytes"), static_cast<qint64>(privateBytes())}});
    }
    return {{QStringLiteral("scenario"), QString::fromLatin1(name)},
            {QStringLiteral("source_storage"), storage},
            {QStringLiteral("samples"), samples}};
}

QJsonObject optimizationSnapshots(const char* name, const QByteArray& encoded, QSize size,
                                  int retainedCount, qsizetype metadataBytes = 0,
                                  bool profiled = false) {
    const auto source = decodedSource(encoded);
    validateOptimizationSource(*source, size, profiled);
    QJsonArray samples;
    for (int repetition = 0; repetition < 3; ++repetition) {
        QString error;
        auto decoder = snow_shot::image_codec::openAnimationDecoder(source, &error);
        optimizationRequire(decoder && decoder->advance(&error) && error.isEmpty(),
                            "Optimization snapshot decoder failed to open/advance.");
        const QImage expected = source->firstFrame;
        quint64 snapshots = 0;
        const double cpu = processCpuMilliseconds();
        QElapsedTimer timer;
        timer.start();
        do {
            const QImage original = decoder->snapshot(&error);
            const QImage image =
                snowCanvasConvertImage(original, QImage::Format_ARGB32_Premultiplied);
            optimizationRequire(!original.isNull() && !image.isNull() && error.isEmpty() &&
                                    image.size() == size &&
                                    image.pixel(0, 0) == expected.pixel(0, 0) &&
                                    image.pixel(size.width() - 1, size.height() - 1) ==
                                        expected.pixel(size.width() - 1, size.height() - 1),
                                "Optimization decoder snapshot changed composed pixels.");
            ++snapshots;
        } while (timer.elapsed() < 1200);
        const double wallMs = static_cast<double>(timer.nsecsElapsed()) / 1'000'000;
        const double cpuMs = processCpuMilliseconds() - cpu;
        const quint64 residentBefore = residentBytes();
        const quint64 privateBefore = privateBytes();
        std::vector<QImage> retained;
        retained.reserve(static_cast<std::size_t>(retainedCount));
        for (int index = 0; index < retainedCount; ++index) {
            auto image = decoder->snapshot(&error);
            optimizationRequire(!image.isNull() && error.isEmpty(),
                                "Retained optimization snapshot failed.");
            retained.push_back(std::move(image));
        }
        const quint64 residentRetained = residentBytes();
        const quint64 privateRetained = privateBytes();
        optimizationRequire(decoder->advance(&error) && error.isEmpty(),
                            "Optimization decoder failed to advance with retained snapshots.");
        decoder.reset();
        for (const QImage& image : retained)
            optimizationRequire(image == expected && image.colorSpace() == expected.colorSpace(),
                                "Retained snapshot changed after advance/decoder destruction.");
        const qint64 retainedRasterBytes =
            retained.empty() ? 0 : retained.front().sizeInBytes() * retainedCount;
        retained.clear();
        samples.append(QJsonObject{
            {QStringLiteral("snapshots"), static_cast<qint64>(snapshots)},
            {QStringLiteral("wall_ms"), wallMs},
            {QStringLiteral("process_cpu_ms"), cpuMs},
            {QStringLiteral("mean_snapshot_preparation_ms"),
             wallMs / static_cast<double>(snapshots)},
            {QStringLiteral("prepared_raster_mib_per_second"),
             static_cast<double>(expected.sizeInBytes()) * static_cast<double>(snapshots) * 1000.0 /
                 (wallMs * 1024.0 * 1024.0)},
            {QStringLiteral("retained_snapshots"), retainedCount},
            {QStringLiteral("retained_raster_bytes"), retainedRasterBytes},
            {QStringLiteral("resident_before_retention_bytes"),
             static_cast<qint64>(residentBefore)},
            {QStringLiteral("resident_with_retention_bytes"),
             static_cast<qint64>(residentRetained)},
            {QStringLiteral("resident_retention_delta_bytes"),
             static_cast<qint64>(residentRetained) - static_cast<qint64>(residentBefore)},
            {QStringLiteral("private_before_retention_bytes"), static_cast<qint64>(privateBefore)},
            {QStringLiteral("private_with_retention_bytes"), static_cast<qint64>(privateRetained)},
            {QStringLiteral("private_retention_delta_bytes"),
             static_cast<qint64>(privateRetained) - static_cast<qint64>(privateBefore)},
            {QStringLiteral("resident_after_release_bytes"), static_cast<qint64>(residentBytes())},
            {QStringLiteral("private_after_release_bytes"), static_cast<qint64>(privateBytes())}});
    }
    return {{QStringLiteral("scenario"), QString::fromLatin1(name)},
            {QStringLiteral("metadata_payload_bytes"), metadataBytes},
            {QStringLiteral("source_storage"), optimizationSourceStorage(*source)},
            {QStringLiteral("samples"), samples}};
}

QJsonObject optimizationSharedCacheRetirement() {
    const auto source = decodedSource(generatedGif(960, 540, 4));
    QJsonArray samples;
    for (int repetition = 0; repetition < 3; ++repetition) {
        auto retiring = std::make_unique<Pin>(source, QTransform{}, 100, false);
        Pin remaining(source, {}, 100, false);
        for (int index = 1; index < 4; ++index) {
            retiring->playback->nextFrame();
            waitForStep(*retiring, index);
            remaining.playback->nextFrame();
            waitForStep(remaining, index);
        }
        retiring.reset();
        pump(25); // Dispatch retirement before observing the surviving controller.
        const auto before = remaining.playback->diagnostics();
        QElapsedTimer timer;
        timer.start();
        for (int index = 0; index < 20; ++index) {
            const int expected = (remaining.playback->frameIndex() + 3) % 4;
            remaining.playback->previousFrame();
            waitForStep(remaining, expected);
        }
        const auto after = remaining.playback->diagnostics();
        samples.append(QJsonObject{
            {QStringLiteral("steps"), 20},
            {QStringLiteral("wall_ms"), static_cast<double>(timer.nsecsElapsed()) / 1'000'000},
            {QStringLiteral("cache_bytes_after_sibling_retirement"), before.cachedBytes},
            {QStringLiteral("cache_hits"), static_cast<qint64>(after.cacheHits - before.cacheHits)},
            {QStringLiteral("cache_misses"),
             static_cast<qint64>(after.cacheMisses - before.cacheMisses)},
            {QStringLiteral("decoder_resets"),
             static_cast<qint64>(after.decoderResets - before.decoderResets)}});
    }
    return {{QStringLiteral("scenario"), QStringLiteral("shared-cache-sibling-retirement")},
            {QStringLiteral("samples"), samples}};
}

QJsonObject optimizationReport() {
    constexpr qsizetype metadataBytes = 2 * 1024 * 1024;
    const auto gif1080 = generatedGif(1920, 1080, 2);
    const auto gif4k = generatedGif(3840, 2160, 2);
    using namespace pinned_animation_optimization_fixtures;
    QJsonArray scenarios;
    scenarios.append(optimizationColdImport("cold-import-gif-1080p", gif1080, {1920, 1080}));
    scenarios.append(optimizationColdImport("cold-import-gif-4k", gif4k, {3840, 2160}));
    scenarios.append(optimizationSnapshots("snapshot-gif-1080p", gif1080, {1920, 1080}, 2));
    scenarios.append(optimizationSnapshots("snapshot-gif-4k", gif4k, {3840, 2160}, 2));
    scenarios.append(
        optimizationSnapshots("snapshot-profiled-webp-control", webp(0), {2, 2}, 64, 0, true));
    scenarios.append(optimizationSnapshots("snapshot-profiled-webp-metadata", webp(metadataBytes),
                                           {2, 2}, 64, metadataBytes, true));
    scenarios.append(
        optimizationSnapshots("snapshot-profiled-apng-control", apng(0), {2, 2}, 64, 0, true));
    scenarios.append(optimizationSnapshots("snapshot-profiled-apng-metadata", apng(metadataBytes),
                                           {2, 2}, 64, metadataBytes, true));
    scenarios.append(optimizationSharedCacheRetirement());
    return {{QStringLiteral("benchmark"), QStringLiteral("animation-optimization")},
            {QStringLiteral("fixture"),
             QStringLiteral("Three repetitions per scenario. Cold imports and real decoder "
                            "snapshot plus ARGB32 premultiplied preparation each run for at least "
                            "1200 ms; fixture generation "
                            "is excluded. Retained Display P3 WebP/APNG snapshots compare a 2 MiB "
                            "valid XMP payload against profile-only controls. Resident/private "
                            "memory is sampled before retention, with 64 snapshots, and after "
                            "release. Initial raster storage is counted by distinct constBits "
                            "addresses. Shared-cache retirement steps a surviving sibling twenty "
                            "times after the other controller retires. These are synchronous "
                            "codec/storage measurements, excluding native window presentation.")},
            {QStringLiteral("private_memory_available"), privateBytes() > 0},
            {QStringLiteral("scenarios"), scenarios}};
}
} // namespace

int main(int argc, char** argv) {
    QGuiApplication application(argc, argv);
    QJsonArray scenarios;
    if (QGuiApplication::arguments().contains(QStringLiteral("--optimization-only"))) {
        try {
            std::cout
                << QJsonDocument(optimizationReport()).toJson(QJsonDocument::Indented).constData();
            return 0;
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
    if (QGuiApplication::arguments().contains(QStringLiteral("--reverse-only"))) {
        const QJsonObject report{
            {QStringLiteral("benchmark"), QStringLiteral("animation-playback-scheduling")},
            {QStringLiteral("scenarios"), QJsonArray{reverseCacheMiss()}}};
        std::cout << QJsonDocument(report).toJson(QJsonDocument::Indented).constData();
        return 0;
    }
    scenarios.append(measure("visible-10fps", 1, 100'000'000, false, false));
    scenarios.append(measure("visible-60fps", 1, 16'666'667, false, false));
    scenarios.append(measure("visible-twelve-pins", 12, 16'666'667, false, false));
    scenarios.append(measure("hidden", 12, 16'666'667, true, false));
    scenarios.append(measure("thumbnail", 12, 16'666'667, true, false));
    scenarios.append(measure("paused", 12, 16'666'667, false, true));
    for (const auto& fixture : pinned_animation_fixtures::images()) {
        const auto source = decodedSource(fixture.bytes);
        const QByteArray scenario = QByteArray("native-small-") + fixture.suffix.toLatin1();
        scenarios.append(measure(scenario.constData(), 1, 0, false, false, source));
    }
    const auto nativeSource = decodedSource(generatedGif());
    scenarios.append(measure("native-gif", 1, 0, false, false, nativeSource));
    scenarios.append(measure("native-gif-four-pins", 4, 0, false, false, nativeSource));
    scenarios.append(
        measure("native-gif-rotated", 1, 0, false, false, nativeSource, QTransform().rotate(90)));
    scenarios.append(measure("native-gif-hidden", 4, 0, true, false, nativeSource));
    scenarios.append(measure("native-gif-paused", 4, 0, false, true, nativeSource));
    scenarios.append(measure("native-gif-thumbnail", 4, 0, true, false, nativeSource));
    scenarios.append(measure("native-gif-high-rate", 1, 0, false, false, nativeSource, {}, 400));
    scenarios.append(
        measure("native-large-gif", 1, 0, false, false, decodedSource(generatedGif(1920, 1080))));
    scenarios.append(reverseCacheMiss());
    const QJsonObject report{
        {QStringLiteral("benchmark"), QStringLiteral("animation-playback-scheduling")},
        {QStringLiteral("fixture"),
         QStringLiteral(
             "Synthetic immutable frames isolate deadline and idle work. Native GIF uses "
             "twelve changing full 960x540 frames at 50 fps, real bounded decode workers, "
             "normalization, rotations, caching and cancellation. QPainter paints each published "
             "frame into a managed raster before acknowledgement. A 16 ms GUI heartbeat runs "
             "only in active scenarios; idle measurements have no heartbeat. Additional small "
             "GIF/WebP/APNG, 1920x1080, 400% and >64 MiB reverse-replay fixtures cover extremes. "
             "Shared coordinator wakes and per-pin callbacks are scheduling power proxies. "
             "Native window interactions are tested separately.")},
        {QStringLiteral("scenarios"), scenarios}};
    std::cout << QJsonDocument(report).toJson(QJsonDocument::Indented).constData();
    return 0;
}
