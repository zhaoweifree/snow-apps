#include "snow_canvas_pressure_brush_fixture.h"
#include "snow_canvas_export.h"

#include <QGuiApplication>

#include <cmath>
#include <vector>

namespace {
using namespace pressure_brush_fixture;

void nominalAndVariableWidthsMatchTheWidthEditor() {
    for (const qreal dpr : {1.0, 2.0}) {
        for (const SnowBrushMode mode :
             {SNOW_BRUSH_MODE_REGULAR, SNOW_BRUSH_MODE_PRESSURE_SENSITIVE}) {
            Engine engine(mode, 16);
            engine.begin({20, 100}, 0.5);
            for (int x = 21; x <= 280; ++x)
                engine.move({static_cast<double>(x), 100}, 0.5);
            engine.finish();
            const QImage image = render(engine.cache, dpr);
            require(std::abs(columnCoverage(image, qRound(140 * dpr)) - 16) < 1,
                    "pressure .5 and regular strokes must preserve the nominal editor width");
        }
        Engine pressure;
        pressure.begin({20, 100}, 0.1);
        for (int x = 21; x <= 280; ++x)
            pressure.move({static_cast<double>(x), 100}, x < 130 ? 0.1 : 0.9);
        pressure.finish();
        require(!pressure.item().pressurePathChunks().empty(),
                "pressure input must cross the ABI as owned contour chunks");
        const QImage image = render(pressure.cache, dpr);
        require(columnCoverage(image, qRound(220 * dpr)) >
                    columnCoverage(image, qRound(60 * dpr)) * 2,
                "tablet pressure must vary native rendered width");
        Engine regular(SNOW_BRUSH_MODE_REGULAR);
        regular.begin({20, 100}, 0.1);
        for (int x = 21; x <= 280; ++x)
            regular.move({static_cast<double>(x), 100}, x < 130 ? 0.1 : 0.9);
        regular.finish();
        const QImage uniform = render(regular.cache, dpr);
        require(std::abs(columnCoverage(uniform, qRound(60 * dpr)) -
                         columnCoverage(uniform, qRound(220 * dpr))) < 0.1,
                "regular brush geometry must stay uniform under changing tablet pressure");
    }
}

void mouseSpeedProducesObviousWidthVariation() {
    for (const double width : {4.0, 16.0, 32.0}) {
        for (const SnowBrushMode mode :
             {SNOW_BRUSH_MODE_REGULAR, SNOW_BRUSH_MODE_PRESSURE_SENSITIVE}) {
            Engine engine(mode, width, SNOW_STROKE_STYLE_SOLID, {220, 30, 90, 255}, {1024, 256});
            const auto mouse = [&](SnowPointerEventType type, double x) {
                auto event = pointer(type, {x, 100}, 0, engine.timestamp + 8);
                event.pointer.device = SNOW_POINTER_DEVICE_MOUSE;
                event.pointer.has_pressure = 0;
                engine.dispatch(event);
            };
            mouse(SNOW_POINTER_EVENT_DOWN, 20);
            for (int i = 1; i <= 80; ++i)
                mouse(SNOW_POINTER_EVENT_MOVE, 20 + i * width / 16);
            for (int i = 1; i <= 48; ++i)
                mouse(SNOW_POINTER_EVENT_MOVE, 20 + 5 * width + i * width / 2);
            for (const bool finalize : {false, true}) {
                if (finalize)
                    mouse(SNOW_POINTER_EVENT_UP, engine.endpoint.x());
                engine.sync();
                for (const qreal dpr : {1.0, 1.5, 2.0}) {
                    const auto image = render(engine.cache, dpr);
                    // Measure settled speed plateaus, away from acceleration and endpoint caps.
                    const double slow = columnCoverage(image, qRound((20 + 3.75 * width) * dpr));
                    const double fast = columnCoverage(image, qRound((20 + 27 * width) * dpr));
                    if (mode == SNOW_BRUSH_MODE_PRESSURE_SENSITIVE) {
                        require(
                            slow > fast * 2 && fast < width * 0.75,
                            "mouse speed must visibly thin live and finalized pressure strokes");
                    } else {
                        require(std::abs(slow - width) < 0.5 && std::abs(fast - width) < 0.5,
                                "mouse speed must preserve the regular brush's nominal width");
                    }
                }
            }
        }
    }
}

void stationaryPressurePeakSurvivesPositionSimplification() {
    Engine engine;
    engine.begin({20, 100}, 0.1);
    for (int x = 21; x <= 110; ++x)
        engine.move({static_cast<double>(x), 100}, 0.1);
    engine.move({110, 100}, 0.95);
    engine.move({110, 100}, 0.1);
    for (int x = 111; x <= 280; ++x)
        engine.move({static_cast<double>(x), 100}, 0.1);
    engine.finish();
    const QImage image = render(engine.cache);
    double peak = 0;
    for (int x = 90; x <= 125; ++x)
        peak = std::max(peak, columnCoverage(image, x));
    require(peak > columnCoverage(image, 60) * 2,
            "stationary pressure peaks must survive a simplified straight centerline");
}

void stationaryDotsRetainPressurePeaksForEveryPattern() {
    for (const auto style :
         {SNOW_STROKE_STYLE_SOLID, SNOW_STROKE_STYLE_DASHED, SNOW_STROKE_STYLE_DOTTED}) {
        Engine engine(SNOW_BRUSH_MODE_PRESSURE_SENSITIVE, 16, style);
        engine.begin({40, 100}, 0.2);
        engine.move({40, 100}, 0.9);
        engine.move({40, 100}, 0.2);
        engine.sync();
        const auto preview = render(engine.cache);
        engine.finish();
        for (const qreal dpr : {1.0, 1.5, 2.0}) {
            const auto image = render(engine.cache, dpr);
            require(columnCoverage(image, qRound(40 * dpr)) > 19.0,
                    "stationary pressure peaks retain their painted width for every pattern");
            require(qAlpha(image.pixel(qRound(40 * dpr), qRound(109 * dpr))) > 200 &&
                        qAlpha(image.pixel(qRound(40 * dpr), qRound(112 * dpr))) == 0,
                    "stationary peak coverage paints the peak without exceeding its radius");
        }
        require(preview == render(engine.cache),
                "stationary patterned pressure dots retain identical preview and committed ink");
    }
}

void batchedShiftStrokeRetainsItsPressureHistory() {
    Engine engine;
    auto press = pointer(SNOW_POINTER_EVENT_DOWN, {20, 100}, 0.2, 100);
    press.pointer.modifiers.shift = 1;
    engine.dispatch(press);
    std::vector<SnowInputEvent> events;
    const struct {
        double x;
        double pressure;
    } samples[] = {{60, 0.9}, {60, 0.2}, {100, 0.2}, {200, 0.2}};
    for (const auto& sample : samples) {
        auto event = pointer(SNOW_POINTER_EVENT_MOVE, {sample.x, 100}, sample.pressure,
                             108 + static_cast<std::uint64_t>(events.size()) * 8);
        event.pointer.modifiers.shift = 1;
        events.push_back(event);
    }
    engine.batch(events);
    engine.sync();
    const auto preview = render(engine.cache);
    engine.finish();
    for (const qreal dpr : {1.0, 1.5, 2.0}) {
        const auto image = render(engine.cache, dpr);
        require(columnCoverage(image, qRound(60 * dpr)) > 19.0 &&
                    columnCoverage(image, qRound(170 * dpr)) < 12.0,
                "a batched Shift stroke retains earlier pressure peaks and a thin final tail");
    }
    require(preview == render(engine.cache),
            "Shift pressure history renders identically before and after finalization");
}

SnowCanvasSceneItem syntheticCrossing() {
    SnowCanvasSceneItem item;
    item.kind = SNOW_SCENE_DISPLAY_ITEM_ARROW;
    item.is_free_draw = 1;
    item.brush_mode = SNOW_BRUSH_MODE_PRESSURE_SENSITIVE;
    item.stroke = {220, 30, 90, 128};
    item.stroke_width = 20;
    item.opacity = 0.5;
    item.arrow_stroke_style = SNOW_STROKE_STYLE_SOLID;
    item.blend_mode = SNOW_BLEND_MODE_NORMAL;
    item.center_x = 152;
    item.center_y = 120;
    item.width = 256;
    item.height = 208;
    SnowArrowPathCommand center[2]{};
    center[0].kind = SNOW_ARROW_PATH_COMMAND_MOVE_TO;
    center[0].point = {24, 100};
    center[1].kind = SNOW_ARROW_PATH_COMMAND_LINE_TO;
    center[1].point = {280, 100};
    SnowPathChunk centerChunk{};
    centerChunk.stable_id = 1;
    centerChunk.command_count = 2;
    centerChunk.start_x = centerChunk.min_x = 24;
    centerChunk.start_y = centerChunk.min_y = centerChunk.max_y = 100;
    centerChunk.max_x = 280;
    SnowPathChunkRange centerRange{};
    centerRange.insert_chunk_count = 1;
    require(
        item.applyPathGeometryPatch(0, 1, &centerRange, 1, &centerChunk, 1, center, 2, false, true),
        "install synthetic pressure centerline");
    const QRectF rectangles[] = {{24, 90, 136, 20}, {156, 90, 124, 20}, {140, 24, 20, 200}};
    std::vector<SnowArrowPathCommand> commands;
    std::vector<SnowPathChunk> chunks;
    for (const QRectF& rectangle : rectangles) {
        SnowPathChunk chunk{};
        chunk.stable_id = chunks.size() + 1;
        chunk.command_start = static_cast<std::uint32_t>(commands.size());
        chunk.command_offset = chunk.command_start;
        chunk.command_count = 5;
        chunk.start_x = chunk.min_x = rectangle.left();
        chunk.start_y = chunk.min_y = rectangle.top();
        chunk.max_x = rectangle.right();
        chunk.max_y = rectangle.bottom();
        const QPointF vertices[] = {rectangle.topLeft(), rectangle.topRight(),
                                    rectangle.bottomRight(), rectangle.bottomLeft(),
                                    rectangle.topLeft()};
        for (int index = 0; index < 5; ++index) {
            SnowArrowPathCommand command{};
            command.kind =
                index == 0 ? SNOW_ARROW_PATH_COMMAND_MOVE_TO : SNOW_ARROW_PATH_COMMAND_LINE_TO;
            command.point = {vertices[index].x(), vertices[index].y()};
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
            "install distinct pressure contour geometry role");
    return item;
}

void chunkSeamsAndCrossingsApplyOpacityOnce() {
    const auto item = syntheticCrossing();
    SceneDisplayInfo info;
    info.surface_width = 320;
    info.surface_height = 256;
    info.camera_center_x = 160;
    info.camera_center_y = 128;
    for (qreal dpr : {1.0, 2.0}) {
        QImage image(QSize(qRound(320 * dpr), qRound(256 * dpr)),
                     QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(dpr);
        image.fill(Qt::transparent);
        paint(image, info, &item, 1, QRegion(QRect(0, 0, 320, 256)));
        for (const QPointF point :
             {QPointF(60, 100), QPointF(158, 100), QPointF(150, 100), QPointF(150, 180)}) {
            require(std::abs(qAlpha(image.pixel(qRound(point.x() * dpr), qRound(point.y() * dpr))) -
                             64) <= 1,
                    "translucent chunk seams and self-crossings must receive opacity only once");
        }
        QImage tiled(image.size(), image.format());
        tiled.setDevicePixelRatio(dpr);
        tiled.fill(Qt::transparent);
        for (int x = 0; x < 320; x += 32)
            paint(tiled, info, &item, 1, QRegion(QRect(x, 0, 32, 256)));
        require(tiled == image, "pressure coverage must be identical across exposed-region seams");
    }
}

void strokePatternsKeepTheGlobalPhase() {
    for (const SnowStrokeStyle style : {SNOW_STROKE_STYLE_DASHED, SNOW_STROKE_STYLE_DOTTED}) {
        Engine engine(SNOW_BRUSH_MODE_PRESSURE_SENSITIVE, 8, style);
        engine.begin({20, 100});
        for (int x = 21; x <= 290; ++x)
            engine.move({static_cast<double>(x), 100}, 0.5);
        engine.finish();
        const QImage image = render(engine.cache);
        const double period = style == SNOW_STROKE_STYLE_DOTTED ? 16 : 35.2;
        for (int cycle = 1; cycle <= 6; ++cycle) {
            const double origin = 20 + cycle * period;
            const int ink = qRound(origin + (style == SNOW_STROKE_STYLE_DOTTED ? 0 : 8));
            const int gap = qRound(origin + (style == SNOW_STROKE_STYLE_DOTTED ? 8 : 25.6));
            require(qAlpha(image.pixel(ink, 100)) > 240 && qAlpha(image.pixel(gap, 100)) < 5,
                    "native pressure dashes and dots must keep phase across pressure samples");
        }
        QImage tiled(image.size(), image.format());
        tiled.fill(Qt::transparent);
        for (int x = 0; x < 320; x += 23)
            paint(tiled, engine.cache.sceneInfo(), engine.cache.sceneItems(),
                  engine.cache.sceneItemCount(), QRegion(QRect(x, 0, 23, 256)), &engine.cache);
        require(tiled == image, "clipping must not restart dash or dot phase");
    }
}

void fractionalCamerasPreservePartialRepaintEdges() {
    const auto item = syntheticCrossing();
    SceneDisplayInfo info;
    info.surface_width = 320;
    info.surface_height = 256;
    info.camera_center_x = 159.63;
    info.camera_center_y = 128.28;
    for (const qreal dpr : {1.0, 1.5, 2.0}) {
        for (const double zoom : {0.73, 0.85, 1.17, 2.0}) {
            info.camera_zoom = zoom;
            QImage full(QSize(qRound(320 * dpr), qRound(256 * dpr)),
                        QImage::Format_ARGB32_Premultiplied);
            full.setDevicePixelRatio(dpr);
            full.fill(Qt::transparent);
            paint(full, info, &item, 1, QRegion(QRect(0, 0, 320, 256)));
            QImage partial(full.size(), full.format());
            partial.setDevicePixelRatio(dpr);
            partial.fill(Qt::transparent);
            for (int x = 0; x < 320; x += 23) {
                for (int y = 0; y < 256; y += 31) {
                    paint(partial, info, &item, 1,
                          QRegion(QRect(x, y, qMin(23, 320 - x), qMin(31, 256 - y))));
                }
            }
            require(partial == full,
                    "fractional camera zoom and DPR preserve every partial-repaint edge pixel");
        }
    }
}

void closedFillExportAndPeerViewportsShareGeometry() {
    Engine engine(SNOW_BRUSH_MODE_PRESSURE_SENSITIVE, 8, SNOW_STROKE_STYLE_SOLID,
                  {220, 30, 90, 128}, {320, 256}, {30, 90, 220, 96});
    engine.begin({70, 60});
    const QPointF corners[] = {{250, 60}, {250, 190}, {70, 190}, {70, 60}};
    QPointF previous(70, 60);
    for (const auto& corner : corners) {
        for (int step = 1; step <= 90; ++step) {
            const double t = step / 90.0;
            engine.move(previous + (corner - previous) * t, 0.2 + t * 0.6);
        }
        previous = corner;
    }
    engine.finish();
    require(engine.item().pathIsClosed(), "pressure strokes must retain closed centerline fills");
    require(qAlpha(render(engine.cache).pixel(150, 120)) == 96,
            "closed pressure contours must preserve the separate path fill");
    SnowCanvasViewport peer;
    auto config = snow_canvas_viewport::defaultEngineConfig();
    config.clear_color = {0, 0, 0, 0};
    require(peer.create(engine.runtime.get(), config) &&
                snow_viewport_set_surface_size(engine.runtime.get(), peer.get(), 320, 256) ==
                    SNOW_OK &&
                snow_viewport_set_camera(engine.runtime.get(), peer.get(), 160, 128, 1) == SNOW_OK,
            "create a peer pressure brush viewport");
    SnowCanvasDisplayCache peerCache;
    require(peerCache.sync(engine.runtime.get(), peer.get()), "sync shared pressure document");
    for (const qreal dpr : {1.0, 2.0}) {
        const QImage view = render(engine.cache, dpr);
        require(render(peerCache, dpr) == view, "peer viewports must share native pressure ink");
        QImage exported =
            snow_canvas_export::renderToImage(engine.runtime.get(), QRectF(0, 0, 320, 256),
                                              QSize(qRound(320 * dpr), qRound(256 * dpr)), {});
        require(!exported.isNull(), "export pressure brush document");
        exported.setDevicePixelRatio(dpr);
        require(exported == view,
                "DPR view rendering and scaled export must agree pixel for pixel");
    }
}

void warmRenderingAndIncrementalPatchesRetainThePrefix() {
    Engine engine(SNOW_BRUSH_MODE_PRESSURE_SENSITIVE, 8);
    engine.begin({20, 100});
    const auto sample = [](int index) {
        return QPointF(20 + index * 1.5,
                       100 + std::sin(index * 0.21) * 24 + std::sin(index * 1.7) * 4);
    };
    for (int index = 1; index <= 900; ++index)
        engine.move(sample(index), 0.5 + std::sin(index * 0.08) * 0.4);
    engine.sync();
    const auto before = engine.item().pressurePathChunks();
    require(before.size() > 2, "long pressure fixture must span several independent chunks");
    const auto revision = engine.item().pressureGeometryRevision();
    const auto bytes = geometryPayloadBytes(engine.item());
    const auto first = render(engine.cache);
    require(render(engine.cache) == first &&
                engine.cache.sync(engine.runtime.get(), engine.viewport.get()) &&
                engine.item().pressureGeometryRevision() == revision &&
                geometryPayloadBytes(engine.item()) == bytes,
            "warm paints and no-op sync must not replace pressure geometry or grow its payload");
    for (int index = 901; index <= 916; ++index)
        engine.move(sample(index), 0.5 + std::sin(index * 0.08) * 0.4);
    engine.sync();
    const auto& after = engine.item().pressurePathChunks();
    require(after.size() >= before.size() && engine.item().pressureGeometryRevision() > revision,
            "incremental input must advance only the live pressure geometry");
    require(before.front().stableId == after.front().stableId &&
                before.front().canvasPath == after.front().canvasPath,
            "appending to a long pressure stroke must retain stable prefix geometry");
    engine.finish();
    SnowCanvasViewport fresh;
    auto config = snow_canvas_viewport::defaultEngineConfig();
    config.clear_color = {0, 0, 0, 0};
    require(fresh.create(engine.runtime.get(), config) &&
                snow_viewport_set_surface_size(engine.runtime.get(), fresh.get(), 320, 256) ==
                    SNOW_OK &&
                snow_viewport_set_camera(engine.runtime.get(), fresh.get(), 160, 128, 1) == SNOW_OK,
            "create full reconstruction pressure viewport");
    SnowCanvasDisplayCache freshCache;
    require(freshCache.sync(engine.runtime.get(), fresh.get()) &&
                render(freshCache) == render(engine.cache),
            "final incremental patches must render like a fresh document reconstruction");
}
} // namespace

int main(int argc, char** argv) {
    QGuiApplication application(argc, argv);
    nominalAndVariableWidthsMatchTheWidthEditor();
    mouseSpeedProducesObviousWidthVariation();
    stationaryPressurePeakSurvivesPositionSimplification();
    stationaryDotsRetainPressurePeaksForEveryPattern();
    batchedShiftStrokeRetainsItsPressureHistory();
    chunkSeamsAndCrossingsApplyOpacityOnce();
    strokePatternsKeepTheGlobalPhase();
    fractionalCamerasPreservePartialRepaintEdges();
    closedFillExportAndPeerViewportsShareGeometry();
    warmRenderingAndIncrementalPatchesRetainThePrefix();
    return 0;
}
