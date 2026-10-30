#pragma once

#include "snow_shot/image/screenshotregiongeometry.h"

#include <QMap>
#include <QSize>
#include <limits>

// Stored in canvas units, just like selection shadow width. This is an export
// preference, not a transformation of the editable selection.
struct ScreenshotLayoutCompaction {
    bool enabled = false;
    int spacing = 0;
    static constexpr int maximumSpacing = 100;
    bool operator==(const ScreenshotLayoutCompaction&) const = default;

    ScreenshotLayoutCompaction normalized() const {
        return {enabled, std::clamp(spacing, 0, maximumSpacing)};
    }
};

namespace screenshot_layout_compaction {
// Unlike toFillPolygons(), this never introduces bridges between contours and
// retains cubic control points. Hole contours retain the parent's fill rule.
inline QVector<QPainterPath> contours(const QPainterPath& path) {
    QVector<QPainterPath> result;
    QPainterPath current;
    const auto finish = [&] {
        if (!current.isEmpty()) {
            current.setFillRule(path.fillRule());
            result.append(current);
        }
    };
    for (int i = 0; i < path.elementCount(); ++i) {
        const auto element = path.elementAt(i);
        if (element.isMoveTo()) {
            finish();
            current = {};
            current.moveTo(element.x, element.y);
        } else if (element.isLineTo()) {
            current.lineTo(element.x, element.y);
        } else if (element.type == QPainterPath::CurveToElement && i + 2 < path.elementCount()) {
            const auto control = path.elementAt(++i);
            const auto end = path.elementAt(++i);
            current.cubicTo(element.x, element.y, control.x, control.y, end.x, end.y);
        }
    }
    finish();
    return result;
}

inline bool hasMultipleComponents(const ScreenshotRegionGeometry& geometry) {
    // Normalize winding and self-intersections only for component counting.
    // Export retains the original cubic paths.
    const auto parts = contours(geometry.path().simplified());
    QVector<qsizetype> filled;
    for (qsizetype i = 0; i < parts.size(); ++i) {
        int nesting = 0;
        for (qsizetype j = 0; j < parts.size(); ++j) {
            if (i != j && parts[j].boundingRect().contains(parts[i].boundingRect()) &&
                parts[j].contains(parts[i]))
                ++nesting;
        }
        if (nesting % 2 == 0)
            filled.append(i);
    }
    if (filled.size() < 2)
        return false;
    // Simplification can retain separate contours that meet at a single point.
    // Shared vertices still connect those shapes, including after restoration.
    QVector<qsizetype> parents(filled.size());
    for (qsizetype i = 0; i < parents.size(); ++i)
        parents[i] = i;
    const auto root = [&parents](qsizetype i) {
        while (parents[i] != i)
            i = parents[i];
        return i;
    };
    qsizetype components = filled.size();
    for (qsizetype i = 0; i < filled.size(); ++i) {
        for (qsizetype j = 0; j < i; ++j) {
            if (root(i) == root(j))
                continue;
            const auto& a = parts[filled[i]];
            const auto& b = parts[filled[j]];
            const auto ar = a.boundingRect(), br = b.boundingRect();
            if (ar.left() > br.right() || br.left() > ar.right() || ar.top() > br.bottom() ||
                br.top() > ar.bottom())
                continue;
            bool joined = false;
            for (int x = 0; !joined && x < a.elementCount(); ++x)
                for (int y = 0; !joined && y < b.elementCount(); ++y)
                    joined = a.elementAt(x).x == b.elementAt(y).x &&
                             a.elementAt(x).y == b.elementAt(y).y;
            if (joined) {
                parents[root(i)] = root(j);
                if (--components == 1)
                    return false;
            }
        }
    }
    return true;
}

struct Band {
    int begin = 0;
    int end = 0; // Half-open source pixel interval.
    int destination = 0;
};

inline QVector<Band> mergeBands(QVector<Band> bands, int spacing) {
    std::sort(bands.begin(), bands.end(), [](const Band& a, const Band& b) {
        return a.begin < b.begin || (a.begin == b.begin && a.end < b.end);
    });
    QVector<Band> merged;
    for (const auto& band : bands) {
        if (!merged.isEmpty() && band.begin <= merged.last().end)
            merged.last().end = std::max(merged.last().end, band.end);
        else
            merged.append(band);
    }
    qint64 destination = 0;
    for (auto& band : merged) {
        if (destination + band.end - band.begin > std::numeric_limits<int>::max())
            return {};
        band.destination = int(destination);
        destination += qint64(band.end) - band.begin + spacing;
    }
    return merged;
}

inline qsizetype bandAt(const QVector<Band>& bands, qreal coordinate) {
    const auto found =
        std::upper_bound(bands.begin(), bands.end(), coordinate,
                         [](qreal value, const Band& band) { return value < band.begin; });
    return std::max<qsizetype>(0, found - bands.begin() - 1);
}
} // namespace screenshot_layout_compaction

