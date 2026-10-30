#include "snow_draw_engine_qt/snow_canvas_path_geometry.h"

#include <cstdio>
#include <cstdlib>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}

void clearDiscardsActiveStroke() {
    SnowCanvasStrokeFilter filter;
    filter.reset({0, 0});
    for (int i = 1; i <= 128; ++i)
        filter.append({static_cast<qreal>(i), static_cast<qreal>(i % 3)});
    require(!filter.takePoints().isEmpty(),
            "an active stroke must produce filtered preview points");
    filter.append({140, 8});

    filter.clear();
    require(filter.takePoints(true).isEmpty(),
            "clearing must discard both queued samples and the native unfinished endpoint");
    filter.append({150, 10});
    require(filter.takePoints(true).isEmpty(),
            "cleared filters must stay inactive until the next stroke is reset");
    filter.clear();
}

void clearAllowsAnIndependentNextStroke() {
    SnowCanvasStrokeFilter filter;
    filter.clear();
    filter.clear();
    for (int cycle = 0; cycle < 4; ++cycle) {
        filter.reset({0, 0});
        filter.append({40, 20});
        filter.takePoints();
        filter.append({80, 40});
        filter.clear();
        filter.clear();

        const QPointF start(1000 + cycle * 100, 2000);
        const QPointF endpoint = start + QPointF(30, 15);
        filter.reset(start, 2);
        filter.append(endpoint);
        const auto points = filter.takePoints(true);
        SnowCanvasStrokeFilter freshFilter;
        freshFilter.reset(start, 2);
        freshFilter.append(endpoint);
        require(points == freshFilter.takePoints(true),
                "the next stroke must match a fresh filter after repeated cleanup");
        require(!points.isEmpty() && points.back() == endpoint,
                "a reused filter must still finish at the new stroke's exact endpoint");
    }
}

} // namespace

int main() {
    clearDiscardsActiveStroke();
    clearAllowsAnIndependentNextStroke();
    return 0;
}
