#include "snow_canvas_pressure_brush_fixture.h"
#include "snow_canvas_pressure_mask_atlas.h"

#include <QGuiApplication>

#include <chrono>
#include <iomanip>
#include <string>

#if defined(SNOW_PRESSURE_BENCHMARK_NOT_RELEASE)
#error "Pressure brush benchmarks must be compiled with the Release performance preset."
#endif

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

namespace {
using namespace pressure_brush_fixture;
using Clock = std::chrono::steady_clock;

struct Scenario {
    double width;
    qreal dpr;
    SnowStrokeStyle style;
    std::string trace;
    bool fullPaint;
};

struct Options {
    std::size_t samples = 0;
    std::size_t iterations = 32;
    std::size_t batchSize = 8;
    double width = 0;
    qreal dpr = 0;
    double zoom = 1;
    std::string device = "pen";
    std::string style;
    std::string trace;
    std::string mode = "both";
    std::string paint;
    bool matrix = false;
    QSize surface{1920, 1080};
};

Options parseOptions(const QStringList& arguments) {
    Options options;
    for (int index = 1; index < arguments.size(); ++index) {
        const std::string name = arguments[index].toStdString();
        if (name == "--matrix") {
            options.matrix = true;
            continue;
        }
        if (name == "-platform") {
            ++index;
            continue;
        }
        require(index + 1 < arguments.size(), "benchmark options require a value");
        const std::string value = arguments[++index].toStdString();
        if (name == "--samples")
            options.samples = std::max<std::size_t>(2, std::stoull(value));
        else if (name == "--iterations")
            options.iterations = std::max<std::size_t>(1, std::stoull(value));
        else if (name == "--batch")
            options.batchSize = std::clamp<std::size_t>(std::stoull(value), 1, 256);
        else if (name == "--width")
            options.width = std::clamp(std::stod(value), 1.0, 72.0);
        else if (name == "--dpr")
            options.dpr = std::clamp(std::stod(value), 1.0, 2.0);
        else if (name == "--zoom")
            options.zoom = std::clamp(std::stod(value), 0.1, 8.0);
        else if (name == "--device")
            options.device = value;
        else if (name == "--style")
            options.style = value;
        else if (name == "--trace")
            options.trace = value;
        else if (name == "--mode")
            options.mode = value;
        else if (name == "--paint")
            options.paint = value;
        else if (name == "--surface") {
            require(value == "compact" || value == "full", "--surface accepts compact or full");
            options.surface = value == "compact" ? QSize(640, 360) : QSize(1920, 1080);
        } else if (name == "--surface-width")
            options.surface.setWidth(std::clamp(std::stoi(value), 64, 8192));
        else if (name == "--surface-height")
            options.surface.setHeight(std::clamp(std::stoi(value), 64, 8192));
        else
            require(false, "unknown pressure brush benchmark option");
    }
    require(options.mode == "both" || options.mode == "regular" || options.mode == "pressure",
            "--mode accepts regular, pressure or both");
    require(options.device == "pen" || options.device == "mouse", "--device accepts pen or mouse");
    require(options.paint.empty() || options.paint == "full" || options.paint == "local",
            "--paint accepts full or local");
    require(options.style.empty() || options.style == "solid" || options.style == "dashed" ||
                options.style == "dotted",
            "--style accepts solid, dashed or dotted");
    require(options.trace.empty() || options.trace == "straight" || options.trace == "noisy" ||
                options.trace == "spiral" || options.trace == "offscreen",
            "--trace accepts straight, noisy, spiral or offscreen");
    return options;
}

const char* styleName(SnowStrokeStyle style) {
    return style == SNOW_STROKE_STYLE_DASHED   ? "dashed"
           : style == SNOW_STROKE_STYLE_DOTTED ? "dotted"
                                               : "solid";
}

std::vector<Scenario> scenarios(const Options& options) {
    std::vector<Scenario> cases;
    if (options.matrix) {
        for (double width : {1.0, 8.0, 32.0, 72.0})
            for (qreal dpr : {1.0, 2.0})
                for (SnowStrokeStyle style :
                     {SNOW_STROKE_STYLE_SOLID, SNOW_STROKE_STYLE_DASHED, SNOW_STROKE_STYLE_DOTTED})
                    for (bool full : {false, true})
                        cases.push_back({width, dpr, style, "noisy", full});
    } else {
        // Cover every required dimension while keeping default runs manageable. --matrix
        // expands width x DPR x stroke style x repaint policy only when explicitly requested.
        cases = {{1, 1, SNOW_STROKE_STYLE_SOLID, "straight", false},
                 {8, 2, SNOW_STROKE_STYLE_DASHED, "noisy", true},
                 {32, 1, SNOW_STROKE_STYLE_DOTTED, "spiral", false},
                 {72, 2, SNOW_STROKE_STYLE_SOLID, "offscreen", true}};
    }
    for (auto& test : cases) {
        if (options.width > 0)
            test.width = options.width;
        if (options.dpr > 0)
            test.dpr = options.dpr;
        if (!options.style.empty())
            test.style = options.style == "dashed"   ? SNOW_STROKE_STYLE_DASHED
                         : options.style == "dotted" ? SNOW_STROKE_STYLE_DOTTED
                                                     : SNOW_STROKE_STYLE_SOLID;
        if (!options.trace.empty())
            test.trace = options.trace;
        if (!options.paint.empty())
            test.fullPaint = options.paint == "full";
    }
    if (options.width > 0 && options.dpr > 0 && !options.style.empty() && !options.trace.empty() &&
        !options.paint.empty())
        cases.resize(1);
    return cases;
}

QPointF tracePoint(const Scenario& test, std::size_t index, const QSize& surface) {
    const double step = static_cast<double>(index);
    if (test.trace == "spiral") {
        const double radius = 100 + std::sin(step * 0.007) * 80;
        return {320 + std::cos(step * 0.03) * radius, 180 + std::sin(step * 0.03) * radius * 0.7};
    }
    if (test.trace == "offscreen")
        return {std::max(2200, surface.width() + 256) + step * 0.9,
                180 + std::sin(step * 0.07) * 30};
    const double phase = std::fmod(step * 0.9, 1160.0);
    const double x = 30 + (phase <= 580 ? phase : 1160 - phase);
    if (test.trace == "straight")
        return {x, 180};
    return {x, 180 + std::sin(step * 0.07) * 40 + (index % 2 == 0 ? -1.0 : 1.0)};
}

double milliseconds(Clock::duration elapsed) {
    return std::chrono::duration<double, std::milli>(elapsed).count();
}

double percentile(std::vector<double> values, double fraction) {
    std::sort(values.begin(), values.end());
    return values[static_cast<std::size_t>(
        std::round(static_cast<double>(values.size() - 1) * fraction))];
}

std::uint64_t peakResidentBytes() {
#if defined(Q_OS_WIN)
    PROCESS_MEMORY_COUNTERS memory{};
    memory.cb = static_cast<DWORD>(sizeof(memory));
    return GetProcessMemoryInfo(GetCurrentProcess(), &memory, static_cast<DWORD>(sizeof(memory)))
               ? static_cast<std::uint64_t>(memory.PeakWorkingSetSize)
               : 0;
#else
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0)
        return 0;
#if defined(Q_OS_MACOS)
    return static_cast<std::uint64_t>(usage.ru_maxrss);
#else
    return static_cast<std::uint64_t>(usage.ru_maxrss) * 1024;
#endif
#endif
}