struct ScreenshotLayoutCompactionPlan {
    struct Tile {
        QRect source;
        QPoint destination;
    };
    QSize sourceSize;
    QSize outputSize;
    QVector<screenshot_layout_compaction::Band> columns;
    QVector<screenshot_layout_compaction::Band> rows;
    QVector<Tile> tiles;
    bool changed = false;

    bool isValid() const {
        return !outputSize.isEmpty();
    }

    // Each connected contour lies inside exactly one occupied band on either
    // axis. Translating contours preserves holes, curves and rounded masks.
    QPainterPath mapPath(const QPainterPath& source) const {
        if (!changed)
            return source;
        QPainterPath result;
        result.setFillRule(source.fillRule());
        for (auto part : screenshot_layout_compaction::contours(source)) {
            const auto center = part.boundingRect().center();
            const auto& x = columns[screenshot_layout_compaction::bandAt(columns, center.x())];
            const auto& y = rows[screenshot_layout_compaction::bandAt(rows, center.y())];
            result.addPath(part.translated(x.destination - x.begin, y.destination - y.begin));
        }
        return result;
    }
};

// Geometry is relative to the original selection origin. Rounding outward keeps
// every covered pixel; intervals that touch after rasterization remain joined.
// Shadow padding is the compositor's normalized width in output pixels.
inline ScreenshotLayoutCompactionPlan
makeScreenshotLayoutCompactionPlan(const ScreenshotRegionGeometry& geometry,
                                   const QSize& sourceSize, qreal scale,
                                   ScreenshotLayoutCompaction options, int shadowPadding = 0) {
    using namespace screenshot_layout_compaction;
    ScreenshotLayoutCompactionPlan result;
    if (sourceSize.isEmpty() || !std::isfinite(scale) || scale <= 0 || scale > 256)
        return result;
    result.sourceSize = result.outputSize = sourceSize;
    if (!options.enabled || geometry.isEmpty())
        return result;
    options = options.normalized();
    shadowPadding = std::max(0, shadowPadding);
    const int spacing = qRound(options.spacing * scale);
    const auto parts = contours(QTransform::fromScale(scale, scale).map(geometry.path(scale)));
    QVector<QRect> bounds;
    QVector<Band> columns;
    QVector<Band> rows;
    for (const auto& part : parts) {
        const auto rect = part.boundingRect().intersected(QRectF(QPointF(), QSizeF(sourceSize)));
        if (rect.isEmpty())
            continue;
        const QRect pixels = rect.toAlignedRect().intersected(QRect(QPoint(), sourceSize));
        bounds.append(pixels);
        // Shadows occupy the otherwise empty bands between content contours.
        // Keep tiles limited to content; the compositor paints the shadow after
        // translation and supplies the padding outside the source selection.
        const int right = pixels.x() + pixels.width();
        const int bottom = pixels.y() + pixels.height();
        columns.append({std::max(0, pixels.x() - shadowPadding),
                        right + std::min(shadowPadding, sourceSize.width() - right), 0});
        rows.append({std::max(0, pixels.y() - shadowPadding),
                     bottom + std::min(shadowPadding, sourceSize.height() - bottom), 0});
    }
    if (bounds.isEmpty())
        return {};
    result.columns = mergeBands(std::move(columns), spacing);
    result.rows = mergeBands(std::move(rows), spacing);
    if (result.columns.isEmpty() || result.rows.isEmpty())
        return {};
    // Only internal empty bands are removed. Keep rounding margins at the
    // outside of the source selection, just as the uncompacted export does.
    result.columns.first().begin = 0;
    result.columns.last().end = sourceSize.width();
    result.rows.first().begin = 0;
    result.rows.last().end = sourceSize.height();
    const auto position = [spacing](QVector<Band>& bands) -> int {
        qint64 end = 0;
        for (auto& band : bands) {
            if (end + band.end - band.begin > std::numeric_limits<int>::max())
                return 0;
            band.destination = int(end);
            end += qint64(band.end) - band.begin + spacing;
        }
        return int(end - spacing);
    };
    result.outputSize = {position(result.columns), position(result.rows)};
    if (result.outputSize.isEmpty())
        return {};
    for (const auto& band : result.columns)
        result.changed |= band.destination != band.begin;
    for (const auto& band : result.rows)
        result.changed |= band.destination != band.begin;
    if (!result.changed)
        return result;

    // Allocate only occupied band intersections, not a Cartesian product of
    // every row and column. Nested holes and overlapping bounds share a tile.
    QMap<std::pair<qsizetype, qsizetype>, QRect> occupied;
    for (const auto& rect : bounds) {
        const auto x = bandAt(result.columns, rect.center().x());
        const auto y = bandAt(result.rows, rect.center().y());
        occupied[{x, y}] |= rect;
    }
    for (auto it = occupied.cbegin(); it != occupied.cend(); ++it) {
        const auto& x = result.columns[it.key().first];
        const auto& y = result.rows[it.key().second];
        result.tiles.append({it.value(), it.value().topLeft() + QPoint(x.destination - x.begin,
                                                                       y.destination - y.begin)});
    }
    return result;
}

