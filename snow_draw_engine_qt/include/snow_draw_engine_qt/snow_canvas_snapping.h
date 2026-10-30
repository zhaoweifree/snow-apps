#pragma once

#include "snow_canvas_types.h"
#include <QRegion>
#include <QTransform>
#include <optional>

class QPainter;

struct SnowCanvasSnapGuide {
    bool gap = false;
    bool vertical = false;
    QPointF start;
    QPointF end;
    QVector<QPointF> markers;
    std::optional<double> label;

    bool operator==(const SnowCanvasSnapGuide& other) const {
        return gap == other.gap && vertical == other.vertical && start == other.start &&
               end == other.end && markers == other.markers && label == other.label;
    }
};

struct SnowCanvasMoveSnap {
    QPointF correction;
    QVector<SnowCanvasSnapGuide> guides;
};

struct SnowCanvasMoveSnapConstraints {
    QRectF bounds;
    qreal positionStep = 1.0;
};

// Explicit object snapping; the persistent enabled flag is intentionally not consulted.
// All rectangles use continuous edges (x + width, y + height), in canvas coordinates.
std::optional<SnowCanvasMoveSnap>
snowCanvasSnapMove(const QRectF& target, const QVector<QRectF>& references, qreal canvasToViewScale,
                   const SnowCanvasSnapConfig& config = {},
                   std::optional<SnowCanvasMoveSnapConstraints> constraints = std::nullopt);

// Uses the same painting and bounds calculation as the engine's rectangle guides.
void snowCanvasPaintSnapGuides(QPainter& painter, const QVector<SnowCanvasSnapGuide>& guides,
                               const QTransform& canvasToView, const QRegion& exposedRegion,
                               const SnowCanvasSnapConfig& config = {});
QRegion snowCanvasSnapGuideDamage(const QVector<SnowCanvasSnapGuide>& guides,
                                  const QTransform& canvasToView,
                                  const SnowCanvasSnapConfig& config = {});
