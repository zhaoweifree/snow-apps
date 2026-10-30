#include "snow_shot/presentation/screenshotresultcompositor.h"
#include "snow_shot/presentation/screenshotselectionshadowrenderer.h"

#include <QApplication>
#include <QPainter>
#include <QRegion>
#include <QTransform>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

struct Fixture {
    const char* name;
    ScreenshotRegionGeometry region;
    int cornerRadius = 0;
};

std::vector<Fixture> regionFixtures() {
    const QRegion connected =
        (QRegion(QRect(48, 44, 460, 240)) + QRect(360, 200, 280, 270)) - QRect(110, 100, 130, 80);
    const QRegion dense = QRegion(QRect(48, 44, 1240, 900)) - QRect(470, 315, 270, 210);
    const QRegion sparse = QRegion(QRect(48, 44, 124, 96)) + QRect(1200, 900, 76, 40);
    QPainterPath curves;
    curves.setFillRule(Qt::OddEvenFill);
    curves.addEllipse(QRectF(48.25, 44.5, 540.5, 420.25));
    curves.addEllipse(QRectF(180.5, 140.25, 200.25, 140.5));
    curves.moveTo(1120.5, 780.25);
    curves.cubicTo(1290.25, 770.5, 1310.75, 920.25, 1190.25, 940.5);
    curves.cubicTo(1080.5, 930.25, 1100.25, 850.5, 1120.5, 780.25);
    curves.closeSubpath();
    return {{"connected rounded union and hole", connected, 18},
            {"dense tiled union and hole", dense, 24},
            {"sparse separated islands", sparse, 12},
            {"custom curves and hole",
             ScreenshotRegionGeometry::fromPath(curves, ScreenshotRegionType::Curve), 0}};
}

