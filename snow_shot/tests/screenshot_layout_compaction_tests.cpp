#include "snow_shot/image/screenshotlayoutcompaction.h"
#include "snow_shot/presentation/screenshotresultcompositor.h"
#include "snow_shot/presentation/screenshotselectionpin.h"
#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include <QMouseEvent>

#include <QApplication>
#include <QPainter>
#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

ScreenshotRegionGeometry regions(std::initializer_list<QRect> rectangles) {
    QRegion region;
    for (const auto& rectangle : rectangles)
        region += rectangle;
    return region;
}

void bandsPreserveArrangement() {
    const auto grid = regions({{0, 0, 10, 8}, {30, 0, 20, 8}, {0, 25, 10, 12}, {30, 25, 20, 12}});
    for (int spacing : {0, 1, 40, 100}) {
        const auto plan = makeScreenshotLayoutCompactionPlan(grid, {50, 37}, 1, {true, spacing});
        require(plan.isValid() && plan.outputSize == QSize(30 + spacing, 20 + spacing),
                "grid must compact before inserting exact spacing");
        require(plan.tiles.size() == 4, "grid tiles must remain disjoint");
        require(plan.mapPath(grid.path()).contains(QPointF(10 + spacing + 10, 8 + spacing + 6)),
                "bottom-right region must retain its original dimensions and ordering");
    }
    require(makeScreenshotLayoutCompactionPlan(grid, {50, 37}, 1, {true, 1000}).outputSize ==
                QSize(130, 120),
            "spacing above 100 must clamp before export");
    require(makeScreenshotLayoutCompactionPlan(grid, {50, 37}, 1, {true, -1}).outputSize ==
                QSize(30, 20),
            "negative spacing must clamp to zero before export");
    const auto staircase = regions({{0, 0, 10, 10}, {30, 30, 10, 10}, {60, 60, 10, 10}});
    const auto diagonal = makeScreenshotLayoutCompactionPlan(staircase, {70, 70}, 1, {true, 0});
    require(diagonal.outputSize == QSize(30, 30) && diagonal.tiles.size() == 3,
            "staggered regions must preserve rows/columns without a quadratic tile list");
    require(!diagonal.mapPath(staircase.path()).contains(QPointF(15, 5)),
            "local empty pockets must remain empty");
    const auto noBands = regions({{0, 0, 10, 30}, {5, 20, 30, 10}});
    const auto joined = makeScreenshotLayoutCompactionPlan(noBands, {35, 30}, 1, {true, 20});
    require(!joined.changed && joined.outputSize == QSize(35, 30),
            "joined regions must not acquire new internal spacing");
    const auto touching = regions({{0, 0, 10, 10}, {10, 0, 10, 10}});
    require(!makeScreenshotLayoutCompactionPlan(touching, {20, 10}, 1, {true, 30}).changed,
            "touching regions stay joined");
    require(!makeScreenshotLayoutCompactionPlan(grid, {50, 37}, 1, {false, 20}).changed,
            "disabled compaction is an identity");
    require(!makeScreenshotLayoutCompactionPlan(grid, {}, 1, {true, 0}).isValid() &&
                !makeScreenshotLayoutCompactionPlan(grid, {50, 37}, 0, {true, 0}).isValid(),
            "invalid dimensions/scales must fail");
}