struct ScreenshotLayoutCompactionCache {
    ScreenshotRegionGeometry geometry;
    QSize size;
    qreal scale = 0;
    ScreenshotLayoutCompaction options;
    int shadowPadding = 0;
    std::shared_ptr<const ScreenshotLayoutCompactionPlan> plan;
};
inline thread_local ScreenshotLayoutCompactionCache screenshotLayoutCompactionCache;

inline void resetScreenshotLayoutCompactionCache() {
    screenshotLayoutCompactionCache = {};
}

// A bounded per-thread cache is shared by pin metadata and export composition.
// Geometry equality deliberately excludes selection hover and toolbar state.
inline std::shared_ptr<const ScreenshotLayoutCompactionPlan>
screenshotLayoutCompactionPlan(const ScreenshotRegionGeometry& geometry, const QSize& size,
                               qreal scale, ScreenshotLayoutCompaction options,
                               int shadowPadding = 0) {
    auto& cache = screenshotLayoutCompactionCache;
    options = options.normalized();
    shadowPadding = std::max(0, shadowPadding);
    if (cache.plan && cache.geometry == geometry && cache.size == size && cache.scale == scale &&
        cache.options == options && cache.shadowPadding == shadowPadding)
        return cache.plan;
    auto plan = std::make_shared<const ScreenshotLayoutCompactionPlan>(
        makeScreenshotLayoutCompactionPlan(geometry, size, scale, options, shadowPadding));
    cache = {geometry, size, scale, options, shadowPadding, plan};
    return plan;
}