QImage surface(const QSize& viewSize, qreal dpr) {
    QImage image(QSize(qCeil(viewSize.width() * dpr), qCeil(viewSize.height() * dpr)),
                 QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(dpr);
    image.fill(Qt::transparent);
    return image;
}

QImage referenceShadow(const ScreenshotRegionGeometry& region, int cornerRadius, int shadowWidth,
                       const QColor& color, const QTransform& canvasToView, const QSize& viewSize,
                       qreal dpr, const QRegion& exposed) {
    const QRect bounds = region.boundingRect();
    QImage empty(bounds.size(), QImage::Format_ARGB32_Premultiplied);
    empty.fill(Qt::transparent);
    ScreenshotResultStyle style{cornerRadius, shadowWidth, color,
                                region.translated(-bounds.topLeft())};
    const QImage composed = ScreenshotResultCompositor::compose(empty, style);
    require(!composed.isNull(), "reference compositor must produce a shadow");
    QImage image = surface(viewSize, dpr);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setClipRegion(exposed);
    painter.setCompositionMode(QPainter::CompositionMode_DestinationOver);
    painter.drawImage(canvasToView.mapRect(QRectF(
                          bounds.adjusted(-shadowWidth, -shadowWidth, shadowWidth, shadowWidth))),
                      composed);
    return image;
}

QImage previewShadow(ScreenshotRegionShadowRenderer& renderer,
                     const ScreenshotRegionGeometry& region, int cornerRadius, int shadowWidth,
                     const QColor& color, const QTransform& canvasToView, const QSize& viewSize,
                     qreal dpr, const QRegion& exposed) {
    QImage image = surface(viewSize, dpr);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setClipRegion(exposed);
    painter.setCompositionMode(QPainter::CompositionMode_DestinationOver);
    renderer.render(painter, region, cornerRadius, shadowWidth, color, canvasToView, exposed);
    return image;
}

void requireMatchingPixels(const QImage& actual, const QImage& expected, const std::string& label,
                           int tolerance = 0) {
    require(actual.size() == expected.size(), "shadow images must have matching dimensions");
    int maximumDifference = 0;
    QPoint firstMismatch(-1, -1);
    QRect mismatchBounds;
    std::size_t mismatchCount = 0;
    for (int y = 0; y < actual.height(); ++y) {
        const auto* actualRow = reinterpret_cast<const QRgb*>(actual.constScanLine(y));
        const auto* expectedRow = reinterpret_cast<const QRgb*>(expected.constScanLine(y));
        for (int x = 0; x < actual.width(); ++x) {
            const QRgb a = actualRow[x], e = expectedRow[x];
            const int difference =
                std::max({std::abs(qAlpha(a) - qAlpha(e)), std::abs(qRed(a) - qRed(e)),
                          std::abs(qGreen(a) - qGreen(e)), std::abs(qBlue(a) - qBlue(e))});
            maximumDifference = std::max(maximumDifference, difference);
            if (difference > tolerance) {
                if (firstMismatch.x() < 0)
                    firstMismatch = {x, y};
                mismatchBounds = mismatchBounds.united(QRect(x, y, 1, 1));
                ++mismatchCount;
            }
        }
    }
    if (maximumDifference > tolerance) {
        std::cerr << label << ": maximum premultiplied channel difference=" << maximumDifference
                  << " first mismatch=" << firstMismatch.x() << ',' << firstMismatch.y()
                  << " mismatch count=" << mismatchCount << " bounds=" << mismatchBounds.x() << ','
                  << mismatchBounds.y() << ',' << mismatchBounds.width() << ','
                  << mismatchBounds.height() << '\n';
        throw std::runtime_error("shadow-only tiles must preserve compositor pixels");
    }
}

void shapedShadowsMatchExistingCompositor() {
    const QSize viewSize(1640, 1250);
    const QRegion exposed(QRect(QPoint(), viewSize));
    const std::array<int, 5> widths{1, 7, 20, 1, 1};
    const std::array<QColor, 5> colors{QColor(20, 30, 40, 160), QColor(180, 40, 220, 255),
                                       QColor(20, 180, 90, 45), QColor(20, 30, 40, 160),
                                       QColor(20, 30, 40, 160)};
    std::array<QTransform, 5> transforms;
    transforms[1].translate(13.25, 17.5);
    transforms[1].scale(.83, .83);
    transforms[2].translate(-3.5, 9.25);
    transforms[2].scale(1.25, 1.25);
    transforms[3].translate(13.25, 17.5);
    transforms[3].scale(.2, .2);
    transforms[4].translate(13.25, 17.5);
    transforms[4].scale(.23, .23);
    for (const auto& fixture : regionFixtures()) {
        for (const qreal dpr : {1.0, 1.25, 2.0}) {
            for (std::size_t i = 0; i < transforms.size(); ++i) {
                ScreenshotRegionShadowRenderer renderer;
                const auto expected =
                    referenceShadow(fixture.region, fixture.cornerRadius, widths[i], colors[i],
                                    transforms[i], viewSize, dpr, exposed);
                const auto actual =
                    previewShadow(renderer, fixture.region, fixture.cornerRadius, widths[i],
                                  colors[i], transforms[i], viewSize, dpr, exposed);
                const std::string label = std::string(fixture.name) +
                                          " dpr=" + std::to_string(dpr) +
                                          " transform=" + std::to_string(i);
                requireMatchingPixels(actual, expected, label);
            }
        }
    }
}

void clippedExposureAndStyleChangesPreservePixels() {
    const QSize viewSize(1460, 1120);
    const auto fixture = regionFixtures()[1];
    QTransform transform;
    transform.translate(7.25, 11.5);
    transform.scale(1.07, 1.07);
    const QRegion exposed = QRegion(QRect(0, 0, 570, 1120)) + QRect(690, 200, 770, 650);
    for (const qreal dpr : {1.0, 1.25, 2.0}) {
        ScreenshotRegionShadowRenderer renderer;
        const std::array<int, 5> widths{9, 9, 21, 21, 21};
        const std::array<QColor, 5> colors{QColor(20, 30, 40, 160), QColor(190, 20, 70, 220),
                                           QColor(190, 20, 70, 220), QColor(40, 160, 20, 0),
                                           QColor(40, 160, 20, 55)};
        for (std::size_t i = 0; i < widths.size(); ++i) {
            const auto expected = referenceShadow(fixture.region, fixture.cornerRadius, widths[i],
                                                  colors[i], transform, viewSize, dpr, exposed);
            const auto actual =
                previewShadow(renderer, fixture.region, fixture.cornerRadius, widths[i], colors[i],
                              transform, viewSize, dpr, exposed);
            requireMatchingPixels(actual, expected, "clipped style change");
        }
        const QRegion fullExposure(QRect(QPoint(), viewSize));
        requireMatchingPixels(previewShadow(renderer, fixture.region, fixture.cornerRadius, 21,
                                            colors.back(), transform, viewSize, dpr, fullExposure),
                              referenceShadow(fixture.region, fixture.cornerRadius, 21,
                                              colors.back(), transform, viewSize, dpr,
                                              fullExposure),
                              "expanding exposure must build previously hidden boundary tiles");
    }
}

void coverageAndBlurAreReusedAcrossRepaintColorAndTranslation() {
    const auto fixture = regionFixtures()[0];
    const QSize viewSize(900, 700);
    const QRegion exposed(QRect(QPoint(), viewSize));
    ScreenshotRegionShadowRenderer renderer;
    const QColor firstColor(20, 30, 40, 160);
    const auto first = previewShadow(renderer, fixture.region, fixture.cornerRadius, 13, firstColor,
                                     QTransform(), viewSize, 1.25, exposed);
    require(renderer.retainedBytes() > 0, "shadow-only renderer must retain its boundary assets");
    ScreenshotSelectionShadowRenderer::resetDiagnosticsForCurrentThread();
    const auto repeated = previewShadow(renderer, fixture.region, fixture.cornerRadius, 13,
                                        firstColor, QTransform(), viewSize, 1.25, exposed);
    auto work = ScreenshotSelectionShadowRenderer::diagnosticsForCurrentThread();
    require(first == repeated, "unchanged shadow repaint must preserve pixels exactly");
    require(work.regionMaskBuilds == 0 && work.regionShadowBuilds == 0,
            "unchanged shadow repaint must reuse coverage and blur");

    const QColor nextColor(170, 20, 90, 230);
    ScreenshotSelectionShadowRenderer::resetDiagnosticsForCurrentThread();
    const auto recolored = previewShadow(renderer, fixture.region, fixture.cornerRadius, 13,
                                         nextColor, QTransform(), viewSize, 1.25, exposed);
    work = ScreenshotSelectionShadowRenderer::diagnosticsForCurrentThread();
    require(work.regionMaskBuilds == 0 && work.regionShadowBuilds == 0,
            "color and alpha changes must reuse color-independent coverage and blur");
    requireMatchingPixels(recolored,
                          referenceShadow(fixture.region, fixture.cornerRadius, 13, nextColor,
                                          QTransform(), viewSize, 1.25, exposed),
                          "recolored cached blur");

    const auto moved = fixture.region.translated(17, 23);
    ScreenshotSelectionShadowRenderer::resetDiagnosticsForCurrentThread();
    const auto translated = previewShadow(renderer, moved, fixture.cornerRadius, 13, nextColor,
                                          QTransform(), viewSize, 1.25, exposed);
    work = ScreenshotSelectionShadowRenderer::diagnosticsForCurrentThread();
    require(work.regionMaskBuilds == 0 && work.regionShadowBuilds == 0,
            "integer selection translation must retain canonical coverage and blur");
    requireMatchingPixels(translated,
                          referenceShadow(moved, fixture.cornerRadius, 13, nextColor, QTransform(),
                                          viewSize, 1.25, exposed),
                          "translated cached blur");

    renderer.reset();
    require(renderer.retainedBytes() == 0, "reset must release every boundary asset");
    ScreenshotSelectionShadowRenderer::resetDiagnosticsForCurrentThread();
    const auto reset = previewShadow(renderer, moved, fixture.cornerRadius, 13, nextColor,
                                     QTransform(), viewSize, 1.25, exposed);
    work = ScreenshotSelectionShadowRenderer::diagnosticsForCurrentThread();
    require(work.regionMaskBuilds > 0 && work.regionShadowBuilds > 0,
            "reset renderer must rebuild its boundary assets");
    require(reset == translated, "reset must preserve the shadow output");

    ScreenshotRegionShadowRenderer independent;
    ScreenshotSelectionShadowRenderer::resetDiagnosticsForCurrentThread();
    const auto isolated = previewShadow(independent, moved, fixture.cornerRadius, 13, nextColor,
                                        QTransform(), viewSize, 1.25, exposed);
    work = ScreenshotSelectionShadowRenderer::diagnosticsForCurrentThread();
    require(work.regionMaskBuilds > 0 && work.regionShadowBuilds > 0,
            "separate overlay instances must own independent boundary assets");
    require(isolated == translated, "independent cache must preserve the shadow output");
    independent.reset();
    require(independent.retainedBytes() == 0 && renderer.retainedBytes() > 0,
            "resetting one overlay must release only its own shadow assets");
}

void fractionalStripRepaintsReuseComposition() {
    const auto fixture = regionFixtures()[1];
    const QSize viewSize(1460, 1120);
    const QRegion fullExposure(QRect(QPoint(), viewSize));
    const QRegion clippedExposure(QRect(0, 0, 600, viewSize.height()));
    constexpr int width = 13;
    const QColor firstColor(20, 30, 40, 160);
    const QColor nextColor(190, 20, 70, 220);
    ScreenshotRegionShadowRenderer renderer;

    // Populate all primary boundary assets independently of the fractional strip
    // cache, so expanding exposure can prove those assets survive strip invalidation.
    static_cast<void>(previewShadow(renderer, fixture.region, fixture.cornerRadius, width,
                                    firstColor, QTransform(), viewSize, 1, fullExposure));
    ScreenshotSelectionShadowRenderer::resetDiagnosticsForCurrentThread();
    const auto first = previewShadow(renderer, fixture.region, fixture.cornerRadius, width,
                                     firstColor, QTransform(), viewSize, 1.25, clippedExposure);
    auto work = ScreenshotSelectionShadowRenderer::diagnosticsForCurrentThread();
    require(work.regionStripBuilds > 0 && work.regionMaskBuilds == 0 &&
                work.regionShadowBuilds == 0,
            "fractional composition must build strips from retained boundary assets");
    requireMatchingPixels(first,
                          referenceShadow(fixture.region, fixture.cornerRadius, width, firstColor,
                                          QTransform(), viewSize, 1.25, clippedExposure),
                          "initial clipped fractional strips");

    ScreenshotSelectionShadowRenderer::resetDiagnosticsForCurrentThread();
    const auto repeated = previewShadow(renderer, fixture.region, fixture.cornerRadius, width,
                                        firstColor, QTransform(), viewSize, 1.25, clippedExposure);
    work = ScreenshotSelectionShadowRenderer::diagnosticsForCurrentThread();
    require(work.regionStripBuilds == 0 && work.regionMaskBuilds == 0 &&
                work.regionShadowBuilds == 0,
            "warm fractional repaint must reuse strip composition, coverage and blur");
    require(first == repeated, "warm fractional strips must preserve pixels exactly");

    ScreenshotSelectionShadowRenderer::resetDiagnosticsForCurrentThread();
    const auto recolored = previewShadow(renderer, fixture.region, fixture.cornerRadius, width,
                                         nextColor, QTransform(), viewSize, 1.25, clippedExposure);
    work = ScreenshotSelectionShadowRenderer::diagnosticsForCurrentThread();
    require(work.regionStripBuilds > 0 && work.regionMaskBuilds == 0 &&
                work.regionShadowBuilds == 0,
            "color changes must rebuild fractional composition while reusing coverage and blur");
    requireMatchingPixels(recolored,
                          referenceShadow(fixture.region, fixture.cornerRadius, width, nextColor,
                                          QTransform(), viewSize, 1.25, clippedExposure),
                          "recolored clipped fractional strips");

    ScreenshotSelectionShadowRenderer::resetDiagnosticsForCurrentThread();
    const auto expanded = previewShadow(renderer, fixture.region, fixture.cornerRadius, width,
                                        nextColor, QTransform(), viewSize, 1.25, fullExposure);
    work = ScreenshotSelectionShadowRenderer::diagnosticsForCurrentThread();
    require(
        work.regionStripBuilds > 0 && work.regionMaskBuilds == 0 && work.regionShadowBuilds == 0,
        "expanded exposure must replace incomplete strips and preserve primary boundary assets");
    requireMatchingPixels(expanded,
                          referenceShadow(fixture.region, fixture.cornerRadius, width, nextColor,
                                          QTransform(), viewSize, 1.25, fullExposure),
                          "expanded fractional strip exposure");

    ScreenshotSelectionShadowRenderer::resetDiagnosticsForCurrentThread();
    require(expanded == previewShadow(renderer, fixture.region, fixture.cornerRadius, width,
                                      nextColor, QTransform(), viewSize, 1.25, fullExposure),
            "expanded fractional strips must preserve their cached pixels");
    work = ScreenshotSelectionShadowRenderer::diagnosticsForCurrentThread();
    require(work.regionStripBuilds == 0 && work.regionMaskBuilds == 0 &&
                work.regionShadowBuilds == 0,
            "expanded fractional exposure must reuse its complete strip working set");

    const auto moved = fixture.region.translated(17, 23);
    ScreenshotSelectionShadowRenderer::resetDiagnosticsForCurrentThread();
    const auto translated = previewShadow(renderer, moved, fixture.cornerRadius, width, nextColor,
                                          QTransform(), viewSize, 1.25, fullExposure);
    work = ScreenshotSelectionShadowRenderer::diagnosticsForCurrentThread();
    require(work.regionStripBuilds == 0 && work.regionMaskBuilds == 0 &&
                work.regionShadowBuilds == 0,
            "translation with unchanged visible sources must reuse canonical fractional strips");
    requireMatchingPixels(translated,
                          referenceShadow(moved, fixture.cornerRadius, width, nextColor,
                                          QTransform(), viewSize, 1.25, fullExposure),
                          "translated cached fractional strips");

    QTransform downscaled;
    downscaled.translate(13.25, 17.5);
    downscaled.scale(.23, .23);
    ScreenshotSelectionShadowRenderer::resetDiagnosticsForCurrentThread();
    const auto changedLayout = previewShadow(renderer, moved, fixture.cornerRadius, width,
                                             nextColor, downscaled, viewSize, 1.5, fullExposure);
    work = ScreenshotSelectionShadowRenderer::diagnosticsForCurrentThread();
    require(work.regionStripBuilds > 0 && work.regionMaskBuilds == 0 &&
                work.regionShadowBuilds == 0,
            "DPR and strip-halo layout changes must preserve primary coverage and blur");
    requireMatchingPixels(changedLayout,
                          referenceShadow(moved, fixture.cornerRadius, width, nextColor, downscaled,
                                          viewSize, 1.5, fullExposure),
                          "changed fractional strip layout");
    ScreenshotSelectionShadowRenderer::resetDiagnosticsForCurrentThread();
    require(changedLayout == previewShadow(renderer, moved, fixture.cornerRadius, width, nextColor,
                                           downscaled, viewSize, 1.5, fullExposure),
            "changed strip layout must preserve cached pixels");
    work = ScreenshotSelectionShadowRenderer::diagnosticsForCurrentThread();
    require(work.regionStripBuilds == 0 && work.regionMaskBuilds == 0 &&
                work.regionShadowBuilds == 0,
            "changed strip layout must reuse its complete warm working set");
    require(renderer.retainedBytes() <= 64u * 1024u * 1024u,
            "primary assets and fractional strips must share the shadow cache byte budget");
}

void oversizedOutputReusesBoundedBoundaryTiles() {
    const QSize viewSize(5720, 3100);
    const QRegion exposed(QRect(QPoint(), viewSize));
    const ScreenshotRegionGeometry region =
        QRegion(QRect(48, 44, 5620, 3000)) - QRect(1900, 700, 1800, 1500);
    constexpr int width = 16;
    const QColor color(20, 30, 40, 170);
    const auto outputSize = region.boundingRect().size() + QSize(width * 2, width * 2);
    require(qint64(outputSize.width()) * outputSize.height() * 4 > 64 * 1024 * 1024,
            "regression fixture must cross the old full-shadow cache limit");
    ScreenshotRegionShadowRenderer renderer;
    ScreenshotSelectionShadowRenderer::resetDiagnosticsForCurrentThread();
    const auto first =
        previewShadow(renderer, region, 18, width, color, QTransform(), viewSize, 1, exposed);
    auto work = ScreenshotSelectionShadowRenderer::diagnosticsForCurrentThread();
    require(work.regionMaskBuilds > 0 && work.regionShadowBuilds > 0,
            "large fixture must build shadow boundary tiles");
    require(work.selectionSizedTransientAllocations == 0,
            "shadow-only rendering must avoid selection-sized content and output allocations");
    require(work.regionScratchPeakBytes < 8u * 1024u * 1024u,
            "large shadow generation must keep temporary blur storage bounded by tile size");
    require(renderer.retainedBytes() > 0 && renderer.retainedBytes() <= 64u * 1024u * 1024u,
            "large output must retain bounded boundary assets");
    requireMatchingPixels(
        first, referenceShadow(region, 18, width, color, QTransform(), viewSize, 1, exposed),
        "output above the former 64 MiB cache cliff");
    ScreenshotSelectionShadowRenderer::resetDiagnosticsForCurrentThread();
    const auto repeated =
        previewShadow(renderer, region, 18, width, color, QTransform(), viewSize, 1, exposed);
    work = ScreenshotSelectionShadowRenderer::diagnosticsForCurrentThread();
    require(work.regionMaskBuilds == 0 && work.regionShadowBuilds == 0,
            "large unchanged preview must reuse its complete boundary working set");
    require(first == repeated, "large cached tiles must preserve pixels exactly");
    renderer.reset();
    require(renderer.retainedBytes() == 0, "large preview reset must release every cached tile");
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    try {
        shapedShadowsMatchExistingCompositor();
        clippedExposureAndStyleChangesPreservePixels();
        coverageAndBlurAreReusedAcrossRepaintColorAndTranslation();
        fractionalStripRepaintsReuseComposition();
        oversizedOutputReusesBoundedBoundaryTiles();
        ScreenshotSelectionShadowRenderer::resetCacheForCurrentThread();
        std::cout << "Screenshot region shadow renderer tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Screenshot region shadow renderer tests failed: " << error.what() << '\n';
        return 1;
    }
}