void contoursRetainHolesAndCurves() {
    require(!screenshot_layout_compaction::hasMultipleComponents(
                regions({{0, 0, 10, 10}, {10, 10, 10, 10}})),
            "point-touching restored shapes remain joined");
    const auto ring =
        ScreenshotRegionGeometry(QRegion(QRect(0, 0, 40, 40)).subtracted(QRect(10, 10, 20, 20)));
    require(!screenshot_layout_compaction::hasMultipleComponents(ring),
            "a hole must not count as a second region");
    auto geometry = ring.united(QRect(60, 0, 20, 20));
    require(screenshot_layout_compaction::hasMultipleComponents(geometry),
            "separate regions must be detected after restore");
    const auto plan = makeScreenshotLayoutCompactionPlan(geometry, {80, 40}, 1, {true, 0});
    const auto mapped = plan.mapPath(geometry.path());
    require(plan.outputSize == QSize(60, 40) && !mapped.contains(QPointF(20, 20)) &&
                mapped.contains(QPointF(50, 10)),
            "holes must move with their enclosing shape");

    QPainterPath curved;
    curved.addEllipse(QRectF(0, 0, 10, 20));
    curved.addEllipse(QRectF(40, 35, 10, 20));
    geometry = ScreenshotRegionGeometry::fromPath(curved, ScreenshotRegionType::Curve);
    for (qreal scale : {0.5, 1.0, 1.25, 1.5, 2.0, 4.0}) {
        const auto scaled = QTransform::fromScale(scale, scale).map(curved);
        const QSize size(qCeil(50 * scale), qCeil(55 * scale));
        const auto result = makeScreenshotLayoutCompactionPlan(geometry, size, scale, {true, 3});
        const auto output = result.mapPath(scaled);
        require(output.elementCount() == scaled.elementCount(),
                "cubic controls must not be flattened by compaction");
        require(result.columns.size() == 2 && result.rows.size() == 2,
                "curved islands must produce two occupied bands");
        require(result.columns[1].destination - (result.columns[0].end - result.columns[0].begin) ==
                    qRound(3 * scale),
                "spacing must use shadow-width canvas units and export rounding");
    }
    QPainterPath nested;
    nested.setFillRule(Qt::OddEvenFill);
    nested.addRect(QRectF(0, 0, 40, 40));
    nested.addRect(QRectF(5, 5, 30, 30));
    nested.addRect(QRectF(10, 10, 20, 20));
    require(screenshot_layout_compaction::hasMultipleComponents(
                ScreenshotRegionGeometry::fromPath(nested, ScreenshotRegionType::Polyline)),
            "an island inside a hole is a distinct component");
}

void shadowFootprintsDefineSpacing() {
    const auto geometry =
        regions({{0, 0, 20, 20}, {60, 0, 20, 20}, {0, 50, 20, 20}, {60, 50, 20, 20}});
    for (int shadow : {1, 3, 8}) {
        for (int spacing : {0, 5}) {
            const ScreenshotResultStyle style{6, shadow, Qt::black, geometry};
            const auto appearance =
                screenshotSelectionBorderAppearance({80, 70}, style, {true, spacing});
            require(appearance.sourceSize ==
                            QSize(40 + 4 * shadow + spacing, 40 + 4 * shadow + spacing) &&
                        appearance.contentRect == QRectF(shadow, shadow, 40 + 2 * shadow + spacing,
                                                         40 + 2 * shadow + spacing),
                    "compaction spacing must start outside each region's full shadow footprint");

            const auto plan =
                makeScreenshotLayoutCompactionPlan(geometry, {80, 70}, 1, {true, spacing}, shadow);
            QImage source(80, 70, QImage::Format_ARGB32_Premultiplied);
            source.fill(Qt::white);
            const auto original = ScreenshotResultCompositor::compose(source, style);
            QImage expected(appearance.sourceSize, source.format());
            expected.fill(Qt::transparent);
            QPainter painter(&expected);
            const int side = 20 + 2 * shadow;
            for (int row = 0; row < 2; ++row)
                for (int column = 0; column < 2; ++column)
                    painter.drawImage(QPoint(column * (side + spacing), row * (side + spacing)),
                                      original, QRect(column * 60, row * 50, side, side));
            painter.end();
            QImage content(plan.outputSize, source.format());
            content.fill(Qt::white);
            require(ScreenshotResultCompositor::compose(
                        content, compactedScreenshotResultStyle(style, plan)) == expected,
                    "compaction must retain all shadow pixels around each rounded region");
        }
    }

    for (int gap : {5, 6, 7}) {
        const auto close = regions({{0, 0, 20, 20}, {20 + gap, 0, 20, 20}});
        const auto plan =
            makeScreenshotLayoutCompactionPlan(close, {40 + gap, 20}, 1, {true, 0}, 3);
        require(plan.changed == (gap > 6) && plan.outputSize == QSize(40 + std::min(gap, 6), 20),
                "touching and overlapping shadow footprints must remain joined");
    }
    for (qreal scale : {0.5, 1.0, 1.25, 1.5, 2.0}) {
        const auto pair = regions({{0, 0, 20, 20}, {60, 0, 20, 20}});
        const int padding = qRound(3 * scale);
        const auto plan = makeScreenshotLayoutCompactionPlan(
            pair, {qCeil(80 * scale), qCeil(20 * scale)}, scale, {true, 5}, padding);
        require(plan.outputSize ==
                    QSize(qRound(40 * scale) + 2 * padding + qRound(5 * scale), qCeil(20 * scale)),
                "shadow footprint must use the rendered pixel width at fractional scales");
    }
    resetScreenshotLayoutCompactionCache();
    const auto plain = screenshotLayoutCompactionPlan(geometry, {80, 70}, 1, {true, 0}, 0);
    const auto shadowed = screenshotLayoutCompactionPlan(geometry, {80, 70}, 1, {true, 0}, 3);
    require(plain != shadowed && plain->outputSize == QSize(40, 40) &&
                shadowed->outputSize == QSize(46, 46) &&
                screenshotLayoutCompactionPlan(geometry, {80, 70}, 1, {true, 0}, 3) == shadowed,
            "changing shadow width must invalidate the compaction cache");
}