// Probe after the measured input/sync/paint frame. Acquiring the same incremental patch
// beforehand would warm geometry caches and make the measured sync artificially cheap.
std::size_t transportedBytes(Engine& engine, const SnowPatchCursor& previousCursor) {
    ScopedPatchHandle patch;
    SnowPatchInfo info{};
    require(snow_viewport_acquire_patch(engine.runtime.get(), engine.viewport.get(),
                                        &previousCursor, patch.outParam()) == SNOW_OK &&
                snow_patch_get_info(patch.get(), &info) == SNOW_OK,
            "inspect incremental pressure brush payload");
    std::size_t bytes =
        sizeof(info) +
        static_cast<std::size_t>(info.scene_op_count + info.overlay_op_count +
                                 info.spotlight_op_count) *
            sizeof(SnowPatchOp) +
        static_cast<std::size_t>(info.scene_item_count) * sizeof(SnowSceneDisplayItem) +
        static_cast<std::size_t>(info.scene_dirty_rect_count + info.overlay_dirty_rect_count +
                                 info.decoration_dirty_rect_count) *
            sizeof(SnowDirtyRect);
    const SnowPathGeometryPatch* operations = nullptr;
    const SnowPathChunkRange* ranges = nullptr;
    const SnowPathChunk* chunks = nullptr;
    const SnowArrowPathCommand* commands = nullptr;
    std::uint32_t operationCount = 0, rangeCount = 0, chunkCount = 0, commandCount = 0;
    require(
        snow_patch_get_path_geometry_ops(patch.get(), &operations, &operationCount) == SNOW_OK &&
            snow_patch_get_path_geometry_ranges(patch.get(), &ranges, &rangeCount) == SNOW_OK &&
            snow_patch_get_path_geometry_chunks(patch.get(), &chunks, &chunkCount) == SNOW_OK &&
            snow_patch_get_path_geometry_commands(patch.get(), &commands, &commandCount) == SNOW_OK,
        "inspect pressure geometry ABI arrays");
    bytes += static_cast<std::size_t>(operationCount) * sizeof(*operations) +
             static_cast<std::size_t>(rangeCount) * sizeof(*ranges) +
             static_cast<std::size_t>(chunkCount) * sizeof(*chunks) +
             static_cast<std::size_t>(commandCount) * sizeof(*commands);
    return bytes;
}

