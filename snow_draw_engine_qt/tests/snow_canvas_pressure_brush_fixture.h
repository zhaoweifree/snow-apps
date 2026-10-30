#pragma once

#include "snow_canvas_display_cache.h"
#include "snow_canvas_ffi_handles.h"
#include "snow_canvas_renderer.h"
#include "snow_canvas_viewport.h"

#include <QImage>
#include <QPainter>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace pressure_brush_fixture {

inline void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

inline SnowInputEvent pointer(SnowPointerEventType type, QPointF point, double pressure,
                              std::uint64_t timestamp) {
    SnowInputEvent event{};
    event.kind = SNOW_INPUT_EVENT_POINTER;
    event.pointer.pointer_id = 1;
    event.pointer.event_type = type;
    event.pointer.device = SNOW_POINTER_DEVICE_PEN;
    event.pointer.position_x = point.x();
    event.pointer.position_y = point.y();
    event.pointer.button =
        type == SNOW_POINTER_EVENT_MOVE ? SNOW_POINTER_BUTTON_NONE : SNOW_POINTER_BUTTON_PRIMARY;
    event.pointer.buttons = type == SNOW_POINTER_EVENT_UP ? 0 : 1;
    event.pointer.has_pressure = 1;
    event.pointer.pressure = pressure;
    event.pointer.timestamp_ms = timestamp;
    return event;
}

struct Engine {
    ScopedRuntimeHandle runtime;
    SnowCanvasViewport viewport;
    SnowCanvasDisplayCache cache;
    QSize size;
    QPointF endpoint;
    double lastPressure = 0.5;
    std::uint64_t timestamp = 1;

    explicit Engine(SnowBrushMode mode = SNOW_BRUSH_MODE_PRESSURE_SENSITIVE, double width = 16,
                    SnowStrokeStyle strokeStyle = SNOW_STROKE_STYLE_SOLID,
                    SnowColorRgba8 color = {220, 30, 90, 255}, QSize surface = {320, 256},
                    SnowColorRgba8 fill = {0, 0, 0, 0})
        : size(surface) {
        SnowStyleDefaults defaults{};
        require(snow_runtime_style_defaults_default(&defaults) == SNOW_OK,
                "create pressure brush default styles");
        defaults.free_draw.brush_mode = mode;
        defaults.free_draw.stroke_width = width;
        defaults.free_draw.stroke_style = strokeStyle;
        defaults.free_draw.stroke = color;
        defaults.free_draw.fill = fill;
        defaults.free_draw.opacity = 1;
        const SnowRuntimeConfig config{&defaults};
        auto viewportConfig = snow_canvas_viewport::defaultEngineConfig();
        viewportConfig.clear_color = {0, 0, 0, 0};
        ScopedChangedViewportList changed;
        require(snow_runtime_create_with_config(&config, runtime.outParam()) == SNOW_OK &&
                    viewport.create(runtime.get(), viewportConfig) &&
                    snow_viewport_set_surface_size(
                        runtime.get(), viewport.get(), static_cast<std::uint32_t>(size.width()),
                        static_cast<std::uint32_t>(size.height())) == SNOW_OK &&
                    snow_viewport_set_camera(runtime.get(), viewport.get(), size.width() / 2.0,
                                             size.height() / 2.0, 1.0) == SNOW_OK &&
                    snow_viewport_set_active_tool_ex(runtime.get(), viewport.get(),
                                                     SNOW_ACTIVE_TOOL_FREE_DRAW,
                                                     changed.outParam()) == SNOW_OK &&
                    cache.sync(runtime.get(), viewport.get()),
                "create pressure brush runtime, viewport and display cache");
    }

    void dispatch(const SnowInputEvent& event) {
        SnowInteractionOutput output{};
        ScopedChangedViewportList changed;
        require(snow_viewport_process_input_ex(runtime.get(), viewport.get(), &event, &output,
                                               changed.outParam()) == SNOW_OK,
                "dispatch pressure brush pointer event");
        endpoint = {event.pointer.position_x, event.pointer.position_y};
        lastPressure = event.pointer.pressure;
        timestamp = event.pointer.timestamp_ms;
    }