void roundedMasksSurviveJoining() {
    const auto geometry = regions({{0, 0, 20, 20}, {40, 0, 20, 20}});
    ScreenshotResultStyle style{6, 0, QColor(0, 0, 0, 150), geometry};
    const auto plan = makeScreenshotLayoutCompactionPlan(geometry, {60, 20}, 1, {true, 0});
    const auto compacted = compactedScreenshotResultStyle(style, plan);
    require(compacted.cornerRadius == 0 && compacted.region && compacted.region->custom(),
            "rounded masks must be baked before joining");
    require(!compacted.region->contains(QPointF(20, 0)) &&
                compacted.region->contains(QPointF(20, 10)),
            "zero spacing must preserve corner cutouts but join the straight edges");
    QImage pixels(plan.outputSize, QImage::Format_ARGB32_Premultiplied);
    pixels.fill(Qt::white);
    const auto output = ScreenshotResultCompositor::compose(pixels, compacted);
    require(output.size() == QSize(40, 20), "joined rounded masks must retain content dimensions");
}

void invalidRasterRequestsFailBeforeAllocation() {
    SnowCanvasRuntime runtime;
    const QList<CanvasExportTile> tile{{QRect(0, 0, 10, 10), QPoint()}};
    require(runtime
                .renderTilesToImage(QRectF(0, 0, 10, 10), {10, 10},
                                    {std::numeric_limits<int>::max(), 10}, tile, {})
                .isNull(),
            "oversized output must fail before allocation");
    require(runtime
                .renderTilesToImage(QRectF(0, 0, std::numeric_limits<double>::infinity(), 10),
                                    {10, 10}, {10, 10}, tile, {})
                .isNull(),
            "non-finite projections must fail");
    require(runtime
                .renderTilesToImage(
                    QRectF(0, 0, 10, 10), {10, 10}, {10, 10},
                    {{QRect(0, 0, 10, 10), QPoint(std::numeric_limits<int>::max(), 0)}}, {})
                .isNull(),
            "destination endpoint overflow must fail before constructing a rectangle");
}

void nonRectangularMasksFollowSource() {
    for (auto type : {ScreenshotRegionType::Polyline, ScreenshotRegionType::Curve,
                      ScreenshotRegionType::Freehand}) {
        QPainterPath left;
        left.moveTo(0, 10);
        left.lineTo(30, 0);
        left.lineTo(40, 70);
        left.lineTo(10, 90);
        left.closeSubpath();
        QPainterPath right;
        right.addEllipse(QRectF(80, 10, 30, 70));
        auto geometry = ScreenshotRegionGeometry::fromPath(left, type)
                            .united(ScreenshotRegionGeometry::fromPath(right, type))
                            .subtracted(QRect(15, 30, 10, 20));
        for (qreal scale : {1.0, 1.5, 2.0}) {
            const QSize pixels(qCeil(110 * scale), qCeil(90 * scale));
            const auto plan =
                makeScreenshotLayoutCompactionPlan(geometry, pixels, scale, {true, 3});
            ScreenshotResultStyle style{0, 0, Qt::black, geometry, scale};
            QImage source(pixels, QImage::Format_ARGB32_Premultiplied);
            source.fill(QColor(50, 130, 220));
            const auto original = ScreenshotResultCompositor::compose(source, style);
            QImage expected(plan.outputSize, source.format());
            QImage content(plan.outputSize, source.format());
            expected.fill(Qt::transparent);
            content.fill(Qt::transparent);
            QPainter reference(&expected), compacted(&content);
            for (const auto& tile : plan.tiles) {
                reference.drawImage(tile.destination, original, tile.source);
                compacted.drawImage(tile.destination, source, tile.source);
            }
            reference.end();
            compacted.end();
            require(ScreenshotResultCompositor::compose(
                        content, compactedScreenshotResultStyle(style, plan)) == expected,
                    "curved/freehand/subtractive masks must retain their antialiased pixels");
        }
    }
}

