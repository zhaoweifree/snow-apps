#include "snow_canvas_display_item.h"
#include "snow_canvas_render_geometry.h"
#include "snow_canvas_renderer.h"
#include "snow_canvas_stroke_rendering.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QPainter>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

constexpr int width = 1920;
constexpr int height = 1080;

std::vector<SnowCanvasSceneItem> makeItems(int count, SnowStrokeRenderingStyle style) {
    std::vector<SnowCanvasSceneItem> items;
    items.reserve(static_cast<std::size_t>(count));
    const int columns = static_cast<int>(std::ceil(std::sqrt(count * 16.0 / 9.0)));
    const int rows = (count + columns - 1) / columns;
    const double cellWidth = static_cast<double>(width) / columns;
    const double cellHeight = static_cast<double>(height) / rows;
    for (int index = 0; index < count; ++index) {
        SnowSceneDisplayItem source{};
        source.kind = SNOW_SCENE_DISPLAY_ITEM_DRAW_RECT;
        source.element_id = {static_cast<std::uint32_t>(index), 1};
        source.center_x = -width / 2.0 + (index % columns + 0.5) * cellWidth;
        source.center_y = -height / 2.0 + (index / columns + 0.5) * cellHeight;
        source.width = cellWidth * 0.75;
        source.height = cellHeight * 0.7;
        source.rotation = index % 7 == 0 ? 0.08 : 0;
        source.stroke = {30, 50, 80, 170};
        source.stroke_width = 1.5;
        source.fill = {80, 160, 220, 70};
        source.fill_style = index % 13 == 0  ? SNOW_FILL_STYLE_CROSS_LINE
                            : index % 7 == 0 ? SNOW_FILL_STYLE_LINE
                                             : SNOW_FILL_STYLE_SOLID;
        source.opacity = 0.8;
        source.rect_shape = static_cast<std::uint8_t>(index % 3);
        source.stroke_rendering_style = style;
        source.stroke_rendering_seed = static_cast<std::uint32_t>(index * 142857);
        if (index % 4 == 0)
            source.corner_radii = {2, 2, 2, 2};
        if (index % 5 == 0)
            source.stroke_style = SNOW_STROKE_STYLE_DASHED;
        std::array<SnowArrowPoint, 4> points{};
        if (index % 4 == 3) {
            source.kind = SNOW_SCENE_DISPLAY_ITEM_ARROW;
            source.arrow_type = static_cast<SnowArrowType>((index / 4) % 3);
            source.arrow_stroke_style = source.stroke_style;
            source.arrow_ratio = 0.8;
            source.arrow_end_head = index % 8 == 3 ? SNOW_ARROWHEAD_ARROW : SNOW_ARROWHEAD_NONE;
            const double left = source.center_x - source.width / 2;
            const double right = source.center_x + source.width / 2;
            const double top = source.center_y - source.height / 2;
            const double bottom = source.center_y + source.height / 2;
            if (source.arrow_type == SNOW_ARROW_TYPE_STRAIGHT) {
                points[0] = {left, source.center_y};
                points[1] = {right, source.center_y};
                source.arrow_point_count = 2;
            } else if (source.arrow_type == SNOW_ARROW_TYPE_CURVE) {
                points = {{{left, source.center_y},
                           {source.center_x - source.width * 0.15, top},
                           {source.center_x + source.width * 0.15, bottom},
                           {right, source.center_y}}};
                source.arrow_point_count = 4;
            } else {
                points = {{{left, top},
                           {source.center_x, top},
                           {source.center_x, bottom},
                           {right, bottom}}};
                source.arrow_point_count = 4;
            }
            source.arrow_points = points.data();
            source.rotation = 0;
        }
        items.emplace_back(source);
    }
    return items;
}

SceneDisplayInfo displayInfo() {
    SceneDisplayInfo info{};
    info.surface_width = width;
    info.surface_height = height;
    info.camera_zoom = 1;
    return info;
}

void render(QImage& image, const std::vector<SnowCanvasSceneItem>& items, const QRegion& region,
            const std::vector<std::uint32_t>& candidates) {
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setClipRegion(region);
    const auto info = displayInfo();
    snow_canvas_renderer::SceneRenderRequest request;
    request.painter = &painter;
    request.displayInfo = &info;
    request.sceneItems = items.data();
    request.sceneItemCount = static_cast<std::uint32_t>(items.size());
    request.exposedRegion = region;
    request.candidateIndices = candidates.empty() ? nullptr : candidates.data();
    request.candidateCount = static_cast<std::uint32_t>(candidates.size());
    snow_canvas_renderer::renderSceneItems(request);
}