    void begin(QPointF point, double pressure = 0.5) {
        dispatch(pointer(SNOW_POINTER_EVENT_DOWN, point, pressure, timestamp));
    }

    void batch(const std::vector<SnowInputEvent>& events) {
        if (events.empty())
            return;
        SnowInteractionOutput output{};
        ScopedChangedViewportList changed;
        require(snow_viewport_process_pointer_move_batch_ex(
                    runtime.get(), viewport.get(), events.data(),
                    static_cast<std::uint32_t>(events.size()), &output,
                    changed.outParam()) == SNOW_OK,
                "dispatch pressure brush move batch");
        endpoint = {events.back().pointer.position_x, events.back().pointer.position_y};
        lastPressure = events.back().pointer.pressure;
        timestamp = events.back().pointer.timestamp_ms;
    }

    void move(QPointF point, double pressure) {
        dispatch(pointer(SNOW_POINTER_EVENT_MOVE, point, pressure, timestamp + 8));
    }

    void finish() {
        dispatch(pointer(SNOW_POINTER_EVENT_UP, endpoint, lastPressure, timestamp + 8));
        sync();
    }

    void sync() {
        require(cache.sync(runtime.get(), viewport.get()), "sync pressure brush display cache");
    }

    const SnowCanvasSceneItem& item() const {
        require(cache.sceneItemCount() == 1, "pressure brush fixture must contain one scene item");
        return cache.sceneItems()[0];
    }
};

inline void paint(QImage& target, const SceneDisplayInfo& info, const SnowCanvasSceneItem* items,
                  std::uint32_t itemCount, const QRegion& exposed,
                  const SnowCanvasDisplayCache* cache = nullptr) {
    if (exposed.isEmpty())
        return;
    QPainter painter(&target);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.setClipRegion(exposed);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    for (const QRect& rect : exposed)
        painter.fillRect(rect, Qt::transparent);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    snow_canvas_renderer::SceneRenderRequest request;
    request.painter = &painter;
    request.displayInfo = &info;
    request.sceneItems = items;
    request.sceneItemCount = itemCount;
    request.exposedRegion = exposed;
    request.displayCache = cache;
    request.clearBackgroundEnabled = false;
    snow_canvas_renderer::renderSceneItems(request);
}

inline QImage render(const SnowCanvasDisplayCache& cache, qreal dpr = 1) {
    const QSize logical(qRound(cache.sceneInfo().surface_width),
                        qRound(cache.sceneInfo().surface_height));
    QImage image(QSize(qRound(logical.width() * dpr), qRound(logical.height() * dpr)),
                 QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(dpr);
    image.fill(Qt::transparent);
    paint(image, cache.sceneInfo(), cache.sceneItems(), cache.sceneItemCount(),
          QRegion(QRect(QPoint(), logical)), &cache);
    return image;
}

inline double columnCoverage(const QImage& image, int x) {
    double coverage = 0;
    for (int y = 0; y < image.height(); ++y)
        coverage += qAlpha(image.pixel(x, y)) / 255.0;
    return coverage / image.devicePixelRatioF();
}

inline std::size_t geometryPayloadBytes(const SnowCanvasSceneItem& item) {
    std::size_t bytes = 0;
    for (const auto* chunks : {&item.pathChunks(), &item.pressurePathChunks()}) {
        bytes += chunks->capacity() * sizeof(SnowCanvasSceneItem::PathChunk);
        for (const auto& chunk : *chunks) {
            bytes += static_cast<std::size_t>(chunk.canvasPath.elementCount()) *
                     sizeof(QPainterPath::Element);
            bytes += chunk.spatialCells.capacity() * sizeof(std::int64_t);
        }
    }
    return bytes;
}

} // namespace pressure_brush_fixture