void sparseRasterMatchesReference(int decoration) {
    SnowCanvasRuntime runtime;
    require(runtime.isValid(), "runtime must initialize");
    SnowCanvasWidget canvas(runtime);
    canvas.resize(1400, 1000);
    canvas.show();
    QCoreApplication::processEvents();
    require(canvas.setViewportCamera(700, 500, 1), "large scene camera");
    if (decoration == 1 || decoration == 3 || decoration == 4) {
        require(canvas.setCanvasTool(decoration == 1   ? SnowCanvasTool::Shape
                                     : decoration == 4 ? SnowCanvasTool::Magnifier
                                                       : SnowCanvasTool::RectangleFilter),
                "large scene tool");
        SnowCanvasFilterStyle filter;
        filter.type = SnowCanvasFilterType::GaussianBlur;
        filter.strength = 0.2;
        if (decoration == 3)
            require(canvas.setCanvasFilterStyle(filter, SnowCanvasFilterStylePropertyType |
                                                            SnowCanvasFilterStylePropertyStrength),
                    "large scene filter");
        for (auto type :
             {QEvent::MouseButtonPress, QEvent::MouseMove, QEvent::MouseButtonRelease}) {
            const QPointF pos =
                type == QEvent::MouseButtonPress ? QPointF(100, 40) : QPointF(1300, 700);
            QMouseEvent event(type, pos, canvas.mapToGlobal(pos.toPoint()),
                              type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                              type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                              Qt::NoModifier);
            QCoreApplication::sendEvent(&canvas, &event);
        }
        require(canvas.canvasHistoryState().canUndo, "large scene drawing committed");
    } else if (decoration == 2) {
        SnowCanvasWatermarkConfig watermark;
        watermark.text = QStringLiteral("Snow Shot");
        watermark.gap = 120;
        require(canvas.setCanvasWatermarkConfig(watermark), "large scene watermark");
    }
    QImage source(1400, 1000, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < source.height(); ++y)
        for (int x = 0; x < source.width(); ++x)
            source.setPixel(x, y, qRgb(x % 255, y % 255, (x + y) % 255));
    const QList<CanvasExportSource> sources{{source, QRectF(0, 0, 1400, 1000)}};
    const QRect frame(10, 10, 1380, 980);
    const auto geometry = regions({{0, 0, 550, 600}, {1100, 0, 280, 600}, {0, 900, 550, 80}});
    for (qreal scale : {0.5, 1.0, 1.25, 2.0}) {
        const QSize pixels(qCeil(frame.width() * scale), qCeil(frame.height() * scale));
        const auto plan = makeScreenshotLayoutCompactionPlan(geometry, pixels, scale, {true, 3});
        const auto reference = runtime.renderToImage(frame, pixels, sources);
        QImage expected(plan.outputSize, QImage::Format_ARGB32_Premultiplied);
        expected.fill(Qt::transparent);
        QPainter painter(&expected);
        QList<CanvasExportTile> tiles;
        for (const auto& tile : plan.tiles) {
            painter.drawImage(tile.destination, reference, tile.source);
            tiles.append({tile.source, tile.destination});
        }
        painter.end();
        const auto actual =
            runtime.renderTilesToImage(frame, pixels, plan.outputSize, tiles, sources);
        require(actual.size() == expected.size(), "batched output dimensions");
        int maximumDifference = 0;
        int alphaErrors = 0;
        for (int y = 0; y < actual.height(); ++y) {
            for (int x = 0; x < actual.width(); ++x) {
                const QRgb a = actual.pixel(x, y), b = expected.pixel(x, y);
                // Qt's fractional SIMD interpolation changes precision at clipped
                // span endpoints. Integer exports are exact; fractional exports
                // allow at most five levels at this pattern's 254-level edges.
                alphaErrors += qAlpha(a) != qAlpha(b);
                maximumDifference =
                    std::max({maximumDifference, std::abs(qRed(a) - qRed(b)),
                              std::abs(qGreen(a) - qGreen(b)), std::abs(qBlue(a) - qBlue(b))});
            }
        }
        // Rotated glyph images have the same clipped-span rounding at integral
        // export scales. All other integral and half-scale samples are exact.
        const int tolerance = scale == 1.25 ? 5 : decoration == 2 ? 2 : 0;
        require(alphaErrors == 0 && maximumDifference <= tolerance,
                "batched pixels must retain source samples and exact coverage");
    }
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    bandsPreserveArrangement();
    invalidRasterRequestsFailBeforeAllocation();
    contoursRetainHolesAndCurves();
    shadowFootprintsDefineSpacing();
    roundedMasksSurviveJoining();
    nonRectangularMasksFollowSource();
    for (int decoration : {0, 1, 2, 3, 4})
        sparseRasterMatchesReference(decoration);
    std::cout << "Layout compaction tests passed\n";
}
