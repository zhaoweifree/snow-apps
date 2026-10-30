#include "snow_canvas_pressure_brush_fixture.h"
#include "snow_canvas_pressure_mask_atlas.h"
#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"

#include <QApplication>
#include <QTransform>

#include <limits>
#include <memory>
#include <thread>

namespace {
using namespace pressure_brush_fixture;
namespace mask = snow_canvas_pressure_mask;

QPointF spiralPoint(std::size_t index) {
    const double step = static_cast<double>(index);
    const double radius = 65 + std::sin(step * 0.017) * 25;
    return {160 + std::cos(step * 0.041) * radius, 128 + std::sin(step * 0.041) * radius};
}

void append(Engine& engine, std::size_t start, std::size_t count) {
    std::vector<SnowInputEvent> events;
    for (std::size_t index = start; index < start + count; ++index) {
        events.push_back(pointer(SNOW_POINTER_EVENT_MOVE, spiralPoint(index),
                                 0.5 + std::sin(static_cast<double>(index) * 0.03) * 0.45,
                                 index * 8 + 1));
    }
    engine.batch(events);
    engine.sync();
}

QImage paintMask(const Engine& engine, const QColor& color, bool cached, qreal dpr,
                 const QRegion& clip, const QTransform& camera = {},
                 mask::Diagnostics* diagnostics = nullptr) {
    QImage image(QSize(qRound(engine.size.width() * dpr), qRound(engine.size.height() * dpr)),
                 QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(dpr);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setClipRegion(clip);
    painter.setOpacity(0.65);
    painter.setWorldTransform(camera);
    mask::DrawOptions options;
    options.cacheNamespace = &engine.cache;
    options.cacheEnabled = cached;
    options.diagnostics = diagnostics;
    mask::draw(painter, engine.item(), color, options);
    painter.end();
    return image;
}

QByteArray pressureDocumentSession() {
    Engine engine;
    engine.begin(spiralPoint(0));
    append(engine, 1, 256);
    engine.finish();
    std::size_t size = 0;
    require(snow_runtime_serialize_document_session(engine.runtime.get(), nullptr, 0, &size) ==
                    SNOW_OK &&
                size > 0 && size <= static_cast<std::size_t>(std::numeric_limits<int>::max()),
            "measure a pressure document session for lifecycle cleanup");
    QByteArray payload(static_cast<int>(size), Qt::Uninitialized);
    require(snow_runtime_serialize_document_session(engine.runtime.get(),
                                                    reinterpret_cast<std::uint8_t*>(payload.data()),
                                                    size, &size) == SNOW_OK,
            "serialize a pressure document session for lifecycle cleanup");
    return payload;
}

QImage exportPressureDocument(SnowCanvasRuntime& runtime) {
    const auto image = runtime.renderToImage(QRectF(0, 0, 320, 256), QSize(640, 512), {});
    const auto retained = mask::diagnosticsForCurrentThread();
    require(!image.isNull() && retained.retainedBytes > 0 && retained.entries > 0,
            "exporting a pressure document retains mask coverage on its rendering thread");
    return image;
}

void requireEmptyPressureCache(const char* message) {
    const auto retained = mask::diagnosticsForCurrentThread();
    require(retained.retainedBytes == 0 && retained.entries == 0, message);
}

void documentCleanupReleasesPressureMasksWithAttachedCanvas() {
    const auto payload = pressureDocumentSession();
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    canvas.resize(320, 256);
    for (int cycle = 0; cycle < 3; ++cycle) {
        require(runtime.restoreDocumentSession(payload), "restore a pressure canvas document");
        exportPressureDocument(runtime);
        require(runtime.clearDocumentPreservingViewports(), "clear a pressure canvas document");
        requireEmptyPressureCache("document cleanup releases pressure masks with a live canvas");
        require(!runtime.hasDocumentContent() && canvas.setViewportCamera(160, 128, 1),
                "pressure cleanup preserves the empty document's canvas viewport");
    }
}

void exportRuntimeCleanupReleasesPressureMasksOnPersistentWorker() {
    const auto payload = pressureDocumentSession();
    enum class Cleanup { ClearRenderState, ClearDocument, Reset, DestroyAsync, DestroyOwner };
    std::thread worker([&] {
        for (const auto cleanup : {Cleanup::ClearRenderState, Cleanup::ClearDocument,
                                   Cleanup::Reset, Cleanup::DestroyAsync, Cleanup::DestroyOwner}) {
            auto runtime = std::make_unique<SnowCanvasRuntime>();
            require(runtime->restoreDocumentSession(payload),
                    "restore a pressure export document without canvas clients");
            const auto expected = exportPressureDocument(*runtime);
            switch (cleanup) {
            case Cleanup::ClearRenderState:
                runtime->clearRenderState();
                break;
            case Cleanup::ClearDocument:
                require(runtime->clearDocumentPreservingViewports(),
                        "clear a pressure export document without canvas clients");
                break;
            case Cleanup::Reset:
                require(runtime->reset(), "reset a pressure export runtime");
                break;
            case Cleanup::DestroyAsync:
                runtime->destroyAsync();
                break;
            case Cleanup::DestroyOwner:
                runtime.reset();
                break;
            }
            requireEmptyPressureCache(
                "export runtime cleanup releases pressure masks before its worker exits");
            if (cleanup == Cleanup::ClearRenderState) {
                require(runtime->hasDocumentContent() &&
                            exportPressureDocument(*runtime) == expected,
                        "releasing pressure caches preserves the document and export pixels");
            }
            runtime.reset();
            requireEmptyPressureCache("export runtime destruction releases rebuilt pressure masks");
        }
    });
    worker.join();
}

void cachedAndFreshMasksMatchAcrossClipsColorsAndTransforms() {
    const QRegion full(QRect(0, 0, 320, 256));
    const QRegion sparse = QRegion(QRect(48, 42, 95, 81)) + QRegion(QRect(170, 135, 92, 77));
    for (const auto style :
         {SNOW_STROKE_STYLE_SOLID, SNOW_STROKE_STYLE_DASHED, SNOW_STROKE_STYLE_DOTTED}) {
        Engine engine(SNOW_BRUSH_MODE_PRESSURE_SENSITIVE, 32, style);
        engine.begin(spiralPoint(0));
        for (std::size_t start = 1; start < 1025; start += 64)
            append(engine, start, 64);
        for (const qreal dpr : {1.0, 1.5, 2.0}) {
            for (const auto& camera : {QTransform(), QTransform::fromTranslate(0.25, -0.5),
                                       QTransform(0.85, 0.0, 0.0, 0.85, 22.25, 17.5)}) {
                mask::clearCacheForCurrentThread();
                mask::Diagnostics cold;
                const auto initial =
                    paintMask(engine, QColor(220, 30, 90, 128), true, dpr, full, camera, &cold);
                mask::Diagnostics warm;
                require(paintMask(engine, QColor(220, 30, 90, 128), true, dpr, full, camera,
                                  &warm) == initial &&
                            warm.rasterizedMasks == 0 && warm.unionTileHits > 0,
                        "stationary frames reuse pressure coverage without changing pixels");
                for (const auto& color : {QColor(220, 30, 90, 128), QColor(30, 190, 220, 210)}) {
                    for (const auto& clip : {full, sparse}) {
                        require(paintMask(engine, color, true, dpr, clip, camera) ==
                                    paintMask(engine, color, false, dpr, clip, camera),
                                "cached pressure masks match fresh coverage for sparse clips, "
                                "recoloring, alpha, DPR and fractional camera placement");
                    }
                }
            }
        }
    }
}

void mutableTailsDoNotReuseStaleUnionTiles() {
    Engine engine(SNOW_BRUSH_MODE_PRESSURE_SENSITIVE, 32, SNOW_STROKE_STYLE_DOTTED);
    engine.begin(spiralPoint(0));
    const QRegion full(QRect(0, 0, 320, 256));
    for (std::size_t start = 1; start < 257; start += 64)
        append(engine, start, 64);
    mask::clearCacheForCurrentThread();
    auto previous = paintMask(engine, QColor(220, 30, 90, 128), true, 2, full);
    for (std::size_t start = 257; start < 321; start += 8) {
        append(engine, start, 8);
        const auto current = paintMask(engine, QColor(220, 30, 90, 128), true, 2, full);
        require(current == paintMask(engine, QColor(220, 30, 90, 128), false, 2, full),
                "tail replacements invalidate cached union tiles without stale old coverage");
        require(current != previous, "the mutable pressure tail fixture changes visible coverage");
        previous = current;
    }
    engine.finish();
    require(paintMask(engine, QColor(220, 30, 90, 128), true, 2, full) ==
                paintMask(engine, QColor(220, 30, 90, 128), false, 2, full),
            "committed pressure geometry matches a fresh mask after preview replacement");
}

void cacheBudgetAndCleanupBoundRetainedMaskStorage() {
    Engine engine;
    engine.begin(spiralPoint(0));
    append(engine, 1, 256);
    const QRegion full(QRect(0, 0, 320, 256));
    mask::clearCacheForCurrentThread();
    constexpr std::size_t budget = 32 * 1024;
    mask::setCacheByteBudgetForCurrentThread(budget);
    mask::resetDiagnosticsForCurrentThread();
    for (int index = 0; index < 4; ++index)
        paintMask(engine, QColor(100 + index * 20, 30, 90, 128), true, 2, full);
    const auto retained = mask::diagnosticsForCurrentThread();
    require(retained.retainedBytes <= budget && retained.evictions > 0,
            "pressure tile coverage obeys its shared retained byte budget");
    mask::clearCacheForCurrentThread();
    const auto cleared = mask::diagnosticsForCurrentThread();
    require(cleared.retainedBytes == 0 && cleared.entries == 0,
            "clearing render state releases pressure coverage storage");
    paintMask(engine, QColor(220, 30, 90, 128), false, 2, full);
    require(mask::diagnosticsForCurrentThread().retainedBytes == 0,
            "the fresh reference draw retains no mask cache storage");
    mask::setCacheByteBudgetForCurrentThread(mask::kDefaultBudgetBytes);
}

SnowCanvasSceneItem overlappingContours(const QRectF& rectangle) {
    SnowCanvasSceneItem item;
    item.kind = SNOW_SCENE_DISPLAY_ITEM_ARROW;
    item.is_free_draw = 1;
    item.brush_mode = SNOW_BRUSH_MODE_PRESSURE_SENSITIVE;
    std::vector<SnowArrowPathCommand> commands;
    std::vector<SnowPathChunk> chunks;
    const QPointF vertices[] = {rectangle.topLeft(), rectangle.topRight(), rectangle.bottomRight(),
                                rectangle.bottomLeft(), rectangle.topLeft()};
    for (std::uint32_t index = 0; index < 32; ++index) {
        SnowPathChunk chunk{};
        chunk.stable_id = index + 1;
        chunk.command_start = static_cast<std::uint32_t>(commands.size());
        chunk.command_offset = chunk.command_start;
        chunk.command_count = 5;
        chunk.start_x = chunk.min_x = rectangle.left();
        chunk.start_y = chunk.min_y = rectangle.top();
        chunk.max_x = rectangle.right();
        chunk.max_y = rectangle.bottom();
        for (int vertex = 0; vertex < 5; ++vertex) {
            SnowArrowPathCommand command{};
            command.kind =
                vertex == 0 ? SNOW_ARROW_PATH_COMMAND_MOVE_TO : SNOW_ARROW_PATH_COMMAND_LINE_TO;
            command.point = {vertices[vertex].x(), vertices[vertex].y()};
            commands.push_back(command);
        }
        chunks.push_back(chunk);
    }
    SnowPathChunkRange range{};
    range.insert_chunk_count = static_cast<std::uint32_t>(chunks.size());
    require(item.applyPathGeometryPatch(0, 1, &range, 1, chunks.data(),
                                        static_cast<std::uint32_t>(chunks.size()), commands.data(),
                                        static_cast<std::uint32_t>(commands.size()), false, true,
                                        SNOW_PATH_GEOMETRY_ROLE_PRESSURE_OUTLINE),
            "install dense overlapping pressure contours");
    return item;
}

QImage paintSyntheticMask(const SnowCanvasSceneItem& item, bool cached,
                          mask::Diagnostics* diagnostics = nullptr) {
    QImage image(QSize(256, 64), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setClipRect(image.rect());
    mask::DrawOptions options;
    options.cacheNamespace = &item;
    options.cacheEnabled = cached;
    options.diagnostics = diagnostics;
    mask::draw(painter, item, QColor(220, 30, 90, 128), options);
    painter.end();
    return image;
}

void denseOverlapRetainsCompletedTilesWithinSharedBudget() {
    // Four colored tiles fit comfortably within half the budget, but the 128 overlapping
    // chunk masks exceed the entire budget. Keep uncovered strips so tile saturation cannot
    // skip these masks. A single LRU evicts completed tiles as it walks subsequent tiles.
    const auto item = overlappingContours(QRectF(-1, 8, 259, 48));
    constexpr std::size_t budget = 256 * 1024;
    mask::clearCacheForCurrentThread();
    mask::setCacheByteBudgetForCurrentThread(budget);
    mask::Diagnostics cold;
    const auto initial = paintSyntheticMask(item, true, &cold);
    require(cold.rasterizedMasks == 128 && cold.unionRebuilds == 4 && cold.evictions > 0 &&
                cold.retainedBytes <= budget,
            "dense overlap forces mask eviction while retaining a bounded coverage atlas");
    mask::Diagnostics warm;
    require(paintSyntheticMask(item, true, &warm) == initial && warm.unionTileHits == 4 &&
                warm.rasterizedMasks == 0 && warm.unionRebuilds == 0 && warm.evictions == 0 &&
                warm.retainedBytes <= budget,
            "completed pressure tiles survive mask churn for an unchanged full repaint");
    require(paintSyntheticMask(item, false) == initial,
            "prioritized tile retention preserves fresh pressure coverage pixels");
    mask::clearCacheForCurrentThread();
    mask::setCacheByteBudgetForCurrentThread(mask::kDefaultBudgetBytes);
}

void saturatedCoverageSkipsMasksWithoutLosingCandidateInvalidation() {
    auto item = overlappingContours(QRectF(-1, -1, 259, 67));
    mask::clearCacheForCurrentThread();
    mask::Diagnostics cold;
    const auto initial = paintSyntheticMask(item, true, &cold);
    QImage expected(initial.size(), initial.format());
    expected.fill(QColor(220, 30, 90, 128));
    require(initial == expected && paintSyntheticMask(item, false) == expected &&
                cold.queriedChunks == 128 && cold.rasterizedMasks == 4 && cold.unionCopies == 4 &&
                cold.unionRebuilds == 4,
            "fully opaque MAX coverage skips remaining masks without changing any pixel");

    // Change a candidate skipped by saturation. Its coverage is redundant, but its new Qt
    // identity must still invalidate every union tile; the complete candidate list is retained.
    SnowArrowPathCommand commands[5]{};
    const QPointF vertices[] = {{-1, 0}, {258, 0}, {258, 64}, {-1, 64}, {-1, 0}};
    for (int index = 0; index < 5; ++index) {
        commands[index].kind =
            index == 0 ? SNOW_ARROW_PATH_COMMAND_MOVE_TO : SNOW_ARROW_PATH_COMMAND_LINE_TO;
        commands[index].point = {vertices[index].x(), vertices[index].y()};
    }
    SnowPathChunk chunk{};
    chunk.stable_id = 32;
    chunk.command_start = 31 * 5;
    chunk.command_count = 5;
    chunk.start_x = chunk.min_x = -1;
    chunk.start_y = chunk.min_y = 0;
    chunk.max_x = 258;
    chunk.max_y = 64;
    SnowPathChunkRange range{};
    range.start = 31;
    range.delete_count = 1;
    range.insert_chunk_count = 1;
    require(item.applyPathGeometryPatch(1, 2, &range, 1, &chunk, 1, commands, 5, false, false,
                                        SNOW_PATH_GEOMETRY_ROLE_PRESSURE_OUTLINE),
            "replace a pressure contour skipped after coverage saturated");
    mask::Diagnostics changed;
    require(paintSyntheticMask(item, true, &changed) == expected && changed.unionTileHits == 0 &&
                changed.unionRebuilds == 4 && changed.rasterizedMasks == 0 &&
                changed.unionCopies == 4,
            "skipped pressure candidates retain their union invalidation identities");
    mask::clearCacheForCurrentThread();
}

void defaultClipKeepsNativeDeviceEdgesUnderFractionalCameras() {
    SnowCanvasSceneItem item;
    item.kind = SNOW_SCENE_DISPLAY_ITEM_ARROW;
    item.is_free_draw = 1;
    item.brush_mode = SNOW_BRUSH_MODE_PRESSURE_SENSITIVE;
    SnowArrowPathCommand commands[5]{};
    const QPointF vertices[] = {
        {-4096, -4096}, {4096, -4096}, {4096, 4096}, {-4096, 4096}, {-4096, -4096}};
    for (int index = 0; index < 5; ++index) {
        commands[index].kind =
            index == 0 ? SNOW_ARROW_PATH_COMMAND_MOVE_TO : SNOW_ARROW_PATH_COMMAND_LINE_TO;
        commands[index].point = {vertices[index].x(), vertices[index].y()};
    }
    SnowPathChunk chunk{};
    chunk.stable_id = 1;
    chunk.command_count = 5;
    chunk.start_x = chunk.min_x = -4096;
    chunk.start_y = chunk.min_y = -4096;
    chunk.max_x = chunk.max_y = 4096;
    SnowPathChunkRange range{};
    range.insert_chunk_count = 1;
    require(item.applyPathGeometryPatch(0, 1, &range, 1, &chunk, 1, commands, 5, false, true,
                                        SNOW_PATH_GEOMETRY_ROLE_PRESSURE_OUTLINE),
            "install one opaque contour for native clip comparison");
    const QRegion sparse = QRegion(QRect(0, 0, 10, 40)) + QRegion(QRect(52, 23, 95, 83)) +
                           QRegion(QRect(170, 135, 92, 77)) + QRegion(QRect(315, 0, 5, 256));
    const auto image = [&](qreal dpr, const QTransform& camera, bool native, bool cached) {
        QImage target(QSize(qRound(320 * dpr), qRound(256 * dpr)),
                      QImage::Format_ARGB32_Premultiplied);
        target.setDevicePixelRatio(dpr);
        target.fill(Qt::transparent);
        QPainter painter(&target);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setWindow(0, 0, 320, 256);
        painter.setViewport(3, 5, 320, 256);
        painter.setClipRegion(sparse);
        painter.setWorldTransform(camera);
        const QColor color(220, 30, 90);
        if (native) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(color);
            painter.drawPath(item.pressurePathChunks().front().canvasPath);
        } else {
            mask::DrawOptions options;
            options.cacheNamespace = &item;
            options.cacheEnabled = cached;
            // Exercise the default API, without a pre-camera physicalClip override.
            mask::draw(painter, item, color, options);
        }
        painter.end();
        return target;
    };
    for (const qreal dpr : {1.0, 1.5, 2.0}) {
        for (const qreal zoom : {0.5, 0.85, 1.0, 1.25, 2.0}) {
            QTransform camera;
            camera.translate(22.25, 17.5);
            camera.scale(zoom, zoom);
            mask::clearCacheForCurrentThread();
            const auto expected = image(dpr, camera, true, false);
            require(image(dpr, camera, false, false) == expected &&
                        image(dpr, camera, false, true) == expected,
                    "default pressure clip must retain every native opaque device edge pixel "
                    "under fractional camera, DPR and viewport transforms");
        }
    }
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    documentCleanupReleasesPressureMasksWithAttachedCanvas();
    exportRuntimeCleanupReleasesPressureMasksOnPersistentWorker();
    cachedAndFreshMasksMatchAcrossClipsColorsAndTransforms();
    mutableTailsDoNotReuseStaleUnionTiles();
    cacheBudgetAndCleanupBoundRetainedMaskStorage();
    denseOverlapRetainsCompletedTilesWithinSharedBudget();
    saturatedCoverageSkipsMasksWithoutLosingCandidateInvalidation();
    defaultClipKeepsNativeDeviceEdgesUnderFractionalCameras();
    return 0;
}