double percentile(std::vector<double> values, double fraction) {
    std::sort(values.begin(), values.end());
    const auto index = std::min(values.size() - 1,
                                static_cast<std::size_t>(std::ceil(values.size() * fraction) - 1));
    return values[index];
}

void benchmark(int count, SnowStrokeRenderingStyle style, std::string_view phase, bool partial) {
    auto items = makeItems(count, style);
    QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
    const QRegion region(partial ? QRect(width / 2, height / 2, 64, 64) : image.rect());
    std::vector<std::uint32_t> candidates;
    if (partial) {
        const auto info = displayInfo();
        for (std::uint32_t i = 0; i < items.size(); ++i)
            if (region.intersects(
                    snow_canvas_render_geometry::sceneItemBounds(info, items[i]).toAlignedRect()))
                candidates.push_back(i);
    }
    if (phase != "cold")
        render(image, items, QRegion(image.rect()), {});
    snow_canvas_stroke_rendering::takeDiagnostics();
    std::vector<double> durations;
    const int iterations = phase == "cold" ? 3 : phase == "style-change" ? 4 : 12;
    for (int iteration = 0; iteration < iterations; ++iteration) {
        if (phase == "cold" && iteration != 0)
            items = makeItems(count, style);
        if (phase == "style-change") {
            for (auto& item : items)
                item.stroke_rendering_style = iteration % 2 == 0
                                                  ? SNOW_STROKE_RENDERING_STYLE_CARTOONIST
                                                  : SNOW_STROKE_RENDERING_STYLE_ARTIST;
        }
        QElapsedTimer timer;
        timer.start();
        render(image, items, region, candidates);
        durations.push_back(timer.nsecsElapsed() / 1'000'000.0);
        if (iteration == 0) {
            bool visible = false;
            for (int y = 0; y < image.height() && !visible; ++y)
                for (int x = 0; x < image.width() && !visible; ++x)
                    visible = qAlpha(image.pixel(x, y)) != 0;
            if (!visible) {
                std::cerr << "benchmark fixture did not render visible ink\n";
                std::exit(1);
            }
        }
    }
    const auto diagnostics = snow_canvas_stroke_rendering::takeDiagnostics();
    if (phase == "warm" && (diagnostics.geometryBuilds != 0 || diagnostics.strokeBuilds != 0 ||
                            diagnostics.coverageUnions != 0 || diagnostics.cacheAllocations != 0 ||
                            diagnostics.cacheDetaches != 0)) {
        std::cerr << "warm benchmark unexpectedly rebuilt retained stroke geometry\n";
        std::exit(1);
    }
    std::cout << "stroke-rendering count=" << count << " style=" << static_cast<int>(style)
              << " phase=" << phase << " region=" << (partial ? "64x64" : "full")
              << " median_ms=" << percentile(durations, 0.5)
              << " p95_ms=" << percentile(durations, 0.95)
              << " geometry_builds=" << diagnostics.geometryBuilds
              << " strokes=" << diagnostics.strokeBuilds << " unions=" << diagnostics.coverageUnions
              << " allocations=" << diagnostics.cacheAllocations
              << " detaches=" << diagnostics.cacheDetaches
              << " cache_hits=" << diagnostics.cacheHits << '\n';
}

void benchmarkConstruction(int count, SnowStrokeRenderingStyle style) {
    std::vector<double> durations;
    for (int iteration = 0; iteration < 3; ++iteration) {
        QElapsedTimer timer;
        timer.start();
        const auto items = makeItems(count, style);
        durations.push_back(timer.nsecsElapsed() / 1'000'000.0);
        if (items.size() != static_cast<std::size_t>(count))
            std::exit(1);
    }
    std::cout << "stroke-rendering count=" << count << " style=" << static_cast<int>(style)
              << " phase=scene-item-construction median_ms=" << percentile(durations, 0.5)
              << " p95_ms=" << percentile(durations, 0.95) << '\n';
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    for (int count : {1000, 10000}) {
        for (auto style :
             {SNOW_STROKE_RENDERING_STYLE_ARCHITECT, SNOW_STROKE_RENDERING_STYLE_ARTIST,
              SNOW_STROKE_RENDERING_STYLE_CARTOONIST}) {
            benchmarkConstruction(count, style);
            benchmark(count, style, "cold", false);
            benchmark(count, style, "warm", false);
            benchmark(count, style, "warm", true);
        }
        benchmark(count, SNOW_STROKE_RENDERING_STYLE_ARTIST, "style-change", false);
    }
    return 0;
}
