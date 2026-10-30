#include "snow_draw_engine_qt/snow_canvas_snapping.h"

#include "snow_canvas_type_conversions.h"
#include "snow_canvas_render_geometry.h"
#include "snow_canvas_renderer.h"
#include <QPainter>
#include <memory>
#include <vector>

namespace {
SnowSnapRect snapRect(const QRectF& rect) {
    return {rect.x(), rect.y(), rect.x() + rect.width(), rect.y() + rect.height()};
}

SnowOverlayDisplayItem displayItem(const SnowCanvasSnapGuide& guide, const QTransform& transform,
                                   const SnowCanvasSnapConfig& config) {
    SnowOverlayDisplayItem item{};
    item.kind = SNOW_OVERLAY_DISPLAY_ITEM_SNAP_GUIDE;
    item.snap_guide_kind = guide.gap ? SNOW_SNAP_GUIDE_GAP : SNOW_SNAP_GUIDE_POINT;
    item.snap_guide_axis = guide.vertical ? SNOW_SNAP_GUIDE_VERTICAL : SNOW_SNAP_GUIDE_HORIZONTAL;
    const auto start = transform.map(guide.start);
    const auto end = transform.map(guide.end);
    item.snap_start_x = start.x();
    item.snap_start_y = start.y();
    item.snap_end_x = end.x();
    item.snap_end_y = end.y();
    item.snap_marker_count = static_cast<std::uint8_t>(qMin<qsizetype>(2, guide.markers.size()));
    if (!guide.markers.isEmpty()) {
        const auto marker = transform.map(guide.markers[0]);
        item.snap_marker0_x = marker.x();
        item.snap_marker0_y = marker.y();
    }
    if (guide.markers.size() > 1) {
        const auto marker = transform.map(guide.markers[1]);
        item.snap_marker1_x = marker.x();
        item.snap_marker1_y = marker.y();
    }
    const auto style = snow_canvas_types::toEngineSnapConfig(config);
    item.snap_has_label = config.showGapSize && guide.label.has_value();
    item.snap_label = guide.label.value_or(0.0);
    item.snap_color = style.line_color;
    item.snap_line_width = style.line_width;
    item.snap_marker_size = style.marker_size;
    item.snap_gap_dash_length = style.gap_dash_length;
    item.snap_gap_dash_gap = style.gap_dash_gap;
    return item;
}
} // namespace

std::optional<SnowCanvasMoveSnap>
snowCanvasSnapMove(const QRectF& target, const QVector<QRectF>& references, qreal canvasToViewScale,
                   const SnowCanvasSnapConfig& config,
                   std::optional<SnowCanvasMoveSnapConstraints> constraints) {
    const auto moving = snapRect(target);
    std::vector<SnowSnapRect> targets;
    targets.reserve(static_cast<std::size_t>(references.size()));
    for (const auto& reference : references)
        targets.push_back(snapRect(reference));
    const auto style = snow_canvas_types::toEngineSnapConfig(config);
    const SnowSnapMoveConstraints limits =
        constraints
            ? SnowSnapMoveConstraints{snapRect(constraints->bounds), constraints->positionStep}
            : SnowSnapMoveConstraints{};
    SnowMoveSnap* raw = nullptr;
    if (snow_snap_move(&moving, targets.data(), targets.size(), &style, canvasToViewScale,
                       constraints ? &limits : nullptr, &raw) != SNOW_OK)
        return std::nullopt;
    const std::unique_ptr<SnowMoveSnap, decltype(&snow_move_snap_free)> owner(raw,
                                                                              snow_move_snap_free);
    SnowArrowPoint correction{};
    const SnowOverlayDisplayItem* items = nullptr;
    std::size_t count = 0;
    if (snow_move_snap_read(owner.get(), &correction, &items, &count) != SNOW_OK)
        return std::nullopt;
    SnowCanvasMoveSnap result;
    result.correction = {correction.x, correction.y};
    result.guides.reserve(static_cast<qsizetype>(count));
    for (std::size_t index = 0; index < count; ++index) {
        const auto& item = items[index];
        SnowCanvasSnapGuide guide;
        guide.gap = item.snap_guide_kind == SNOW_SNAP_GUIDE_GAP;
        guide.vertical = item.snap_guide_axis == SNOW_SNAP_GUIDE_VERTICAL;
        guide.start = {item.snap_start_x, item.snap_start_y};
        guide.end = {item.snap_end_x, item.snap_end_y};
        if (item.snap_marker_count > 0)
            guide.markers.append({item.snap_marker0_x, item.snap_marker0_y});
        if (item.snap_marker_count > 1)
            guide.markers.append({item.snap_marker1_x, item.snap_marker1_y});
        if (item.snap_has_label)
            guide.label = item.snap_label;
        result.guides.append(guide);
    }
    return result;
}

void snowCanvasPaintSnapGuides(QPainter& painter, const QVector<SnowCanvasSnapGuide>& guides,
                               const QTransform& canvasToView, const QRegion& exposedRegion,
                               const SnowCanvasSnapConfig& config) {
    if (!config.showGuides)
        return;
    // Map positions first so widths and markers stay in logical view pixels, exactly
    // as in the canvas renderer. Identity projection also supports translated displays.
    for (const auto& guide : guides) {
        const auto item = displayItem(guide, canvasToView, config);
        snow_canvas_renderer::renderOverlayItems(painter, OverlayDisplayInfo{}, &item, 1,
                                                 exposedRegion);
    }
}

QRegion snowCanvasSnapGuideDamage(const QVector<SnowCanvasSnapGuide>& guides,
                                  const QTransform& canvasToView,
                                  const SnowCanvasSnapConfig& config) {
    QRegion damage;
    if (config.showGuides) {
        for (const auto& guide : guides) {
            const auto item = displayItem(guide, canvasToView, config);
            damage += snow_canvas_render_geometry::alignedRectForBounds(
                snow_canvas_render_geometry::overlayItemBounds(OverlayDisplayInfo{}, item));
        }
    }
    return damage;
}