void run(const Options& options, const Scenario& test, std::size_t initialSamples,
         SnowBrushMode mode) {
    snow_canvas_pressure_mask::clearCacheForCurrentThread();
    Engine engine(mode, test.width, test.style, {220, 30, 90, 128}, options.surface);
    require(snow_viewport_set_camera(engine.runtime.get(), engine.viewport.get(),
                                     engine.size.width() / 2.0, engine.size.height() / 2.0,
                                     options.zoom) == SNOW_OK,
            "set benchmark zoom");
    engine.sync();
    const auto input = [&](SnowPointerEventType type, std::size_t index) {
        auto event =
            pointer(type, tracePoint(test, index, engine.size),
                    0.5 + std::sin(static_cast<double>(index) * 0.03) * 0.45, index * 8 + 1);
        if (options.device == "mouse") {
            event.pointer.device = SNOW_POINTER_DEVICE_MOUSE;
            event.pointer.has_pressure = 0;
            event.pointer.pressure = 0;
        }
        return event;
    };
    engine.dispatch(input(SNOW_POINTER_EVENT_DOWN, 0));
    const auto seedBegin = Clock::now();
    for (std::size_t begin = 1; begin < initialSamples; begin += 64) {
        std::vector<SnowInputEvent> batch;
        for (std::size_t index = begin; index < std::min(begin + 64, initialSamples); ++index)
            batch.push_back(input(SNOW_POINTER_EVENT_MOVE, index));
        engine.batch(batch);
    }
    engine.sync();
    const double seedTime = milliseconds(Clock::now() - seedBegin);
    QImage image(
        QSize(qRound(engine.size.width() * test.dpr), qRound(engine.size.height() * test.dpr)),
        QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(test.dpr);
    image.fill(Qt::transparent);
    const auto coldPaintBegin = Clock::now();
    paint(image, engine.cache.sceneInfo(), engine.cache.sceneItems(), engine.cache.sceneItemCount(),
          QRegion(QRect(QPoint(), engine.size)), &engine.cache);
    const double coldPaintTime = milliseconds(Clock::now() - coldPaintBegin);
    const auto coldAtlas = snow_canvas_pressure_mask::diagnosticsForCurrentThread();
    snow_canvas_pressure_mask::resetDiagnosticsForCurrentThread();
    std::vector<double> inputTimes, syncTimes, paintTimes, totalTimes, probeTimes;
    std::size_t ffiBytes = 0, maximumFfiBytes = 0, retainedClientBytes = 0, paintedFrames = 0;
    for (std::size_t frame = 0; frame < options.iterations; ++frame) {
        std::vector<SnowInputEvent> batch;
        batch.reserve(options.batchSize);
        for (std::size_t sample = 0; sample < options.batchSize; ++sample) {
            const std::size_t index = initialSamples + frame * options.batchSize + sample;
            batch.push_back(input(SNOW_POINTER_EVENT_MOVE, index));
        }
        const SnowPatchCursor previousCursor = engine.cache.patchCursor();
        const auto inputBegin = Clock::now();
        engine.batch(batch);
        const auto inputEnd = Clock::now();
        engine.sync();
        const auto syncEnd = Clock::now();
        const QRegion dirty =
            test.fullPaint
                ? QRegion(QRect(QPoint(), engine.size))
                : snow_canvas_display::dirtyRectsToRegion(engine.cache.sceneDirtyRects(),
                                                          engine.cache.sceneDirtyRectCount(),
                                                          QRect(QPoint(), engine.size));
        paint(image, engine.cache.sceneInfo(), engine.cache.sceneItems(),
              engine.cache.sceneItemCount(), dirty, &engine.cache);
        const auto paintEnd = Clock::now();
        if (!dirty.isEmpty())
            ++paintedFrames;
        inputTimes.push_back(milliseconds(inputEnd - inputBegin));
        syncTimes.push_back(milliseconds(syncEnd - inputEnd));
        paintTimes.push_back(milliseconds(paintEnd - syncEnd));
        totalTimes.push_back(milliseconds(paintEnd - inputBegin));
        const std::size_t frameBytes = transportedBytes(engine, previousCursor);
        probeTimes.push_back(milliseconds(Clock::now() - paintEnd));
        ffiBytes += frameBytes;
        maximumFfiBytes = std::max(maximumFfiBytes, frameBytes);
        std::size_t geometryBytes = 0;
        for (std::uint32_t index = 0; index < engine.cache.sceneItemCount(); ++index)
            geometryBytes += geometryPayloadBytes(engine.cache.sceneItems()[index]);
        retainedClientBytes =
            std::max(retainedClientBytes, engine.cache.retainedStorageBytes() + geometryBytes +
                                              static_cast<std::size_t>(image.sizeInBytes()));
    }
    std::size_t centerChunks = 0, pressureChunks = 0;
    for (std::uint32_t index = 0; index < engine.cache.sceneItemCount(); ++index) {
        centerChunks += engine.cache.sceneItems()[index].pathChunks().size();
        pressureChunks += engine.cache.sceneItems()[index].pressurePathChunks().size();
    }
    std::cout << "mode=" << (mode == SNOW_BRUSH_MODE_REGULAR ? "regular" : "pressure")
              << " samples=" << initialSamples << " width=" << test.width << " dpr=" << test.dpr
              << " zoom=" << options.zoom << " device=" << options.device
              << " style=" << styleName(test.style) << " trace=" << test.trace
              << " surface_width=" << engine.size.width()
              << " surface_height=" << engine.size.height()
              << " paint=" << (test.fullPaint ? "full" : "local") << " batch=" << options.batchSize
              << " frames=" << options.iterations << " painted_frames=" << paintedFrames
              << " seed_ms=" << seedTime << " cold_paint_ms=" << coldPaintTime
              << " cold_pressure_masks=" << coldAtlas.rasterizedMasks;
    for (const auto& stage :
         {std::make_pair("input_batch", &inputTimes), std::make_pair("sync", &syncTimes),
          std::make_pair("paint", &paintTimes), std::make_pair("total", &totalTimes),
          std::make_pair("payload_probe", &probeTimes)}) {
        std::cout << ' ' << stage.first << "_p50_ms=" << percentile(*stage.second, 0.5) << ' '
                  << stage.first << "_p95_ms=" << percentile(*stage.second, 0.95);
    }
    const auto pressureAtlas = snow_canvas_pressure_mask::diagnosticsForCurrentThread();
    std::cout << " ffi_payload_mean_bytes="
              << static_cast<double>(ffiBytes) / static_cast<double>(options.iterations)
              << " ffi_payload_max_bytes=" << maximumFfiBytes
              << " retained_client_payload_bytes=" << retainedClientBytes
              << " retained_pressure_atlas_bytes=" << pressureAtlas.retainedBytes
              << " pressure_atlas_entries=" << pressureAtlas.entries
              << " pressure_tiles_queried=" << pressureAtlas.queriedTiles
              << " pressure_chunks_queried=" << pressureAtlas.queriedChunks
              << " pressure_atlas_hits=" << pressureAtlas.hits
              << " pressure_union_tile_hits=" << pressureAtlas.unionTileHits
              << " pressure_masks_rasterized=" << pressureAtlas.rasterizedMasks
              << " pressure_union_rebuilds=" << pressureAtlas.unionRebuilds
              << " pressure_union_copies=" << pressureAtlas.unionCopies
              << " pressure_union_copy_bytes=" << pressureAtlas.unionCopyBytes
              << " pressure_tiles_drawn=" << pressureAtlas.drawnTiles
              << " pressure_atlas_evictions=" << pressureAtlas.evictions
              << " process_peak_rss_bytes=" << peakResidentBytes()
              << " center_chunks=" << centerChunks << " pressure_chunks=" << pressureChunks << '\n';
}
} // namespace

int main(int argc, char** argv) {
    // Qt consumes --style as its own platform option. Preserve benchmark arguments first.
    QStringList arguments;
    for (int index = 0; index < argc; ++index)
        arguments.append(QString::fromLocal8Bit(argv[index]));
    const auto options = parseOptions(arguments);
    QGuiApplication application(argc, argv);
    std::cout << std::fixed << std::setprecision(3);
    const std::vector<std::size_t> samples = options.samples == 0
                                                 ? std::vector<std::size_t>{512, 4096, 16000}
                                                 : std::vector<std::size_t>{options.samples};
    for (const auto sampleCount : samples)
        for (const auto& test : scenarios(options))
            for (const auto mode : {SNOW_BRUSH_MODE_REGULAR, SNOW_BRUSH_MODE_PRESSURE_SENSITIVE})
                if (options.mode == "both" ||
                    (options.mode == "regular" && mode == SNOW_BRUSH_MODE_REGULAR) ||
                    (options.mode == "pressure" && mode == SNOW_BRUSH_MODE_PRESSURE_SENSITIVE))
                    run(options, test, sampleCount, mode);
    return 0;
}
