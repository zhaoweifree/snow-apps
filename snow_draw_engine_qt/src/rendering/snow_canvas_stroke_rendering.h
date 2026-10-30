#pragma once

#include "snow_draw_engine.h"

#include <QPainterPath>
#include <QTransform>

#include <cstdint>

class SnowCanvasSceneItem;
class SnowCanvasOverlayItem;

namespace snow_canvas_stroke_rendering {

// Vector coverage is independent of colour, opacity, camera and pixel density.
// It is built only when the retained element's geometry or stroke changes.
struct Cache {
    std::uint64_t key = 0;
    bool valid = false;
    bool strokeCoverageOnly = true;
    QPainterPath originalFill;
    QPainterPath backgroundHeads;
    QPainterPath ink;
    QRectF paintBounds;
};

struct Diagnostics {
    std::uint64_t cacheHits = 0;
    std::uint64_t geometryBuilds = 0;
    std::uint64_t strokeBuilds = 0;
    std::uint64_t coverageUnions = 0;
    std::uint64_t cacheAllocations = 0;
    std::uint64_t cacheDetaches = 0;
};

Diagnostics takeDiagnostics();
void recordCacheAllocation(bool detach);
const Cache& geometry(const SnowCanvasSceneItem& item);
const Cache& geometry(const SnowCanvasOverlayItem& item);
const Cache* cachedGeometry(const SnowCanvasSceneItem& item);
const Cache* cachedGeometry(const SnowCanvasOverlayItem& item);
QTransform localToCanvas(double centerX, double centerY, double rotation);

} // namespace snow_canvas_stroke_rendering
