#include "snow_canvas_display_item.h"
#include "snow_canvas_render_geometry.h"
#include "snow_canvas_renderer.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QImage>
#include <QPainter>
#include <QRegion>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

constexpr int kSurfaceWidth = 1920;
constexpr int kSurfaceHeight = 1080;
constexpr int kWarmupIterations = 3;
constexpr int kMeasuredIterations = 20;

std::vector<SnowCanvasSceneItem> makeRectangles(int count, SnowFillStyle fillStyle) {
    std::vector<SnowCanvasSceneItem> items;
    items.reserve(count);
    const int columns = static_cast<int>(std::ceil(std::sqrt(count * 16.0 / 9.0)));
    const int rows = (count + columns - 1) / columns;
    const double cellWidth = static_cast<double>(kSurfaceWidth) / columns;
    const double cellHeight = static_cast<double>(kSurfaceHeight) / rows;
    for (int index = 0; index < count; ++index) {
        const int column = index % columns;
        const int row = index / columns;
        SnowSceneDisplayItem view{};
        view.kind = SNOW_SCENE_DISPLAY_ITEM_DRAW_RECT;
        view.element_id = SnowElementId{
            static_cast<std::uint32_t>(index),
            1,
        };
        view.center_x = -kSurfaceWidth / 2.0 + (column + 0.5) * cellWidth;
        view.center_y = -kSurfaceHeight / 2.0 + (row + 0.5) * cellHeight;
        view.width = std::max(2.0, cellWidth * 0.8);
        view.height = std::max(2.0, cellHeight * 0.8);
        view.rotation = index % 7 == 0 ? 0.08 : 0.0;
        view.fill = SnowColorRgba8{40, 130, 210, 190};
        view.fill_style = fillStyle;
        view.stroke = SnowColorRgba8{20, 40, 60, 255};
        view.stroke_width = 1.5;
        view.opacity = 1.0;
        if (index % 5 == 0) {
            view.corner_radii = SnowCornerRadii{3.0, 3.0, 3.0, 3.0};
        }
        items.emplace_back(view);
    }
    return items;
}

double percentile(std::vector<double> values, double fraction) {
    std::sort(values.begin(), values.end());
    const std::size_t index = std::min(
        values.size() - 1, static_cast<std::size_t>(std::ceil(values.size() * fraction) - 1));
    return values[index];
}

void renderOnce(QImage& image, const std::vector<SnowCanvasSceneItem>& items,
                const SceneDisplayInfo& displayInfo, const QRegion& region,
                const std::vector<std::uint32_t>& candidates) {
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setClipRegion(region);
    snow_canvas_renderer::renderSceneItems(snow_canvas_renderer::SceneRenderRequest{
        &painter,
        &displayInfo,
        items.data(),
        static_cast<std::uint32_t>(items.size()),
        region,
        candidates.empty() ? nullptr : candidates.data(),
        static_cast<std::uint32_t>(candidates.size()),
    });
}

void benchmarkRender(int count, SnowFillStyle fillStyle, std::string_view name,
                     const QRegion& region) {
    std::vector<SnowCanvasSceneItem> items = makeRectangles(count, fillStyle);
    SceneDisplayInfo displayInfo{};
    displayInfo.item_count = static_cast<std::uint32_t>(items.size());
    displayInfo.surface_width = kSurfaceWidth;
    displayInfo.surface_height = kSurfaceHeight;
    displayInfo.camera_zoom = 1.0;
    QImage image(kSurfaceWidth, kSurfaceHeight, QImage::Format_ARGB32_Premultiplied);
    std::vector<std::uint32_t> candidates;
    if (region.boundingRect().size() != image.size()) {
        for (std::uint32_t index = 0; index < items.size(); ++index) {
            const QRectF bounds =
                snow_canvas_render_geometry::sceneItemBounds(displayInfo, items[index]);
            if (region.intersects(bounds.toAlignedRect())) {
                candidates.push_back(index);
            }
        }
    }
    for (int iteration = 0; iteration < kWarmupIterations; ++iteration) {
        renderOnce(image, items, displayInfo, region, candidates);
    }

    std::vector<double> milliseconds;
    milliseconds.reserve(kMeasuredIterations);
    for (int iteration = 0; iteration < kMeasuredIterations; ++iteration) {
        QElapsedTimer timer;
        timer.start();
        renderOnce(image, items, displayInfo, region, candidates);
        milliseconds.push_back(timer.nsecsElapsed() / 1'000'000.0);
    }
    std::cout << "render count=" << count << " fill=" << name
              << " region=" << (region.boundingRect().size() == image.size() ? "full" : "64x64")
              << " median_ms=" << percentile(milliseconds, 0.5)
              << " p95_ms=" << percentile(milliseconds, 0.95) << '\n';
}

void benchmarkMiddleReplacement() {
    std::vector<SnowCanvasSceneItem> items = makeRectangles(10'000, SNOW_FILL_STYLE_SOLID);
    SnowCanvasSceneItem replacement = items[items.size() / 2];
    QElapsedTimer timer;
    timer.start();
    for (int iteration = 0; iteration < 10'000; ++iteration) {
        replacement.center_x += 0.001;
        items[items.size() / 2] = replacement;
    }
    std::cout << "patch count=10000 replacements=10000 total_ms="
              << timer.nsecsElapsed() / 1'000'000.0 << '\n';
}

// The same fixtures can be linked against the pre-change renderer to verify
// that Architect retains its pixels, including fractional-DPI rasterization.
bool writeArchitectSnapshots(const QString& directory) {
    if (!QDir().mkpath(directory))
        return false;
    SceneDisplayInfo info{};
    info.item_count = 1;
    info.surface_width = 512;
    info.surface_height = 256;
    info.camera_zoom = 1.0;
    const auto save = [&](const SnowCanvasSceneItem& item, const QString& name, double dpr) {
        QImage image(QSize(512, 256) * dpr, QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(dpr);
        renderOnce(image, {item}, info, QRegion(QRect(0, 0, 512, 256)), {});
        return image.save(
            QDir(directory).filePath(name + QString::number(dpr) + QStringLiteral(".png")));
    };
    for (double dpr : {1.0, 1.5}) {
        for (int shape = 0; shape < 3; ++shape) {
            for (int pattern = 0; pattern < 3; ++pattern) {
                for (int rounded = 0; rounded < 2; ++rounded) {
                    SnowSceneDisplayItem source{};
                    source.kind = SNOW_SCENE_DISPLAY_ITEM_DRAW_RECT;
                    source.width = 185;
                    source.height = 99;
                    source.rotation = rounded != 0 ? 0.23 : 0.0;
                    source.rect_shape = static_cast<std::uint8_t>(shape);
                    source.fill = {20, 130, 210, 110};
                    source.fill_style = rounded != 0 ? SNOW_FILL_STYLE_LINE : SNOW_FILL_STYLE_SOLID;
                    source.stroke = {30, 50, 80, 190};
                    source.stroke_width = 3;
                    source.stroke_style = static_cast<SnowStrokeStyle>(pattern);
                    source.opacity = 0.7;
                    if (rounded != 0)
                        source.corner_radii = {8, 14, 20, 6};
                    if (!save(
                            SnowCanvasSceneItem(source),
                            QStringLiteral("shape-%1-%2-%3-").arg(shape).arg(pattern).arg(rounded),
                            dpr))
                        return false;
                }
            }
        }
        for (int route = 0; route < 3; ++route) {
            for (int pattern = 0; pattern < 3; ++pattern) {
                for (const auto head : {SNOW_ARROWHEAD_ARROW, SNOW_ARROWHEAD_TRIANGLE,
                                        SNOW_ARROWHEAD_CIRCLE_OUTLINE}) {
                    const SnowArrowPoint points[] = {{-160, -30}, {-40, -30}, {40, 35}, {160, 35}};
                    SnowSceneDisplayItem source{};
                    source.kind = SNOW_SCENE_DISPLAY_ITEM_ARROW;
                    source.arrow_points = points;
                    source.arrow_point_count = 4;
                    source.arrow_type = static_cast<SnowArrowType>(route);
                    source.arrow_stroke_style = static_cast<SnowStrokeStyle>(pattern);
                    source.arrow_end_head = head;
                    source.arrow_ratio = 1;
                    source.stroke = {30, 50, 80, 190};
                    source.stroke_width = 3;
                    source.opacity = 0.7;
                    if (!save(SnowCanvasSceneItem(source),
                              QStringLiteral("arrow-%1-%2-%3-")
                                  .arg(route)
                                  .arg(pattern)
                                  .arg(static_cast<int>(head)),
                              dpr))
                        return false;
                }
            }
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc == 3 && std::string_view(argv[1]) == "--architect-snapshots")
        return writeArchitectSnapshots(QString::fromLocal8Bit(argv[2])) ? 0 : 1;
    std::cout << "abi scene_bytes=" << sizeof(SnowSceneDisplayItem)
              << " overlay_bytes=" << sizeof(SnowOverlayDisplayItem) << '\n';
    const QRegion full(QRect(0, 0, kSurfaceWidth, kSurfaceHeight));
    const QRegion partial(QRect(kSurfaceWidth / 2, kSurfaceHeight / 2, 64, 64));
    for (int count : {1'000, 10'000}) {
        benchmarkRender(count, SNOW_FILL_STYLE_SOLID, "solid", full);
        benchmarkRender(count, SNOW_FILL_STYLE_LINE, "line", full);
        benchmarkRender(count, SNOW_FILL_STYLE_CROSS_LINE, "cross", full);
        benchmarkRender(count, SNOW_FILL_STYLE_SOLID, "solid", partial);
        benchmarkRender(count, SNOW_FILL_STYLE_LINE, "line", partial);
        benchmarkRender(count, SNOW_FILL_STYLE_CROSS_LINE, "cross", partial);
    }
    benchmarkMiddleReplacement();
    return 0;
}
