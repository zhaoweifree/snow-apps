#include "snow_canvas_display_cache.h"
#include "snow_canvas_export.h"
#include "snow_canvas_ffi_handles.h"
#include "snow_canvas_filter_tile_cache.h"
#include "snow_canvas_magnifier_renderer.h"
#include "snow_canvas_render_geometry.h"
#include "snow_canvas_renderer.h"
#include "snow_canvas_viewport.h"
#include "snow_canvas_widget_selection_hit_testing.h"
#include "snow_draw_engine_qt/snow_canvas_custom_renderer.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"

#include <QApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QDir>
#include <QFontDatabase>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <tuple>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

SceneDisplayInfo projection() {
    SceneDisplayInfo info{};
    info.surface_width = 200;
    info.surface_height = 140;
    info.camera_center_x = 100;
    info.camera_center_y = 70;
    info.camera_zoom = 1;
    return info;
}

SnowCanvasSceneItem magnifier(double factor = 2.0,
                              std::uint8_t shape = SNOW_DISPLAY_RECT_SHAPE_RECTANGLE) {
    SnowSceneDisplayItem item{};
    item.kind = SNOW_SCENE_DISPLAY_ITEM_MAGNIFIER;
    item.element_id = {1, 1};
    item.center_x = 130;
    item.center_y = 70;
    item.width = 30 * factor;
    item.height = 30 * factor;
    item.opacity = 1;
    item.rect_shape = shape;
    item.stroke = {30, 40, 50, 255};
    item.stroke_width = 2;
    item.magnifier = {35, 70, 30, 30, factor, SNOW_MAGNIFIER_CONNECTION_NONE, 0, {}};
    return SnowCanvasSceneItem(item);
}

QImage sourceImage() {
    QImage image(200, 140, QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(30, 90, 180));
    QPainter painter(&image);
    painter.fillRect(QRect(20, 55, 15, 15), QColor(20, 190, 80));
    painter.fillRect(QRect(35, 55, 15, 15), QColor(230, 180, 20));
    painter.fillRect(QRect(20, 70, 15, 15), QColor(210, 40, 80));
    painter.fillRect(QRect(35, 70, 15, 15), QColor(70, 30, 200));
    return image;
}

QImage render(const std::vector<SnowCanvasSceneItem>& items,
              const QList<SnowCanvasBaseImageSource>& sources, qreal dpr = 1.0,
              const SceneDisplayInfo& info = projection(), QRegion exposed = {}, bool tiled = false,
              const void* cacheNamespace = nullptr, std::uint64_t contentKey = 0) {
    QImage output(QSize(qCeil(info.surface_width * dpr), qCeil(info.surface_height * dpr)),
                  QImage::Format_ARGB32_Premultiplied);
    output.setDevicePixelRatio(dpr);
    output.fill(Qt::transparent);
    if (exposed.isEmpty())
        exposed = QRegion(QRect(0, 0, qCeil(info.surface_width), qCeil(info.surface_height)));
    QPainter painter(&output);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setClipRegion(exposed);
    snow_canvas_renderer::SceneRenderRequest request;
    request.painter = &painter;
    request.displayInfo = &info;
    request.sceneItems = items.data();
    request.sceneItemCount = static_cast<std::uint32_t>(items.size());
    request.exposedRegion = exposed;
    request.clearBackgroundEnabled = false;
    request.baseImageSources = &sources;
    request.enableFilterTileCache = tiled;
    request.cacheNamespace = cacheNamespace;
    request.filterTileContentKey = contentKey;
    if (tiled)
        snow_canvas_renderer::renderSceneItemsTiled(request);
    else
        snow_canvas_renderer::renderSceneItems(request);
    return output;
}

QColor pixel(const QImage& image, double x, double y) {
    return image.pixelColor(qFloor(x * image.devicePixelRatio()),
                            qFloor(y * image.devicePixelRatio()));
}

void samplesOnlyOriginalPixelsAtEveryFactorAndShape() {
    const QImage original = sourceImage();
    const QList<SnowCanvasBaseImageSource> sources{{original, QRectF(0, 0, 200, 140), {}}};
    for (const auto shape : {SNOW_DISPLAY_RECT_SHAPE_RECTANGLE, SNOW_DISPLAY_RECT_SHAPE_ELLIPSE,
                             SNOW_DISPLAY_RECT_SHAPE_DIAMOND}) {
        for (const double factor : {1.0, 2.0, 10.0}) {
            auto lens = magnifier(factor, static_cast<std::uint8_t>(shape));
            lens.center_x = 100;
            auto annotation = lens;
            annotation.kind = SNOW_SCENE_DISPLAY_ITEM_DRAW_RECT;
            annotation.element_id = {2, 1};
            annotation.center_x = 35;
            annotation.center_y = 70;
            annotation.width = 35;
            annotation.height = 35;
            annotation.fill = {250, 0, 0, 255};
            annotation.stroke.a = 0;
            annotation.rect_shape = SNOW_DISPLAY_RECT_SHAPE_RECTANGLE;
            for (const double dpr : {1.0, 1.25, 2.0}) {
                const QImage output = render({annotation, lens}, sources, dpr);
                require(pixel(output, 100 + factor * 3, 70 + factor * 3) == QColor(70, 30, 200),
                        "magnifiers must enlarge original pixels, excluding source annotations");
                require(original.cacheKey() == sources.front().image.cacheKey(),
                        "drawing a magnifier must preserve shared original image storage");
            }
        }
    }
}

void shapesAndBorderClipCleanly() {
    QImage blue(200, 140, QImage::Format_ARGB32_Premultiplied);
    blue.fill(Qt::blue);
    const QList<SnowCanvasBaseImageSource> sources{{blue, QRectF(0, 0, 200, 140), {}}};
    for (const auto shape : {SNOW_DISPLAY_RECT_SHAPE_RECTANGLE, SNOW_DISPLAY_RECT_SHAPE_ELLIPSE,
                             SNOW_DISPLAY_RECT_SHAPE_DIAMOND}) {
        const auto lens = magnifier(2, static_cast<std::uint8_t>(shape));
        const QImage output = render({lens}, sources);
        require(pixel(output, 130, 70) == QColor(Qt::blue), "lens center must show sampled pixels");
        require((pixel(output, 154, 94).alpha() != 0) ==
                    (shape == SNOW_DISPLAY_RECT_SHAPE_RECTANGLE),
                "ellipse and diamond must clip their bounding rectangle corners");
        require(pixel(output, 130, 40) == QColor(30, 40, 50),
                "magnifier border must use the configured stroke color and width");
    }
    auto rotated = magnifier();
    rotated.width = 80;
    rotated.height = 20;
    rotated.rotation = std::acos(-1.0) / 2.0;
    const QImage output = render({rotated}, sources);
    require(pixel(output, 130, 95).alpha() != 0 && pixel(output, 155, 70).alpha() == 0,
            "lens clipping must rotate with its shape");
}

void roundedCornersClipSamplingAndStroke() {
    QImage blue(200, 140, QImage::Format_ARGB32_Premultiplied);
    blue.fill(Qt::blue);
    const QList<SnowCanvasBaseImageSource> sources{{blue, QRectF(0, 0, 200, 140), {}}};
    auto lens = magnifier();
    lens.corner_radii = {6, 12, 24, 0};
    for (double dpr : {1.0, 1.25, 2.0}) {
        const QImage output = render({lens}, sources, dpr);
        require(pixel(output, 100, 40).alpha() == 0 && pixel(output, 158, 41).alpha() == 0 &&
                    pixel(output, 158, 98).alpha() == 0,
                "rounded lens corners must clip sampled pixels and border at every DPR");
        require(pixel(output, 101, 98).alpha() != 0 && pixel(output, 130, 70) == QColor(Qt::blue),
                "a zero-radius corner stays square while the interior retains sampled pixels");
        require(pixel(output, 130, 40) == QColor(30, 40, 50),
                "rounding must preserve the configured lens border");
    }
    lens.rotation = std::acos(-1.0) / 2.0;
    const auto rotated = snow_canvas_magnifier_renderer::lensPath(lens);
    require(!rotated.contains(QPointF(159, 41)) && rotated.contains(QPointF(130, 70)),
            "rotation must transform the rounded clipping contour");
}

void outerBordersSurvivePartialAndOffViewportPainting() {
    const QList<SnowCanvasBaseImageSource> sources{{sourceImage(), QRectF(0, 0, 200, 140), {}}};
    for (const auto shape : {SNOW_DISPLAY_RECT_SHAPE_RECTANGLE, SNOW_DISPLAY_RECT_SHAPE_ELLIPSE,
                             SNOW_DISPLAY_RECT_SHAPE_DIAMOND}) {
        for (const double zoom : {0.5, 1.0, 2.0}) {
            SceneDisplayInfo info = projection();
            info.camera_zoom = zoom;
            auto lens = magnifier(2.0, static_cast<std::uint8_t>(shape));
            lens.center_x = info.camera_center_x;
            lens.stroke_width = 10.0;
            const double top = info.surface_height / 2.0 - lens.height * zoom / 2.0;
            const QRegion exposed(QRect(95, qFloor(top - lens.stroke_width * zoom * 0.4), 10, 1));
            // Sample inside the exposed logical pixel. At fractional DPR its top
            // edge can map to a physical pixel above the rounded device clip.
            const double sampleY = exposed.boundingRect().top() + 0.5;
            for (const double dpr : {1.0, 1.25, 2.0}) {
                const QImage full = render({lens}, sources, dpr, info);
                snow_canvas_magnifier_renderer::resetDiagnosticsForCurrentThread();
                const QImage partial = render({lens}, sources, dpr, info, exposed);
                require(pixel(full, 100, sampleY).alpha() != 0 &&
                            pixel(partial, 100, sampleY) == pixel(full, 100, sampleY),
                        "partial exposure outside the lens interior must retain its outer border");
                require(snow_canvas_magnifier_renderer::diagnosticsForCurrentThread()
                                .sourceLayerDrawCount == 0,
                        "border-only exposure must not sample the original image layers");

                auto outside = lens;
                outside.center_y =
                    info.camera_center_y -
                    (info.surface_height / 2.0 + lens.height * zoom / 2.0 + 1.0) / zoom;
                const QImage clipped = render({outside}, sources, dpr, info);
                require(pixel(clipped, 100, 0).alpha() != 0,
                        "a lens outside the viewport must paint its border extending inside it");
            }
        }
    }
}

void missingPixelsStayTransparentAndCoverageIsRespected() {
    const auto lens = magnifier();
    const QImage empty = render({lens}, {});
    require(pixel(empty, 130, 70).alpha() == 0 && pixel(empty, 130, 40).alpha() != 0,
            "without a base image the magnifier interior must remain transparent");
    QImage image(200, 140, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::green);
    const QImage partial =
        render({lens}, {{image, QRectF(0, 0, 200, 140), QRectF(20, 55, 15, 30)}});
    require(pixel(partial, 120, 70) == QColor(Qt::green) && pixel(partial, 140, 70).alpha() == 0,
            "magnifier sampling must respect layer coverage and transparent missing source pixels");
    QImage patch(15, 15, QImage::Format_ARGB32_Premultiplied);
    patch.fill(Qt::yellow);
    const QImage layered =
        render({lens}, {{image, QRectF(0, 0, 200, 140), {}}, {patch, QRectF(35, 70, 15, 15), {}}});
    require(pixel(layered, 140, 80) == QColor(Qt::yellow),
            "source layers including captured cursor patches must retain paint order");
}

QPainterPath opacityFixtureLensPath(const SnowCanvasSceneItem& lens, const SceneDisplayInfo& info) {
    const QRectF bounds(-lens.width / 2, -lens.height / 2, lens.width, lens.height);
    QPainterPath path;
    if (lens.rect_shape == SNOW_DISPLAY_RECT_SHAPE_ELLIPSE) {
        path.addEllipse(bounds);
    } else if (lens.rect_shape == SNOW_DISPLAY_RECT_SHAPE_DIAMOND) {
        path.moveTo(0, bounds.top());
        path.lineTo(bounds.right(), 0);
        path.lineTo(0, bounds.bottom());
        path.lineTo(bounds.left(), 0);
        path.closeSubpath();
    } else {
        path.addRect(bounds);
    }
    QTransform element;
    element.translate(lens.center_x, lens.center_y);
    element.rotateRadians(lens.rotation);
    const double zoom = info.camera_zoom;
    const QTransform view(zoom, 0, 0, zoom, info.surface_width / 2 - info.camera_center_x * zoom,
                          info.surface_height / 2 - info.camera_center_y * zoom);
    return view.map(element.map(path));
}

QImage renderOpacityFixture(const SnowCanvasSceneItem& lens,
                            const QList<SnowCanvasBaseImageSource>& sources, qreal dpr,
                            const QTransform& outer, const QRegion& exposure, double callerOpacity,
                            QPainter::CompositionMode mode, bool reference) {
    const auto info = projection();
    const QSize size(qCeil(info.surface_width * 2 * dpr), qCeil(info.surface_height * 2 * dpr));
    QImage output(size, QImage::Format_ARGB32_Premultiplied);
    output.setDevicePixelRatio(dpr);
    output.fill(QColor(30, 40, 70, 255));
    QImage composed;
    if (reference) {
        // Independently compose the original stack at destination resolution, without
        // element opacity or the lens mask. Apply those once to the completed stack.
        composed = QImage(size, output.format());
        composed.setDevicePixelRatio(dpr);
        composed.fill(Qt::transparent);
        QPainter layers(&composed);
        layers.setRenderHint(QPainter::Antialiasing, true);
        layers.setRenderHint(QPainter::SmoothPixmapTransform, true);
        layers.setWorldTransform(outer);
        const double scale = lens.magnifier.magnification_factor * info.camera_zoom;
        const QPointF destination(lens.center_x, lens.center_y);
        const QTransform sourceToView(scale, 0, 0, scale,
                                      destination.x() - lens.magnifier.source_center_x * scale,
                                      destination.y() - lens.magnifier.source_center_y * scale);
        for (const auto& source : sources) {
            layers.save();
            if (!source.coverage.isEmpty()) {
                QPainterPath coverage;
                coverage.addRect(source.coverage.intersected(source.canvasRect));
                layers.setClipPath(sourceToView.map(coverage), Qt::IntersectClip);
            }
            layers.setWorldTransform(sourceToView, true);
            layers.drawImage(source.canvasRect, source.image, QRectF(source.image.rect()));
            layers.restore();
        }
        layers.end();
        composed.setDevicePixelRatio(1);
    }
    QPainter painter(&output);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setWorldTransform(outer);
    painter.setClipRegion(exposure);
    painter.setOpacity(callerOpacity);
    painter.setCompositionMode(mode);
    if (reference) {
        painter.setClipPath(opacityFixtureLensPath(lens, info), Qt::IntersectClip);
        painter.setOpacity(callerOpacity * lens.opacity);
        painter.setWorldTransform(QTransform());
        painter.setWorldTransform(painter.deviceTransform().inverted());
        painter.drawImage(QPoint(), composed);
    } else {
        const QTransform worldTransform = painter.worldTransform();
        const QTransform deviceTransform = painter.deviceTransform();
        const bool hasClipping = painter.hasClipping();
        const QPainterPath clipPath = painter.clipPath();
        const QRegion clipRegion = painter.clipRegion();
        const double opacity = painter.opacity();
        const QPainter::CompositionMode compositionMode = painter.compositionMode();
        const QPainter::RenderHints renderHints = painter.renderHints();
        snow_canvas_magnifier_renderer::render(painter, info, lens, &sources);
        require(painter.worldTransform() == worldTransform &&
                    painter.deviceTransform() == deviceTransform &&
                    painter.hasClipping() == hasClipping && painter.clipPath() == clipPath &&
                    painter.clipRegion() == clipRegion && painter.opacity() == opacity &&
                    painter.compositionMode() == compositionMode &&
                    painter.renderHints() == renderHints,
                "magnifier rendering must preserve caller transforms, clips, opacity, blend "
                "mode and render hints");
    }
    return output;
}

void requirePremultipliedPixelsNear(const QImage& actual, const QImage& expected,
                                    const char* message) {
    require(actual.size() == expected.size(), "opacity reference size must match");
    for (int y = 0; y < actual.height(); ++y) {
        for (int x = 0; x < actual.width(); ++x) {
            const QRgb a = actual.pixel(x, y);
            const QRgb b = expected.pixel(x, y);
            require(qAbs(qRed(a) - qRed(b)) <= 1 && qAbs(qGreen(a) - qGreen(b)) <= 1 &&
                        qAbs(qBlue(a) - qBlue(b)) <= 1 && qAbs(qAlpha(a) - qAlpha(b)) <= 1,
                    message);
        }
    }
}

void multilayerOpacityComposesOriginalsBeforeElementAppearance() {
    QImage original(200, 140, QImage::Format_ARGB32_Premultiplied);
    original.fill(Qt::green);
    QImage cursor(15, 15, QImage::Format_ARGB32_Premultiplied);
    auto lens = magnifier();
    lens.stroke.a = 0;
    lens.corner_radii = {};
    lens.opacity = 0.5;
    for (const QColor patchColor : {QColor(Qt::yellow), QColor(240, 35, 80, 120)}) {
        cursor.fill(patchColor);
        const QList<SnowCanvasBaseImageSource> sources{
            {original, QRectF(0, 0, 200, 140), {}},
            {cursor, QRectF(35, 70, 15, 15), {}},
            {cursor, QRectF(28, 63, 15, 15), QRectF(29.25, 64.5, 6.5, 9.25)}};
        for (const auto shape : {SNOW_DISPLAY_RECT_SHAPE_RECTANGLE, SNOW_DISPLAY_RECT_SHAPE_ELLIPSE,
                                 SNOW_DISPLAY_RECT_SHAPE_DIAMOND}) {
            lens.rect_shape = static_cast<std::uint8_t>(shape);
            for (double rotation : {0.0, 0.37}) {
                lens.rotation = rotation;
                for (double dpr : {1.0, 1.25, 2.0}) {
                    for (const QTransform& outer :
                         {QTransform(), QTransform(1.7, 0, 0, 1.25, 5.3, 7.1)}) {
                        for (const QRegion& exposure :
                             {QRegion(QRect(0, 0, 200, 140)), QRegion(QRect(119, 62, 13, 17))}) {
                            for (double opacity : {0.5, 1.0}) {
                                lens.opacity = opacity;
                                const QImage actual = renderOpacityFixture(
                                    lens, sources, dpr, outer, exposure, 1,
                                    QPainter::CompositionMode_SourceOver, false);
                                const QImage expected = renderOpacityFixture(
                                    lens, sources, dpr, outer, exposure, 1,
                                    QPainter::CompositionMode_SourceOver, true);
                                requirePremultipliedPixelsNear(
                                    actual, expected,
                                    "lens opacity and its mask must apply once to opaque and "
                                    "translucent originals under rotation, exposure, DPR and "
                                    "export transforms");
                            }
                        }
                    }
                }
            }
        }
        lens.rect_shape = SNOW_DISPLAY_RECT_SHAPE_ELLIPSE;
        lens.rotation = 0.37;
        for (const auto mode :
             {QPainter::CompositionMode_SourceOver, QPainter::CompositionMode_Multiply}) {
            for (const auto& appearance :
                 {std::pair{0.4, 1.0}, std::pair{1.0, 0.5}, std::pair{0.4, 0.5}}) {
                const double callerOpacity = appearance.first;
                lens.opacity = appearance.second;
                const QRegion exposure(QRect(0, 0, 200, 140));
                const QTransform outer(1.7, 0, 0, 1.25, 5.3, 7.1);
                requirePremultipliedPixelsNear(
                    renderOpacityFixture(lens, sources, 1.25, outer, exposure, callerOpacity, mode,
                                         false),
                    renderOpacityFixture(lens, sources, 1.25, outer, exposure, callerOpacity, mode,
                                         true),
                    "the sampled stack must compose with SourceOver before applying caller "
                    "opacity and its final blend mode");
            }
        }
        lens.opacity = 0.5;
    }
}

void multilayerOpacityTilesKeepSparseClipsAndBoundedScratch() {
    QImage original(200, 140, QImage::Format_ARGB32_Premultiplied);
    original.fill(Qt::green);
    QImage patch(original.size(), original.format());
    patch.fill(QColor(240, 35, 80, 120));
    const QList<SnowCanvasBaseImageSource> sources{{original, QRectF(0, 0, 200, 140), {}},
                                                   {patch, QRectF(0, 0, 200, 140), {}}};
    auto lens = magnifier(10);
    lens.stroke.a = 0;
    lens.corner_radii = {};
    lens.rotation = 0.37;
    lens.opacity = 0.5;
    const QRegion full(QRect(0, 0, 200, 140));
    const QRegion sparse = QRegion(QRect(20, 20, 13, 17)) + QRect(170, 110, 15, 19);
    for (const QTransform& outer : {QTransform(), QTransform(1.3, 0.15, 0.2, 1.1, -13.7, 7.1),
                                    QTransform(1.9, 0, 0, 1.9, 5.3, 7.1)}) {
        for (const QRegion& exposure : {full, sparse}) {
            snow_canvas_magnifier_renderer::resetDiagnosticsForCurrentThread();
            const QImage actual = renderOpacityFixture(lens, sources, 2, outer, exposure, 0.4,
                                                       QPainter::CompositionMode_SourceOver, false);
            const auto diagnostics = snow_canvas_magnifier_renderer::diagnosticsForCurrentThread();
            require(diagnostics.compositedTileCount > 1 &&
                        diagnostics.peakCompositingPixelCount <= 256u * 256u,
                    "large magnifiers must composite across tile seams with bounded scratch");
            requirePremultipliedPixelsNear(
                actual,
                renderOpacityFixture(lens, sources, 2, outer, exposure, 0.4,
                                     QPainter::CompositionMode_SourceOver, true),
                "device-space compositing tiles must preserve sparse clips, transform "
                "cancellation and opacity across their seams");
        }
    }
    const QRegion tiny(QRect(119, 62, 13, 17));
    snow_canvas_magnifier_renderer::resetDiagnosticsForCurrentThread();
    renderOpacityFixture(lens, sources, 2, QTransform(), tiny, 1,
                         QPainter::CompositionMode_SourceOver, false);
    const auto tinyDiagnostics = snow_canvas_magnifier_renderer::diagnosticsForCurrentThread();
    require(tinyDiagnostics.compositedTileCount == 1 &&
                tinyDiagnostics.peakCompositingPixelCount <= 26u * 34u,
            "partial exposure must bound scratch by visible device pixels, not lens size");
    snow_canvas_magnifier_renderer::resetDiagnosticsForCurrentThread();
    const QImage oversized =
        renderOpacityFixture(lens, sources, 2, QTransform::fromScale(1000000, 1000000), full, 1,
                             QPainter::CompositionMode_SourceOver, false);
    const auto oversizedDiagnostics = snow_canvas_magnifier_renderer::diagnosticsForCurrentThread();
    require(oversizedDiagnostics.compositedTileCount > 0 &&
                oversizedDiagnostics.compositedPixelCount <=
                    static_cast<std::size_t>(oversized.width()) *
                        static_cast<std::size_t>(oversized.height()),
            "off-device export transforms must bound compositing work by the raster target");
    for (double opacity : {0.5, 1.0}) {
        lens.opacity = opacity;
        snow_canvas_magnifier_renderer::resetDiagnosticsForCurrentThread();
        renderOpacityFixture(lens, QList<SnowCanvasBaseImageSource>{sources[0]}, 2, QTransform(),
                             full, 1, QPainter::CompositionMode_SourceOver, false);
        const auto diagnostics = snow_canvas_magnifier_renderer::diagnosticsForCurrentThread();
        require(diagnostics.compositedTileCount == 0 && diagnostics.peakCompositingPixelCount == 0,
                "single-source magnifiers must retain zero scratch at every opacity");
    }
    lens.opacity = 0;
    snow_canvas_magnifier_renderer::resetDiagnosticsForCurrentThread();
    renderOpacityFixture(lens, sources, 2, QTransform(), full, 1,
                         QPainter::CompositionMode_SourceOver, false);
    const auto invisibleDiagnostics = snow_canvas_magnifier_renderer::diagnosticsForCurrentThread();
    require(invisibleDiagnostics.sourceLayerDrawCount == 0 &&
                invisibleDiagnostics.peakCompositingPixelCount == 0,
            "invisible magnifiers must not sample originals or allocate scratch");
}

void multilayerCompositingPreservesCallerStateAndEmptyClips() {
    const QImage original = sourceImage();
    QImage patch(original.size(), original.format());
    patch.fill(QColor(240, 35, 80, 120));
    const QList<SnowCanvasBaseImageSource> sources{{original, QRectF(0, 0, 200, 140), {}},
                                                   {patch, QRectF(0, 0, 200, 140), {}}};
    const QRegion full(QRect(0, 0, 200, 140));
    const QRegion emptyLensClip = QRegion(QRect(100, 40, 3, 3)) + QRect(157, 97, 3, 3);
    QPainterPath emptyLensClipPath;
    emptyLensClipPath.addRegion(emptyLensClip);
    for (const auto shape : {SNOW_DISPLAY_RECT_SHAPE_RECTANGLE, SNOW_DISPLAY_RECT_SHAPE_ELLIPSE,
                             SNOW_DISPLAY_RECT_SHAPE_DIAMOND}) {
        auto lens = magnifier(10, static_cast<std::uint8_t>(shape));
        lens.stroke.a = 0;
        lens.corner_radii = {};
        lens.rotation = 0.37;
        lens.opacity = 0.5;
        auto clippedLens = magnifier(2, static_cast<std::uint8_t>(shape));
        clippedLens.stroke.a = 0;
        clippedLens.corner_radii = {};
        clippedLens.opacity = 0.5;
        if (shape != SNOW_DISPLAY_RECT_SHAPE_RECTANGLE) {
            const QPainterPath path = opacityFixtureLensPath(clippedLens, projection());
            require(path.boundingRect().intersects(QRectF(emptyLensClip.boundingRect())) &&
                        path.intersected(emptyLensClipPath).isEmpty(),
                    "sparse corner clips must overlap lens bounds without intersecting its mask");
        }
        for (double dpr : {1.0, 1.25, 2.0}) {
            for (const QTransform& outer :
                 {QTransform(), QTransform(1.3, 0.15, 0.2, 1.1, -13.7, 7.1),
                  QTransform(1.9, 0, 0, 1.9, 5.3, 7.1)}) {
                requirePremultipliedPixelsNear(
                    renderOpacityFixture(lens, sources, dpr, outer, full, 0.4,
                                         QPainter::CompositionMode_Multiply, false),
                    renderOpacityFixture(lens, sources, dpr, outer, full, 0.4,
                                         QPainter::CompositionMode_Multiply, true),
                    "large layered lenses must retain canonical pixels and caller state across "
                    "shapes, DPR and outer transforms");
                if (shape == SNOW_DISPLAY_RECT_SHAPE_RECTANGLE)
                    continue;
                const QImage actual =
                    renderOpacityFixture(clippedLens, sources, dpr, outer, emptyLensClip, 0.4,
                                         QPainter::CompositionMode_Multiply, false);
                QImage unchanged(actual.size(), actual.format());
                unchanged.setDevicePixelRatio(dpr);
                unchanged.fill(QColor(30, 40, 70, 255));
                require(actual == unchanged,
                        "sparse clips outside the actual lens mask must leave the target and "
                        "caller painter state unchanged");
                require(actual == renderOpacityFixture(clippedLens, sources, dpr, outer,
                                                       emptyLensClip, 0.4,
                                                       QPainter::CompositionMode_Multiply, true),
                        "empty lens-mask intersections must match canonical group compositing");
            }
        }
    }
}

void partialPaintPreservesMasksWhenOnlyOneOriginalLayerIsExposed() {
    QImage original(30, 30, QImage::Format_ARGB32_Premultiplied);
    original.fill(Qt::green);
    QImage cursor(4, 4, original.format());
    cursor.fill(Qt::red);
    const QRegion fullExposure(QRect(0, 0, 200, 140));
    const QRegion partialExposure(QRect(96, 38, 14, 35));
    const QRectF comparedViewBounds = QRectF(partialExposure.boundingRect()).adjusted(1, 1, -1, -1);
    for (double sourceLeft : {21.0, 21.2}) {
        const QRectF sourceBounds(sourceLeft, 55, 30, 30);
        const QList<SnowCanvasBaseImageSource> sources{
            {original, sourceBounds, sourceBounds},
            {cursor, QRectF(42, 70, 4, 4), QRectF(42, 70, 4, 4)}};
        for (double factor : {2.0, 2.1}) {
            for (double opacity : {0.5, 1.0}) {
                auto lens = magnifier(factor);
                lens.stroke.a = 0;
                lens.corner_radii = {6, 6, 6, 6};
                lens.opacity = opacity;
                for (double dpr : {1.0, 1.25, 2.0}) {
                    for (const QTransform& outer :
                         {QTransform(), QTransform(1.7, 0, 0, 1.25, 5.3, 7.1)}) {
                        const QImage full =
                            renderOpacityFixture(lens, sources, dpr, outer, fullExposure, 1.0,
                                                 QPainter::CompositionMode_SourceOver, false);
                        snow_canvas_magnifier_renderer::resetDiagnosticsForCurrentThread();
                        const QImage partial =
                            renderOpacityFixture(lens, sources, dpr, outer, partialExposure, 1.0,
                                                 QPainter::CompositionMode_SourceOver, false);
                        const auto diagnostics =
                            snow_canvas_magnifier_renderer::diagnosticsForCurrentThread();
                        require(diagnostics.sourceLayerDrawCount == 1 &&
                                    diagnostics.peakCompositingPixelCount <= 65536u,
                                "stable multilayer masks must retain exposure culling and "
                                "bounded compositing scratch");
                        const QTransform deviceToView =
                            (outer * QTransform::fromScale(dpr, dpr)).inverted();
                        int comparedPixels = 0;
                        for (int y = 0; y < full.height(); ++y) {
                            for (int x = 0; x < full.width(); ++x) {
                                if (!comparedViewBounds.contains(
                                        deviceToView.map(QPointF(x + 0.5, y + 0.5))))
                                    continue;
                                const QRgb expected = full.pixel(x, y);
                                const QRgb actual = partial.pixel(x, y);
                                require(qAbs(qRed(actual) - qRed(expected)) <= 1 &&
                                            qAbs(qGreen(actual) - qGreen(expected)) <= 1 &&
                                            qAbs(qBlue(actual) - qBlue(expected)) <= 1 &&
                                            qAbs(qAlpha(actual) - qAlpha(expected)) <= 1,
                                        "culling a cursor outside the exposure must preserve "
                                        "the original layer's coverage and completed lens mask");
                                ++comparedPixels;
                            }
                        }
                        require(comparedPixels > 0,
                                "the exposure fixture must compare physical interior pixels");
                    }
                }
            }
        }
    }
}

void inheritedOpacityAppliesToTheCompleteMagnifierSceneItem() {
    QImage original(200, 140, QImage::Format_ARGB32_Premultiplied);
    original.fill(Qt::green);
    QImage cursor(15, 15, original.format());
    cursor.fill(Qt::yellow);
    const QList<SnowCanvasBaseImageSource> sources{{original, QRectF(0, 0, 200, 140), {}},
                                                   {cursor, QRectF(35, 70, 15, 15), {}}};
    auto lens = magnifier();
    lens.stroke = {255, 0, 0, 255};
    lens.stroke_width = 4;
    lens.corner_radii = {};
    lens.magnifier.connection_type = SNOW_MAGNIFIER_CONNECTION_LEADER_LINE;
    lens.opacity = 0.5;
    const SnowArrowPoint leader[]{{95, 70}, {35, 70}};
    lens.setArrowPoints(leader, 2);
    lens.arrow_type = SNOW_ARROW_TYPE_STRAIGHT;
    lens.arrow_ratio = 1;
    lens.arrow_stroke_style = SNOW_STROKE_STYLE_SOLID;
    const auto info = projection();
    QImage output(original.size(), original.format());
    output.fill(Qt::transparent);
    {
        QPainter painter(&output);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setOpacity(0.4);
        snow_canvas_renderer::SceneRenderRequest request;
        request.painter = &painter;
        request.displayInfo = &info;
        request.sceneItems = &lens;
        request.sceneItemCount = 1;
        request.exposedRegion = QRegion(output.rect());
        request.clearBackgroundEnabled = false;
        request.baseImageSources = &sources;
        snow_canvas_renderer::renderSceneItems(request);
    }
    QImage reference(1, 1, original.format());
    reference.fill(Qt::transparent);
    {
        QPainter painter(&reference);
        painter.setOpacity(0.4 * 0.5);
        painter.fillRect(reference.rect(), Qt::white);
    }
    const int expectedAlpha = reference.pixelColor(0, 0).alpha();
    QImage imageReference(1, 1, original.format());
    imageReference.fill(Qt::transparent);
    QImage opaque(1, 1, original.format());
    opaque.fill(Qt::white);
    {
        QPainter painter(&imageReference);
        painter.setOpacity(0.4 * 0.5);
        painter.drawImage(QPoint(), opaque);
    }
    // Qt quantizes image opacity and path opacity differently by one alpha unit.
    // Compare each against the same independent drawing primitive and opacity.
    const int expectedImageAlpha = imageReference.pixelColor(0, 0).alpha();
    for (const auto& [sample, alpha] :
         {std::pair{QPoint(60, 70), expectedAlpha}, std::pair{QPoint(130, 38), expectedAlpha},
          std::pair{QPoint(120, 80), expectedImageAlpha},
          std::pair{QPoint(140, 80), expectedImageAlpha}}) {
        require(output.pixelColor(sample).alpha() == alpha,
                "leader, border and composed source pixels must share caller times element "
                "opacity");
    }
}

void partialPaintAndProjectionRetainOriginalSampling() {
    const QImage original = sourceImage();
    const QList<SnowCanvasBaseImageSource> sources{{original, QRectF(0, 0, 200, 140), {}}};
    const auto lens = magnifier();
    const QRegion exposed(QRect(130, 70, 10, 10));
    const QImage partial = render({lens}, sources, 1, projection(), exposed);
    require(pixel(partial, 135, 75) == QColor(70, 30, 200) && pixel(partial, 125, 65).alpha() == 0,
            "partial repaint must sample the same pixels and stay within exposure");
    SceneDisplayInfo zoomed = projection();
    zoomed.camera_center_x = 130;
    zoomed.camera_zoom = 2;
    const QImage zoomedOutput = render({lens}, sources, 1.25, zoomed);
    require(pixel(zoomedOutput, 110, 80) == QColor(70, 30, 200),
            "camera zoom and DPR must transform the lens without resampling its annotation scene");
}

QImage patternedImage(const QSize& size) {
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < size.height(); ++y) {
        for (int x = 0; x < size.width(); ++x) {
            image.setPixelColor(
                x, y, QColor(x % 2 == 0 ? 0 : 255, y % 2 == 0 ? 0 : 255, x % 2 == 0 ? 255 : 0));
        }
    }
    return image;
}

bool sameSampleWithinRasterRounding(const QColor& left, const QColor& right) {
    // Qt's bilinear rasterizer advances fixed-point coordinates along each span.
    // A different clip can restart that span with one channel's rounding changed.
    return left.alpha() == right.alpha() && qAbs(left.red() - right.red()) <= 1 &&
           qAbs(left.green() - right.green()) <= 1 && qAbs(left.blue() - right.blue()) <= 1;
}

void paintCanonicalSourceReference(QPainter& painter, const SnowCanvasSceneItem& lens,
                                   const QList<SnowCanvasBaseImageSource>& sources,
                                   const SceneDisplayInfo& info) {
    QPainterPath outline;
    const QRectF local(-lens.width / 2, -lens.height / 2, lens.width, lens.height);
    if (lens.rect_shape == SNOW_DISPLAY_RECT_SHAPE_ELLIPSE) {
        outline.addEllipse(local);
    } else if (lens.rect_shape == SNOW_DISPLAY_RECT_SHAPE_DIAMOND) {
        outline.moveTo(0, local.top());
        outline.lineTo(local.right(), 0);
        outline.lineTo(0, local.bottom());
        outline.lineTo(local.left(), 0);
        outline.closeSubpath();
    } else {
        outline.addRect(local);
    }
    QTransform element;
    element.translate(lens.center_x, lens.center_y);
    element.rotateRadians(lens.rotation);
    const double zoom = info.camera_zoom;
    const QTransform view(zoom, 0, 0, zoom, info.surface_width / 2 - info.camera_center_x * zoom,
                          info.surface_height / 2 - info.camera_center_y * zoom);
    const QPainterPath viewOutline = view.map(element.map(outline));
    const QPointF destination = view.map(QPointF(lens.center_x, lens.center_y));
    const double scale = lens.magnifier.magnification_factor * zoom;
    const QTransform sourceView(scale, 0, 0, scale,
                                destination.x() - lens.magnifier.source_center_x * scale,
                                destination.y() - lens.magnifier.source_center_y * scale);
    // Independently paint one complete sampling domain with every original
    // layer: no spatial culling, tiling or production coordinate helpers.
    // Qt's downsampling precision depends on local-origin fixed-point setup
    // and SIMD/scalar tail selection at the true horizontal span endpoints.
    // Canonical-at-exposure therefore uses this domain's integer physical origin.
    if (sources.size() == 1) {
        // The established single-original path resolves coverage geometrically
        // with the lens before rasterization and needs no isolated group.
        const auto& source = sources.front();
        QPainterPath clip = viewOutline;
        if (!source.coverage.isEmpty()) {
            QPainterPath coverage;
            coverage.addRect(source.coverage.intersected(source.canvasRect));
            clip = clip.intersected(sourceView.map(coverage));
        }
        painter.save();
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.setClipPath(clip, Qt::IntersectClip);
        painter.setWorldTransform(sourceView, true);
        painter.drawImage(source.canvasRect, source.image, QRectF(source.image.rect()));
        painter.restore();
        return;
    }
    const QRectF exposed = painter.hasClipping()
                               ? painter.clipBoundingRect()
                               : QRectF(0, 0, info.surface_width, info.surface_height);
    const QRect samplePixels =
        painter.deviceTransform()
            .mapRect(viewOutline.boundingRect().intersected(exposed))
            .toAlignedRect()
            .intersected(QRect(0, 0, painter.device()->width(), painter.device()->height()));
    if (samplePixels.isEmpty())
        return;
    QImage composed(samplePixels.size(), QImage::Format_ARGB32_Premultiplied);
    composed.fill(Qt::transparent);
    QPainter layers(&composed);
    layers.setRenderHints(painter.renderHints());
    layers.setRenderHint(QPainter::SmoothPixmapTransform, true);
    layers.setWorldTransform(painter.deviceTransform() *
                             QTransform::fromTranslate(-samplePixels.x(), -samplePixels.y()));
    for (const auto& source : sources) {
        layers.save();
        if (!source.coverage.isEmpty()) {
            QPainterPath covered;
            covered.addRect(source.coverage.intersected(source.canvasRect));
            layers.setClipPath(sourceView.map(covered), Qt::IntersectClip);
        }
        layers.setWorldTransform(sourceView, true);
        layers.drawImage(source.canvasRect, source.image, QRectF(source.image.rect()));
        layers.restore();
    }
    layers.end();
    painter.save();
    painter.setClipPath(viewOutline, Qt::IntersectClip);
    painter.setWorldTransform(QTransform());
    painter.setWorldTransform(painter.deviceTransform().inverted());
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    painter.drawImage(samplePixels.topLeft(), composed);
    painter.restore();
}

QImage canonicalSourceReference(const SnowCanvasSceneItem& lens,
                                const QList<SnowCanvasBaseImageSource>& sources, qreal dpr,
                                const SceneDisplayInfo& info, const QRegion& exposed) {
    QImage output(QSize(qCeil(info.surface_width * dpr), qCeil(info.surface_height * dpr)),
                  QImage::Format_ARGB32_Premultiplied);
    output.setDevicePixelRatio(dpr);
    output.fill(Qt::transparent);
    QPainter painter(&output);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setClipRegion(exposed);
    paintCanonicalSourceReference(painter, lens, sources, info);
    return output;
}

void patternedMultilayerSamplingPreservesSpanEndpointsAcrossCompositingStripes() {
    // A dense original begins partway across the exposed device domain. With
    // fractional source phase, Qt's low-precision SIMD batches and scalar tails
    // differ visibly if an internal allocation edge splits that horizontal span.
    const QImage dense = patternedImage(QSize(1600, 1600));
    QImage translucent(200, 140, dense.format());
    translucent.fill(QColor(30, 40, 70, 80));
    const QList<SnowCanvasBaseImageSource> denseSources{{dense, QRectF(0, 0, 200, 200), {}},
                                                        {translucent, QRectF(0, 0, 200, 140), {}}};
    auto downsampled = magnifier(1);
    downsampled.width = downsampled.height = 600;
    downsampled.magnifier.source_width = downsampled.magnifier.source_height = 600;
    downsampled.magnifier.source_center_x = 35.23;
    downsampled.magnifier.source_center_y = 70;
    downsampled.stroke.a = 0;
    const QRegion full(QRect(0, 0, 200, 140));
    snow_canvas_magnifier_renderer::resetDiagnosticsForCurrentThread();
    const QImage actual = render({downsampled}, denseSources, 2);
    const auto diagnostics = snow_canvas_magnifier_renderer::diagnosticsForCurrentThread();
    require(diagnostics.compositedTileCount > 1 && diagnostics.peakCompositingPixelCount <= 65536u,
            "dense originals must composite in multiple bounded stripes");
    const QImage expected =
        canonicalSourceReference(downsampled, denseSources, 2, projection(), full);
    for (int y = 0; y < actual.height(); ++y) {
        for (int x = 0; x < actual.width(); ++x) {
            // Restarting Qt's fixed-point setup at a stripe's integer Y origin
            // can round RGB by one. Alpha is exact; artificial horizontal tails
            // previously changed RGB by more than this existing raster tolerance.
            require(
                sameSampleWithinRasterRounding(actual.pixelColor(x, y), expected.pixelColor(x, y)),
                "internal compositing boundaries and final span tails must retain "
                "canonical samples without changing coverage or horizontal precision");
        }
    }

    const QImage original = patternedImage(QSize(200, 140));
    const QList<SnowCanvasBaseImageSource> enlargedSources{
        {original, QRectF(0, 0, 200, 140), {}},
        {translucent, QRectF(0, 0, 200, 140), QRectF(22, 55, 25, 30)}};
    auto enlarged = magnifier(8, SNOW_DISPLAY_RECT_SHAPE_ELLIPSE);
    enlarged.rotation = 0.37;
    enlarged.stroke.a = 0;
    enlarged.opacity = 0.5;
    snow_canvas_magnifier_renderer::resetDiagnosticsForCurrentThread();
    const QImage enlargedActual =
        renderOpacityFixture(enlarged, enlargedSources, 2, QTransform(), full, 0.4,
                             QPainter::CompositionMode_SourceOver, false);
    const auto enlargedDiagnostics = snow_canvas_magnifier_renderer::diagnosticsForCurrentThread();
    require(enlargedDiagnostics.compositedTileCount > 1 &&
                enlargedDiagnostics.peakCompositingPixelCount <= 65536u,
            "textured enlarged lenses must exercise internal bounded compositing stripes");
    require(enlargedActual == renderOpacityFixture(enlarged, enlargedSources, 2, QTransform(), full,
                                                   0.4, QPainter::CompositionMode_SourceOver, true),
            "integral enlargement must preserve original texture, limited layer coverage "
            "and the completed lens mask across internal stripes");
}

void integerProjectionPartialPaintKeepsExactSamples() {
    const QImage original = patternedImage(QSize(200, 140));
    const QList<SnowCanvasBaseImageSource> sources{{original, QRectF(0, 0, 200, 140), {}}};
    for (double factor : {8.0, 10.0}) {
        auto lens = magnifier(factor);
        lens.stroke.a = 0;
        const int left = factor == 8.0 ? 122 : 120;
        const QImage full = render({lens}, sources);
        const QImage partial =
            render({lens}, sources, 1.0, projection(), QRegion(QRect(left, 62, 13, 17)));
        for (int y = 63; y < 78; ++y) {
            for (int x = left + 1; x < left + 12; ++x) {
                require(factor == 8.0 ? partial.pixel(x, y) == full.pixel(x, y)
                                      : sameSampleWithinRasterRounding(partial.pixelColor(x, y),
                                                                       full.pixelColor(x, y)),
                        "a partial lens paint must preserve its interpolation neighbors, "
                        "exactly at fixed-point scales");
            }
        }
    }
}

void partialPaintKeepsCanonicalSamplingAtEverySourceDensity() {
    for (const auto sourceSize : {QSize(100, 70), QSize(200, 140), QSize(400, 280)}) {
        const QImage original = patternedImage(sourceSize);
        QImage translucent(11, 8, original.format());
        translucent.fill(QColor(60, 20, 170, 90));
        for (const QRectF coverage : {QRectF(), QRectF(34.2, 60.3, 3.6, 20.4)}) {
            for (const auto& sources :
                 {QList<SnowCanvasBaseImageSource>{{original, QRectF(0, 0, 200, 140), coverage}},
                  QList<SnowCanvasBaseImageSource>{{original, QRectF(0, 0, 200, 140), coverage},
                                                   {translucent, QRectF(0, 0, 200, 140), {}}}}) {
                for (const auto shape :
                     {SNOW_DISPLAY_RECT_SHAPE_RECTANGLE, SNOW_DISPLAY_RECT_SHAPE_ELLIPSE,
                      SNOW_DISPLAY_RECT_SHAPE_DIAMOND}) {
                    for (double factor : {1.0, 2.0, 3.7, 10.0}) {
                        auto lens = magnifier(factor, static_cast<std::uint8_t>(shape));
                        lens.stroke.a = 0;
                        lens.rotation = 0.35;
                        lens.magnifier.source_center_x = 35.23;
                        lens.magnifier.source_center_y = 70.47;
                        for (double zoom : {0.5, 1.0, 2.0}) {
                            auto info = projection();
                            info.camera_zoom = zoom;
                            const auto view = snow_canvas_render_geometry::sceneProjection(info);
                            const QPointF center = snow_canvas_render_geometry::canvasToView(
                                view, lens.center_x, lens.center_y);
                            const QRegion exposed(
                                QRect(qFloor(center.x()) - 10, qFloor(center.y()) - 8, 13, 17));
                            for (double dpr : {1.0, 1.25, 2.0}) {
                                const QImage full = render({lens}, sources, dpr, info);
                                const QImage partial = render({lens}, sources, dpr, info, exposed);
                                // Qt's downsampling span helpers can use different interpolation
                                // precision after clipping. Compare with direct canonical image
                                // painting at this exposure instead of demanding identical spans.
                                const bool downsampling =
                                    factor * zoom * dpr < sourceSize.width() / 200.0;
                                const double pixelScale =
                                    factor * zoom * dpr * 200.0 / sourceSize.width();
                                const double fixedStep = 65536.0 / pixelScale;
                                const bool exactStep = fixedStep == std::floor(fixedStep);
                                const QImage expected =
                                    downsampling ? canonicalSourceReference(lens, sources, dpr,
                                                                            info, exposed)
                                                 : full;
                                const QRect interior =
                                    exposed.boundingRect().adjusted(1, 1, -1, -1);
                                for (int y = qCeil(interior.top() * dpr);
                                     y < qFloor((interior.y() + interior.height()) * dpr); ++y) {
                                    for (int x = qCeil(interior.left() * dpr);
                                         x < qFloor((interior.x() + interior.width()) * dpr); ++x) {
                                        const QColor fullPixel = expected.pixelColor(x, y);
                                        const QColor partialPixel = partial.pixelColor(x, y);
                                        // Partial source-stack stripes restart Qt's fixed-point
                                        // spans at a different integer origin. Fractional steps
                                        // can round RGB by one; alpha and integral steps stay
                                        // exact.
                                        const bool matches =
                                            downsampling && exactStep
                                                ? partial.pixel(x, y) == expected.pixel(x, y)
                                                : sameSampleWithinRasterRounding(partialPixel,
                                                                                 fullPixel);
                                        if (!matches) {
                                            std::cerr
                                                << "sampling mismatch: source "
                                                << sourceSize.width() << 'x' << sourceSize.height()
                                                << ", coverage " << !coverage.isEmpty()
                                                << ", shape " << shape << ", factor " << factor
                                                << ", zoom " << zoom << ", DPR " << dpr
                                                << ", physical pixel " << x << ',' << y << ", full "
                                                << fullPixel.name().toStdString() << ", partial "
                                                << partialPixel.name().toStdString() << '\n';
                                        }
                                        require(matches,
                                                "partial exposure must preserve canonical sampling "
                                                "at every factor, shape, density, zoom and DPR");
                                    }
                                }
                                require(original.cacheKey() == sources.front().image.cacheKey(),
                                        "partial sampling must retain shared original pixels");
                            }
                        }
                    }
                }
            }
        }
    }
}

QImage renderInPhysicalTiles(const SnowCanvasSceneItem& lens,
                             const QList<SnowCanvasBaseImageSource>& sources, qreal dpr,
                             const SceneDisplayInfo& info, bool reference = false) {
    QImage output(QSize(qCeil(info.surface_width * dpr), qCeil(info.surface_height * dpr)),
                  QImage::Format_ARGB32_Premultiplied);
    output.setDevicePixelRatio(dpr);
    output.fill(Qt::transparent);
    for (int top = 0; top < output.height(); top += 23) {
        for (int left = 0; left < output.width(); left += 31) {
            const QRect physicalRect(left, top, qMin(31, output.width() - left),
                                     qMin(23, output.height() - top));
            QImage tile(physicalRect.size(), output.format());
            tile.setDevicePixelRatio(dpr);
            tile.fill(Qt::transparent);
            QPainter painter(&tile);
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.translate(-left / dpr, -top / dpr);
            const QRegion exposed(
                QRectF(left / dpr, top / dpr, tile.width() / dpr, tile.height() / dpr)
                    .toAlignedRect());
            painter.setClipRegion(exposed);
            snow_canvas_renderer::SceneRenderRequest request;
            request.painter = &painter;
            request.displayInfo = &info;
            request.sceneItems = &lens;
            request.sceneItemCount = 1;
            request.exposedRegion = exposed;
            request.clearBackgroundEnabled = false;
            request.baseImageSources = &sources;
            if (reference)
                paintCanonicalSourceReference(painter, lens, sources, info);
            else
                snow_canvas_renderer::renderSceneItems(request);
            painter.end();
            for (int y = 0; y < tile.height(); ++y) {
                std::memcpy(output.scanLine(top + y) + left * static_cast<int>(sizeof(QRgb)),
                            tile.constScanLine(y),
                            static_cast<std::size_t>(tile.width()) * sizeof(QRgb));
            }
        }
    }
    return output;
}

void tiledPaintKeepsCanonicalSamplingAtCoverageBoundaries() {
    const QImage original = patternedImage(QSize(400, 280));
    QImage translucent(11, 8, original.format());
    translucent.fill(QColor(60, 20, 170, 90));
    for (const QRectF coverage : {QRectF(), QRectF(34.2, 60.3, 3.6, 20.4)}) {
        for (const auto& sources :
             {QList<SnowCanvasBaseImageSource>{{original, QRectF(0, 0, 200, 140), coverage}},
              QList<SnowCanvasBaseImageSource>{{original, QRectF(0, 0, 200, 140), coverage},
                                               {translucent, QRectF(0, 0, 200, 140), {}}}}) {
            for (double factor : {1.0, 3.7, 8.0, 10.0}) {
                auto lens = magnifier(factor);
                lens.stroke.a = 0;
                lens.magnifier.source_center_x = 35.23;
                lens.magnifier.source_center_y = 70.47;
                for (double zoom : {0.5, 1.0, 2.0}) {
                    auto info = projection();
                    info.camera_zoom = zoom;
                    for (double dpr : {1.0, 1.25, 2.0}) {
                        const QImage tiled = renderInPhysicalTiles(lens, sources, dpr, info);
                        const double pixelScale = factor * zoom * dpr / 2.0;
                        const double fixedStep = 65536.0 / pixelScale;
                        const bool compareFull =
                            pixelScale >= 1.0 && fixedStep == std::floor(fixedStep);
                        // Fractional steps accumulate Qt's fixed-point rounding differently
                        // from a new tile origin. Use direct canonical painting in each tile
                        // for those cases; exact steps also match the untiled image exactly.
                        const QImage full =
                            compareFull ? render({lens}, sources, dpr, info)
                                        : renderInPhysicalTiles(lens, sources, dpr, info, true);
                        for (int y = 0; y < full.height(); ++y) {
                            for (int x = 0; x < full.width(); ++x) {
                                // The isolated stack can start inside the physical tile's
                                // lens bounds. Fractional spans may round RGB by one;
                                // integral steps and alpha/coverage remain exact.
                                const bool matches =
                                    compareFull
                                        ? tiled.pixel(x, y) == full.pixel(x, y)
                                        : sameSampleWithinRasterRounding(tiled.pixelColor(x, y),
                                                                         full.pixelColor(x, y));
                                if (!matches) {
                                    std::cerr
                                        << "tile mismatch: coverage " << !coverage.isEmpty()
                                        << ", factor " << factor << ", zoom " << zoom << ", DPR "
                                        << dpr << ", physical pixel " << x << ',' << y << ", full "
                                        << full.pixelColor(x, y).name().toStdString() << ", tiled "
                                        << tiled.pixelColor(x, y).name().toStdString() << ", alpha "
                                        << full.pixelColor(x, y).alpha() << '/'
                                        << tiled.pixelColor(x, y).alpha() << '\n';
                                }
                                require(matches,
                                        "tile origins and coverage clips must preserve canonical "
                                        "sampling");
                            }
                        }
                    }
                }
            }
        }
    }
}

void largeOffscreenSourcesKeepSharedPixelsAndCullUnexposedLayers() {
    QImage original(QSize(8192, 4320), QImage::Format_ARGB32_Premultiplied);
    original.fill(Qt::blue);
    {
        QPainter painter(&original);
        painter.fillRect(QRect(6992, 3692, 16, 16), Qt::green);
    }
    const QList<SnowCanvasBaseImageSource> sources{
        {original, QRectF(-100000, -20000, 8192, 4320), {}},
        {original, QRectF(100000, 20000, 8192, 4320), {}}};
    auto lens = magnifier(10.0);
    lens.magnifier.source_center_x = -93000;
    lens.magnifier.source_center_y = -16300;
    for (double zoom : {2.0, 4.0}) {
        auto info = projection();
        info.camera_zoom = zoom;
        for (double dpr : {1.0, 1.25, 2.0}) {
            snow_canvas_magnifier_renderer::resetDiagnosticsForCurrentThread();
            const QImage full = render({lens}, sources, dpr, info);
            require(pixel(full, 100, 70) == QColor(Qt::green),
                    "large source transforms must retain visible pixels at distant origins");
            const auto diagnostics = snow_canvas_magnifier_renderer::diagnosticsForCurrentThread();
            require(diagnostics.sourceLayerDrawCount == 1 && diagnostics.compositedTileCount == 0 &&
                        diagnostics.peakCompositingPixelCount == 0,
                    "a lens sampling one original must cull remote layers without scratch");
            const QRegion exposed(QRect(95, 65, 13, 17));
            const QImage partial = render({lens}, sources, dpr, info, exposed);
            require(pixel(partial, 100, 70) == pixel(full, 100, 70),
                    "partial paints must sample large offscreen originals without cropping");
            require(original.cacheKey() == sources.front().image.cacheKey(),
                    "large source paints must retain immutable shared image storage");
        }
    }
}

void leaderRenderingUsesEveryArrowheadAndSceneBounds() {
    auto lens = magnifier();
    lens.magnifier.connection_type = SNOW_MAGNIFIER_CONNECTION_LEADER_LINE;
    const SnowArrowPoint points[]{{99, 70}, {35, 70}};
    lens.setArrowPoints(points, 2);
    lens.arrow_type = SNOW_ARROW_TYPE_STRAIGHT;
    lens.arrow_ratio = 1;
    lens.arrow_stroke_style = SNOW_STROKE_STYLE_SOLID;
    for (const auto head :
         {SNOW_ARROWHEAD_NONE, SNOW_ARROWHEAD_ARROW, SNOW_ARROWHEAD_BAR, SNOW_ARROWHEAD_DOT,
          SNOW_ARROWHEAD_CIRCLE, SNOW_ARROWHEAD_CIRCLE_OUTLINE, SNOW_ARROWHEAD_TRIANGLE,
          SNOW_ARROWHEAD_TRIANGLE_OUTLINE, SNOW_ARROWHEAD_DIAMOND, SNOW_ARROWHEAD_DIAMOND_OUTLINE,
          SNOW_ARROWHEAD_CROWFOOT_ONE, SNOW_ARROWHEAD_CROWFOOT_MANY,
          SNOW_ARROWHEAD_CROWFOOT_ONE_OR_MANY, SNOW_ARROWHEAD_INDENTED_TRIANGLE}) {
        lens.arrow_end_head = head;
        const QImage output = render({lens}, {});
        require(pixel(output, 70, 70).alpha() != 0,
                "each arrowhead must keep one straight magnifier leader");
    }
    const QImage croppedLeader =
        render({lens}, {}, 1, projection(), QRegion(QRect(65, 65, 10, 10)));
    require(pixel(croppedLeader, 70, 70).alpha() != 0,
            "magnifier culling bounds must include the leader outside its lens");
}

void samplingStaysPristineDuringFilterReplay() {
    auto annotation = magnifier();
    annotation.kind = SNOW_SCENE_DISPLAY_ITEM_DRAW_RECT;
    annotation.center_x = 35;
    annotation.width = 30;
    annotation.height = 30;
    annotation.fill = {255, 0, 0, 255};
    annotation.stroke.a = 0;
    auto filter = annotation;
    filter.kind = SNOW_SCENE_DISPLAY_ITEM_FILTER;
    filter.filter = snow_filter_render_spec_resolve(2, 1);
    const auto lens = magnifier();
    const QList<SnowCanvasBaseImageSource> sources{{sourceImage(), QRectF(0, 0, 200, 140), {}}};
    const QImage before = render({annotation, filter, lens}, sources);
    require(pixel(before, 140, 80) == QColor(70, 30, 200),
            "a magnifier replayed after filters must still sample pristine image layers");
    filter.center_x = 130;
    filter.width = 60;
    filter.height = 60;
    const QImage after = render({lens, filter}, sources);
    const QColor filtered = pixel(after, 140, 80);
    require(filtered.alpha() == 255 && filtered.red() == filtered.green() &&
                filtered.green() == filtered.blue(),
            "a later filter must process magnifier pixels in document paint order");
}

class OriginalImageRenderer final : public SnowCanvasCustomRenderer {
  public:
    QList<SnowCanvasBaseImageSource> baseImageSources() const override {
        return {{image, QRectF(0, 0, 200, 140), {}}};
    }
    void renderBeforeCanvas(QPainter& painter, const SnowCanvasRenderContext& context) override {
        painter.drawImage(context.canvasToViewTransform.mapRect(QRectF(0, 0, 200, 140)), image);
    }
    QImage image = sourceImage();
};

void liveWidgetRetainsOriginalSourcesWithoutFilters() {
    SnowCanvasWidget canvas;
    canvas.resize(200, 140);
    OriginalImageRenderer renderer;
    canvas.setCustomRenderer(&renderer);
    canvas.show();
    QApplication::processEvents();
    require(canvas.setViewportCamera(100, 70, 1) && canvas.setCanvasTool(SnowCanvasTool::Magnifier),
            "configure live magnifier widget");
    for (const auto& [type, position, button, buttons] :
         {std::tuple{QEvent::MouseButtonPress, QPointF(20, 55), Qt::LeftButton,
                     Qt::MouseButtons(Qt::LeftButton)},
          std::tuple{QEvent::MouseMove, QPointF(50, 85), Qt::NoButton,
                     Qt::MouseButtons(Qt::LeftButton)},
          std::tuple{QEvent::MouseButtonRelease, QPointF(50, 85), Qt::LeftButton,
                     Qt::MouseButtons(Qt::NoButton)}}) {
        QMouseEvent event(type, position, position, position, button, buttons, Qt::NoModifier);
        QApplication::sendEvent(&canvas, &event);
    }
    QImage output(200, 140, QImage::Format_ARGB32_Premultiplied);
    output.fill(Qt::transparent);
    QPainter painter(&output);
    canvas.render(&painter);
    painter.end();
    require(output.pixelColor(15, 50) == QColor(20, 190, 80),
            "the widget no-filter fast path must retain original image access for magnifiers");
    canvas.setCustomRenderer(nullptr);
}

void pointer(SnowRuntime runtime, SnowViewport viewport, SnowPointerEventType type,
             const QPointF& position, bool primaryDown = true) {
    SnowInputEvent event{};
    event.kind = SNOW_INPUT_EVENT_POINTER;
    event.pointer.pointer_id = 1;
    event.pointer.event_type = type;
    event.pointer.device = SNOW_POINTER_DEVICE_MOUSE;
    event.pointer.position_x = position.x();
    event.pointer.position_y = position.y();
    event.pointer.button =
        type == SNOW_POINTER_EVENT_MOVE ? SNOW_POINTER_BUTTON_NONE : SNOW_POINTER_BUTTON_PRIMARY;
    event.pointer.buttons = type == SNOW_POINTER_EVENT_UP || !primaryDown ? 0 : 1;
    SnowInteractionOutput output{};
    ScopedChangedViewportList changed;
    require(snow_viewport_process_input_ex(runtime, viewport, &event, &output,
                                           changed.outParam()) == SNOW_OK,
            "magnifier export fixture input failed");
}

QImage renderGuide(const SnowOverlayDisplayItem& item, const OverlayDisplayInfo& info, qreal dpr,
                   bool cached, const QRegion& exposed = QRegion(QRect(0, 0, 200, 140))) {
    QImage output(QSize(qCeil(info.surface_width * dpr), qCeil(info.surface_height * dpr)),
                  QImage::Format_ARGB32_Premultiplied);
    output.setDevicePixelRatio(dpr);
    output.fill(Qt::transparent);
    QPainter painter(&output);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setClipRegion(exposed);
    if (cached) {
        const SnowCanvasOverlayItem owned(item);
        snow_canvas_renderer::renderOverlayItems(painter, info, &owned, 1, exposed);
    } else {
        snow_canvas_renderer::renderOverlayItems(painter, info, &item, 1, exposed);
    }
    return output;
}

double alphaWeight(const QImage& image) {
    double weight = 0;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            weight += static_cast<double>(image.pixelColor(x, y).alphaF());
    return weight;
}

void magnifierMoveHandleMatchesAngleControls(const SnowOverlayDisplayItem& handle,
                                             const OverlayDisplayInfo& info) {
    auto angleHandle = handle;
    angleHandle.rect_kind = SNOW_OVERLAY_RECT_ARROW_ENDPOINT_HANDLE;
    const auto view = snow_canvas_render_geometry::overlayProjection(info);
    const QPointF center =
        snow_canvas_render_geometry::canvasToView(view, handle.center_x, handle.center_y);
    require(std::abs(handle.width * view.cameraZoom - 14.0) < 0.001,
            "magnifier move controls must keep a constant screen size");
    for (double dpr : {1.0, 1.25, 2.0}) {
        const QImage reference = renderGuide(angleHandle, info, dpr, false);
        const QImage direct = renderGuide(handle, info, dpr, false);
        require(direct == renderGuide(handle, info, dpr, true),
                "cached and direct move controls must render identically");
        for (int y = 0; y < direct.height(); ++y) {
            for (int x = 0; x < direct.width(); ++x) {
                const QPointF offset((x + 0.5) / dpr - center.x(), (y + 0.5) / dpr - center.y());
                if (std::abs(offset.x()) > 4.5 || std::abs(offset.y()) > 4.5)
                    require(direct.pixelColor(x, y) == reference.pixelColor(x, y),
                            "move controls must match the angle controls' white fill and blue rim");
            }
        }
        require(pixel(direct, center.x(), center.y()).red() > 220,
                "the move indicator must leave clear space at its center");
        for (const double x : {-1.5, 1.5}) {
            for (const double y : {-2.5, 0.0, 2.5}) {
                const QColor ink = pixel(direct, center.x() + x, center.y() + y);
                require(ink.blue() > ink.red() + 30,
                        "all six grip dots must remain visible at every pixel density");
            }
        }
    }
    require(snow_canvas_widget_selection_hit_testing::selectionInteractionAtItems(&handle, 1, view,
                                                                                  center) ==
                snow_canvas_widget_selection_hit_testing::SelectionInteractionTarget::Handle,
            "the restyled move control must remain interactive at its center");
}

void magnifierHoverAndSelectionReuseCreationGuidesAtEveryShapeZoomAndDpr() {
    for (const auto shape : {SNOW_RECTANGLE_SHAPE_RECTANGLE, SNOW_RECTANGLE_SHAPE_ELLIPSE,
                             SNOW_RECTANGLE_SHAPE_DIAMOND}) {
        for (double zoom : {0.5, 1.0, 2.0}) {
            SnowStyleDefaults defaults{};
            require(snow_runtime_style_defaults_default(&defaults) == SNOW_OK, "load defaults");
            defaults.magnifier.shape = shape;
            defaults.magnifier.factor = 2;
            ScopedRuntimeHandle runtime;
            SnowRuntimeConfig config{&defaults};
            require(snow_runtime_create_with_config(&config, runtime.outParam()) == SNOW_OK,
                    "create guide runtime");
            SnowCanvasViewport viewport;
            require(viewport.create(runtime.get(), snow_canvas_viewport::defaultEngineConfig()) &&
                        snow_viewport_set_surface_size(runtime.get(), viewport.get(), 200, 140) ==
                            SNOW_OK &&
                        snow_viewport_set_camera(runtime.get(), viewport.get(), 100, 70, zoom) ==
                            SNOW_OK,
                    "configure guide viewport");
            ScopedChangedViewportList changed;
            require(snow_viewport_set_active_tool_ex(runtime.get(), viewport.get(),
                                                     SNOW_ACTIVE_TOOL_MAGNIFIER,
                                                     changed.outParam()) == SNOW_OK,
                    "activate guide fixture magnifier");
            pointer(runtime.get(), viewport.get(), SNOW_POINTER_EVENT_DOWN, {70, 55});
            pointer(runtime.get(), viewport.get(), SNOW_POINTER_EVENT_MOVE, {130, 85});
            SnowCanvasDisplayCache cache;
            require(cache.sync(runtime.get(), viewport.get()) && cache.sceneItemCount() == 1 &&
                        cache.overlayItemCount() == 1,
                    "creation must publish one lens and one source guide");
            const SnowCanvasOverlayItem creationGuide(cache.overlayItems()[0]);
            require(creationGuide.kind == SNOW_OVERLAY_DISPLAY_ITEM_FOCUS_CONNECTION &&
                        creationGuide.arrow_stroke_style == SNOW_STROKE_STYLE_DASHED &&
                        std::abs(creationGuide.stroke_width * zoom - 1) < 0.001,
                    "source guide must use a constant screen-width dashed contour");
            for (double dpr : {1.0, 1.25, 2.0}) {
                const QImage dashed = renderGuide(creationGuide, cache.overlayInfo(), dpr, true);
                auto solidGuide = static_cast<const SnowOverlayDisplayItem&>(creationGuide);
                solidGuide.arrow_stroke_style = SNOW_STROKE_STYLE_SOLID;
                const double solidWeight =
                    alphaWeight(renderGuide(solidGuide, cache.overlayInfo(), dpr, true));
                require(alphaWeight(dashed) > solidWeight * 0.35 &&
                            alphaWeight(dashed) < solidWeight * 0.9,
                        "source contours must have visible dashes and gaps at every shape and DPR");
            }
            pointer(runtime.get(), viewport.get(), SNOW_POINTER_EVENT_UP, {130, 85});
            require(cache.sync(runtime.get(), viewport.get()), "sync committed magnifier");
            const auto id = cache.sceneItems()[0].element_id;
            require(snow_viewport_set_active_tool_ex(runtime.get(), viewport.get(),
                                                     SNOW_ACTIVE_TOOL_SELECT,
                                                     changed.outParam()) == SNOW_OK &&
                        snow_viewport_reset_editing_state_ex(runtime.get(), viewport.get(),
                                                             changed.outParam()) == SNOW_OK,
                    "prepare magnifier hover fixture");
            for (const auto position : {QPointF(100, 70), QPointF(145, 70)}) {
                pointer(runtime.get(), viewport.get(), SNOW_POINTER_EVENT_MOVE, position, false);
                require(cache.sync(runtime.get(), viewport.get()) && cache.overlayItemCount() == 1,
                        "hovering source or lens must publish only the dashed source contour");
                const SnowCanvasOverlayItem hoverGuide(cache.overlayItems()[0]);
                require(hoverGuide.kind == SNOW_OVERLAY_DISPLAY_ITEM_FOCUS_CONNECTION &&
                            hoverGuide.arrow_stroke_style == SNOW_STROKE_STYLE_DASHED,
                        "magnifier hover must use the creation contour instead of a rectangle");
                for (double dpr : {1.0, 1.25, 2.0})
                    for (bool cached : {false, true})
                        require(renderGuide(hoverGuide, cache.overlayInfo(), dpr, cached) ==
                                    renderGuide(creationGuide, cache.overlayInfo(), dpr, cached),
                                "hover and creation must render the same dashed source shape");
            }
            pointer(runtime.get(), viewport.get(), SNOW_POINTER_EVENT_MOVE, {10, 10}, false);
            require(cache.sync(runtime.get(), viewport.get()) && cache.overlayItemCount() == 0,
                    "leaving a magnifier must remove its hover contour");
            require(snow_viewport_select_element_ex(runtime.get(), viewport.get(), id,
                                                    changed.outParam()) == SNOW_OK &&
                        cache.sync(runtime.get(), viewport.get()),
                    "select guide fixture magnifier");
            const SnowCanvasOverlayItem* sourceGuide = nullptr;
            const SnowCanvasOverlayItem* frame = nullptr;
            const SnowCanvasOverlayItem* moveHandle = nullptr;
            for (std::uint32_t i = 0; i < cache.overlayItemCount(); ++i) {
                const auto& item = cache.overlayItems()[i];
                if (item.kind == SNOW_OVERLAY_DISPLAY_ITEM_FOCUS_CONNECTION)
                    sourceGuide = &item;
                if (item.kind == SNOW_OVERLAY_DISPLAY_ITEM_DRAW_RECT &&
                    item.rect_kind == SNOW_OVERLAY_RECT_SELECTION_FRAME)
                    frame = &item;
                if (item.kind == SNOW_OVERLAY_DISPLAY_ITEM_DRAW_RECT &&
                    item.rect_kind == SNOW_OVERLAY_RECT_MAGNIFIER_MOVE_HANDLE)
                    moveHandle = &item;
            }
            require(sourceGuide != nullptr && frame != nullptr,
                    "selection must publish the dashed source contour and standard shape frame");
            require(moveHandle != nullptr, "selection must publish a magnifier move control");
            magnifierMoveHandleMatchesAngleControls(*moveHandle, cache.overlayInfo());
            for (double rotation : {0.0, 0.4}) {
                auto rotatedFrame = static_cast<const SnowOverlayDisplayItem&>(*frame);
                rotatedFrame.rotation = rotation;
                auto standardDash = rotatedFrame;
                standardDash.rect_kind = SNOW_OVERLAY_RECT_SELECTION_MULTI_FRAME;
                auto solidFrame = rotatedFrame;
                solidFrame.rect_kind = SNOW_OVERLAY_RECT_SELECTION_FRAME;
                for (double dpr : {1.0, 1.25, 2.0}) {
                    for (bool cached : {false, true}) {
                        const QImage selected =
                            renderGuide(rotatedFrame, cache.overlayInfo(), dpr, cached);
                        require(selected ==
                                    renderGuide(solidFrame, cache.overlayInfo(), dpr, cached),
                                "magnifier frames must match solid shape selection borders");
                        require(alphaWeight(renderGuide(standardDash, cache.overlayInfo(), dpr,
                                                        cached)) < alphaWeight(selected) * 0.85,
                                "magnifier selection borders must remain solid at every DPR");
                        require(renderGuide(*sourceGuide, cache.overlayInfo(), dpr, cached) ==
                                    renderGuide(creationGuide, cache.overlayInfo(), dpr, cached),
                                "creation and selection must render the same source shape");
                        const QRegion exposed(QRect(80, 45, 40, 40));
                        const QImage partial =
                            renderGuide(rotatedFrame, cache.overlayInfo(), dpr, cached, exposed);
                        require(alphaWeight(partial) > 0 &&
                                    alphaWeight(partial) < alphaWeight(selected) &&
                                    pixel(partial, 65, 50).alpha() == 0,
                                "partial frame repaints must respect the exposed region");
                        for (int y = 46; y < 84; ++y)
                            for (int x = 81; x < 119; ++x)
                                require(pixel(partial, x, y) == pixel(selected, x, y),
                                        "exposed guide pixels must match a complete repaint");
                    }
                }
                const auto projection =
                    snow_canvas_render_geometry::overlayProjection(cache.overlayInfo());
                const QPointF center = snow_canvas_render_geometry::canvasToView(
                    projection, rotatedFrame.center_x, rotatedFrame.center_y);
                const double halfHeight = rotatedFrame.height * zoom / 2;
                const QPointF edge = center + QPointF(std::sin(rotation) * halfHeight,
                                                      -std::cos(rotation) * halfHeight);
                require(snow_canvas_widget_selection_hit_testing::selectionInteractionAtItems(
                            &rotatedFrame, 1, projection, edge) ==
                            snow_canvas_widget_selection_hit_testing::SelectionInteractionTarget::
                                Handle,
                        "magnifier selection frames must retain their complete interactive border");
            }
            require(snow_viewport_reset_editing_state_ex(runtime.get(), viewport.get(),
                                                         changed.outParam()) == SNOW_OK &&
                        cache.sync(runtime.get(), viewport.get()) && cache.overlayItemCount() == 0,
                    "deselecting must remove magnifier guides from the overlay");
        }
    }
}

void exportUsesDirectOriginalSourcesIncludingOutsideCrop() {
    SnowStyleDefaults defaults{};
    require(snow_runtime_style_defaults_default(&defaults) == SNOW_OK, "load defaults");
    defaults.magnifier.factor = 2;
    defaults.magnifier.stroke_width = 2;
    defaults.magnifier.connection_type = SNOW_MAGNIFIER_CONNECTION_LEADER_LINE;
    defaults.magnifier.source_outline = 0;
    ScopedRuntimeHandle runtime;
    SnowRuntimeConfig config{&defaults};
    require(snow_runtime_create_with_config(&config, runtime.outParam()) == SNOW_OK,
            "create magnifier runtime");
    SnowCanvasViewport viewport;
    require(viewport.create(runtime.get(), snow_canvas_viewport::defaultEngineConfig()) &&
                snow_viewport_set_surface_size(runtime.get(), viewport.get(), 200, 140) ==
                    SNOW_OK &&
                snow_viewport_set_camera(runtime.get(), viewport.get(), 100, 70, 1) == SNOW_OK,
            "configure magnifier viewport");
    ScopedChangedViewportList changed;
    require(snow_viewport_set_active_tool_ex(runtime.get(), viewport.get(),
                                             SNOW_ACTIVE_TOOL_MAGNIFIER,
                                             changed.outParam()) == SNOW_OK,
            "activate magnifier");
    pointer(runtime.get(), viewport.get(), SNOW_POINTER_EVENT_DOWN, {20, 55});
    pointer(runtime.get(), viewport.get(), SNOW_POINTER_EVENT_MOVE, {50, 85});
    pointer(runtime.get(), viewport.get(), SNOW_POINTER_EVENT_UP, {50, 85});
    SnowCanvasDisplayCache cache;
    require(cache.sync(runtime.get(), viewport.get()) && cache.sceneItemCount() == 1,
            "creation must publish one magnifier scene item");
    const auto initial = cache.sceneItems()[0];
    require(initial.arrow_point_count == 0,
            "a concentric lens must omit its leader while containing the source center");
    pointer(runtime.get(), viewport.get(), SNOW_POINTER_EVENT_DOWN,
            {initial.center_x, initial.center_y});
    pointer(runtime.get(), viewport.get(), SNOW_POINTER_EVENT_UP,
            {initial.center_x, initial.center_y});
    const QPointF grip(initial.center_x, initial.center_y + initial.height / 2 + 24);
    pointer(runtime.get(), viewport.get(), SNOW_POINTER_EVENT_DOWN, grip);
    pointer(runtime.get(), viewport.get(), SNOW_POINTER_EVENT_MOVE, grip + QPointF(95, 0));
    pointer(runtime.get(), viewport.get(), SNOW_POINTER_EVENT_UP, grip + QPointF(95, 0));
    require(cache.sync(runtime.get(), viewport.get()), "sync independent lens drag");
    const auto moved = cache.sceneItems()[0];
    require(std::abs(moved.center_x - initial.center_x - 95) < 0.001 &&
                moved.magnifier.source_center_x == initial.magnifier.source_center_x,
            "destination grip must move only the magnified region");
    require(moved.arrow_point_count == 2,
            "a separated magnifier must publish one straight leader toward its source center");
    for (const double dpr : {1.0, 1.25, 2.0}) {
        const QImage output = render({moved}, {}, dpr);
        require(pixel(output, 95, 70).alpha() == 0 && pixel(output, 85, 70).alpha() != 0 &&
                    pixel(output, 100, 70).alpha() != 0,
                "magnifier leaders must leave a visible gap before the lens border at every DPR");
    }
    const QImage original = sourceImage();
    const QList<CanvasExportSource> sources{{original, QRectF(0, 0, 200, 140)}};
    snow_canvas_export::resetDiagnosticsForCurrentThread();
    const QImage exported = snow_canvas_export::renderToImage(
        runtime.get(), QRectF(100, 40, 60, 60), QSize(120, 120), sources);
    require(exported.pixelColor(70, 70) == QColor(70, 30, 200),
            "a cropped export must retain original sources outside its output crop");
    const auto diagnostics = snow_canvas_export::diagnosticsForCurrentThread();
    require(diagnostics.directSourceFastPathCount == 1 && diagnostics.fullCompositorPathCount == 0,
            "magnifier-only exports must avoid full background compositor allocations");
    defaults.magnifier.leader_arrowhead = SNOW_ARROWHEAD_TRIANGLE;
    for (double dpr : {1.0, 1.25, 2.0}) {
        double previousWeight = 0;
        for (double ratio : {0.5, 1.0, 3.0}) {
            defaults.magnifier.arrow_ratio = ratio;
            require(snow_viewport_set_magnifier_style_patch_ex(
                        runtime.get(), viewport.get(), &defaults.magnifier,
                        SNOW_MAGNIFIER_STYLE_PROPERTY_LEADER_ARROWHEAD |
                            SNOW_MAGNIFIER_STYLE_PROPERTY_ARROW_RATIO,
                        0, changed.outParam()) == SNOW_OK &&
                        cache.sync(runtime.get(), viewport.get()) &&
                        cache.sceneItems()[0].arrow_ratio == ratio,
                    "magnifier proportions must reach the rendered leader geometry");
            const QImage endpoint =
                snow_canvas_export::renderToImage(runtime.get(), QRectF(20, 40, 45, 60),
                                                  QSize(qRound(45 * dpr), qRound(60 * dpr)), {});
            const double weight = alphaWeight(endpoint);
            require(weight > previousWeight,
                    "larger magnifier proportions must enlarge exported arrowheads at every DPR");
            previousWeight = weight;
        }
    }
    defaults.magnifier.connection_type = SNOW_MAGNIFIER_CONNECTION_NONE;
    require(snow_viewport_set_magnifier_style_patch_ex(
                runtime.get(), viewport.get(), &defaults.magnifier,
                SNOW_MAGNIFIER_STYLE_PROPERTY_CONNECTION_TYPE, 0, changed.outParam()) == SNOW_OK &&
                cache.sync(runtime.get(), viewport.get()) &&
                cache.sceneItems()[0].arrow_point_count == 0,
            "disabling the leader must remove its scene geometry");
    const QImage noBase = snow_canvas_export::renderToImage(runtime.get(), QRectF(100, 40, 60, 60),
                                                            QSize(60, 60), {});
    require(noBase.pixelColor(30, 30).alpha() == 0,
            "export without base sources must preserve transparent lens content");
    defaults.magnifier.connection_type = SNOW_MAGNIFIER_CONNECTION_GRADIENT;
    defaults.magnifier.source_outline = 1;
    require(snow_viewport_set_magnifier_style_patch_ex(
                runtime.get(), viewport.get(), &defaults.magnifier,
                SNOW_MAGNIFIER_STYLE_PROPERTY_CONNECTION_TYPE |
                    SNOW_MAGNIFIER_STYLE_PROPERTY_SOURCE_OUTLINE,
                0, changed.outParam()) == SNOW_OK &&
                cache.sync(runtime.get(), viewport.get()) &&
                cache.sceneItems()[0].arrow_point_count == 0,
            "gradient mode must remove leader geometry and publish its source outline");
    for (const double scale : {1.0, 1.25, 2.0}) {
        const QImage bridge =
            snow_canvas_export::renderToImage(runtime.get(), QRectF(60, 65, 20, 10),
                                              QSize(qRound(20 * scale), qRound(10 * scale)), {});
        require(bridge.pixelColor(bridge.width() / 2, bridge.height() / 2).alpha() > 0,
                "a cropped export containing only the bridge must retain the gradient");
        const QImage sourceOutline = snow_canvas_export::renderToImage(
            runtime.get(), QRectF(18, 65, 5, 10), QSize(qRound(5 * scale), qRound(10 * scale)), {});
        require(sourceOutline.pixelColor(qRound(2 * scale), sourceOutline.height() / 2).alpha() >
                    100,
                "a cropped export outside the destination must include the source outline");
    }
    snow_canvas_export::resetDiagnosticsForCurrentThread();
    const QImage gradientExport = snow_canvas_export::renderToImage(
        runtime.get(), QRectF(100, 40, 60, 60), QSize(120, 120), sources);
    require(gradientExport.pixelColor(70, 70) == QColor(70, 30, 200) &&
                snow_canvas_export::diagnosticsForCurrentThread().directSourceFastPathCount == 1 &&
                snow_canvas_export::diagnosticsForCurrentThread().fullCompositorPathCount == 0,
            "gradient decoration must retain pristine cropped sampling and the direct export path");
}

void connectionModesAndSourceOutlinesAreIndependent() {
    auto lens = magnifier();
    const SnowArrowPoint leader[]{{99, 70}, {35, 70}};
    lens.setArrowPoints(leader, 2);
    lens.arrow_type = SNOW_ARROW_TYPE_STRAIGHT;
    lens.arrow_ratio = 1;
    lens.arrow_stroke_style = SNOW_STROKE_STYLE_SOLID;
    for (const auto mode : {SNOW_MAGNIFIER_CONNECTION_NONE, SNOW_MAGNIFIER_CONNECTION_LEADER_LINE,
                            SNOW_MAGNIFIER_CONNECTION_GRADIENT}) {
        for (const bool outline : {false, true}) {
            lens.magnifier.connection_type = mode;
            lens.magnifier.source_outline = outline ? 1 : 0;
            const QImage output = render({lens}, {});
            require((pixel(output, 20, 70).alpha() > 100) == outline,
                    "source outline must toggle independently of the connection mode");
            const int connectionAlpha = pixel(output, 70, 70).alpha();
            require(mode == SNOW_MAGNIFIER_CONNECTION_NONE ? connectionAlpha == 0
                    : mode == SNOW_MAGNIFIER_CONNECTION_LEADER_LINE
                        ? connectionAlpha > 200
                        : connectionAlpha > 0 && connectionAlpha < 110,
                    "None, Leader line and Gradient must draw only their selected connection");
            require(pixel(output, 130, 70).alpha() == 0 && pixel(output, 100, 70).alpha() > 100,
                    "decorations must preserve the transparent lens interior and its border");
        }
    }
}

void gradientUsesStrokeColorAndCombinedOpacity() {
    for (const auto stroke : {SnowColorRgba8{230, 60, 30, 255}, SnowColorRgba8{30, 80, 220, 128}}) {
        for (const double callerOpacity : {1.0, 0.6}) {
            auto lens = magnifier();
            lens.magnifier.connection_type = SNOW_MAGNIFIER_CONNECTION_GRADIENT;
            lens.stroke = stroke;
            lens.stroke_width = 0;
            lens.opacity = 0.7;
            QImage output(200, 140, QImage::Format_ARGB32_Premultiplied);
            output.fill(Qt::transparent);
            {
                QPainter painter(&output);
                painter.setRenderHint(QPainter::Antialiasing);
                painter.setOpacity(callerOpacity);
                snow_canvas_magnifier_renderer::render(painter, projection(), lens, nullptr);
            }
            int previousAlpha = -1;
            for (const int x : {60, 75, 90}) {
                const QRgb sample = output.pixel(x, 70);
                const double fraction = (x + 0.5 - 35.0) / 95.0;
                const int expected =
                    qRound(stroke.a * (0.16 + 0.26 * fraction) * 0.7 * callerOpacity);
                require(qAbs(qAlpha(sample) - expected) <= 2 && qAlpha(sample) > previousAlpha,
                        "gradient opacity must increase from 16% to 42% and multiply every alpha");
                previousAlpha = qAlpha(sample);
                require(qAbs(qRed(sample) - qRound(stroke.r * qAlpha(sample) / 255.0)) <= 1 &&
                            qAbs(qGreen(sample) - qRound(stroke.g * qAlpha(sample) / 255.0)) <= 1 &&
                            qAbs(qBlue(sample) - qRound(stroke.b * qAlpha(sample) / 255.0)) <= 1,
                        "gradient RGB must come from the stroke color");
            }
        }
    }
}

void gradientContoursExcludeBodiesAcrossShapesDirectionsAndProjection() {
    for (const auto shape : {SNOW_DISPLAY_RECT_SHAPE_RECTANGLE, SNOW_DISPLAY_RECT_SHAPE_ELLIPSE,
                             SNOW_DISPLAY_RECT_SHAPE_DIAMOND}) {
        for (const double rotation : {0.0, 0.63}) {
            for (int direction = 0; direction < 8; ++direction) {
                auto lens = magnifier(2, static_cast<std::uint8_t>(shape));
                lens.width = 40;
                lens.height = 30;
                lens.magnifier.source_width = 20;
                lens.magnifier.source_height = 15;
                lens.magnifier.source_center_x = 100;
                lens.magnifier.source_center_y = 70;
                const double angle = direction * M_PI / 4.0;
                lens.center_x = 100 + 65 * std::cos(angle);
                lens.center_y = 70 + 65 * std::sin(angle);
                lens.rotation = rotation;
                lens.corner_radii = {8, 5, 3, 6};
                lens.stroke_width = 0;
                lens.magnifier.connection_type = SNOW_MAGNIFIER_CONNECTION_GRADIENT;
                for (const double zoom : {0.5, 1.0, 2.0}) {
                    auto info = projection();
                    info.camera_zoom = zoom;
                    QTransform toView;
                    toView.translate(100, 70);
                    toView.scale(zoom, zoom);
                    toView.translate(-100, -70);
                    const QPainterPath source =
                        toView.map(snow_canvas_magnifier_renderer::sourcePath(lens));
                    const QPainterPath target =
                        toView.map(snow_canvas_magnifier_renderer::lensPath(lens));
                    for (const double dpr : {1.0, 1.25, 2.0}) {
                        const QImage full = render({lens}, {}, dpr, info);
                        int bridgePixels = 0;
                        for (int y = 2; y < 138; y += 3) {
                            for (int x = 2; x < 198; x += 3) {
                                const int alpha = pixel(full, x, y).alpha();
                                const QRectF guard(x - 1, y - 1, 2, 2);
                                if ((source.contains(guard) || target.contains(guard)) &&
                                    alpha != 0) {
                                    std::cerr << "gradient interior mismatch: shape " << shape
                                              << ", rotation " << rotation << ", direction "
                                              << direction << ", zoom " << zoom << ", DPR " << dpr
                                              << ", pixel " << x << ',' << y << ", alpha " << alpha
                                              << ", source " << source.contains(guard) << ", lens "
                                              << target.contains(guard) << '\n';
                                    require(false,
                                            "gradient must exclude both actual curved interiors");
                                }
                                bridgePixels += alpha > 0 ? 1 : 0;
                            }
                        }
                        require(
                            bridgePixels > 0,
                            "every shape and connection direction must retain a visible bridge");
                        const QRegion exposure(QRect(65, 35, 70, 70));
                        const QImage partial = render({lens}, {}, dpr, info, exposure);
                        for (int y = 37; y < 103; y += 3) {
                            for (int x = 67; x < 133; x += 3) {
                                const int physicalX = qFloor(x * dpr);
                                const int physicalY = qFloor(y * dpr);
                                const QRgb a = partial.pixel(physicalX, physicalY);
                                const QRgb b = full.pixel(physicalX, physicalY);
                                // Splitting antialiased spans may round a premultiplied
                                // channel, including alpha, by one. Unpremultiplication
                                // amplifies that harmless difference for translucent pixels.
                                if (std::max({qAbs(qAlpha(a) - qAlpha(b)), qAbs(qRed(a) - qRed(b)),
                                              qAbs(qGreen(a) - qGreen(b)),
                                              qAbs(qBlue(a) - qBlue(b))}) > 1) {
                                    std::cerr << "gradient exposure mismatch: shape " << shape
                                              << ", rotation " << rotation << ", direction "
                                              << direction << ", zoom " << zoom << ", DPR " << dpr
                                              << ", pixel " << x << ',' << y << ", alpha "
                                              << pixel(full, x, y).alpha() << "/"
                                              << pixel(partial, x, y).alpha() << '\n';
                                    require(false, "partial painting must retain the complete "
                                                   "gradient geometry");
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

void scaledSourceContoursAndOverlapRemainClean() {
    for (const auto shape : {SNOW_DISPLAY_RECT_SHAPE_RECTANGLE, SNOW_DISPLAY_RECT_SHAPE_ELLIPSE,
                             SNOW_DISPLAY_RECT_SHAPE_DIAMOND}) {
        auto lens = magnifier(2, static_cast<std::uint8_t>(shape));
        lens.corner_radii = {12, 8, 4, 16};
        lens.rotation = 0.37;
        QTransform scaleSource;
        scaleSource.translate(lens.magnifier.source_center_x, lens.magnifier.source_center_y);
        scaleSource.scale(0.5, 0.5);
        scaleSource.translate(-lens.center_x, -lens.center_y);
        const QPainterPath expected =
            scaleSource.map(snow_canvas_magnifier_renderer::lensPath(lens));
        const QPainterPath actual = snow_canvas_magnifier_renderer::sourcePath(lens);
        require(actual.elementCount() == expected.elementCount(),
                "source contour must preserve the magnified contour's scaled topology");
        for (int index = 0; index < actual.elementCount(); ++index) {
            const auto a = actual.elementAt(index);
            const auto b = expected.elementAt(index);
            require(std::hypot(a.x - b.x, a.y - b.y) < 1e-9,
                    "source shape and corner radii must scale with the magnification factor");
        }
        lens.magnifier.connection_type = SNOW_MAGNIFIER_CONNECTION_GRADIENT;
        lens.magnifier.source_outline = 1;
        lens.center_x = lens.magnifier.source_center_x = 100;
        lens.center_y = lens.magnifier.source_center_y = 70;
        const QImage concentric = render({lens}, {});
        require(pixel(concentric, 100, 70).alpha() == 0 && pixel(concentric, 100, 58).alpha() == 0,
                "a contained source must not leave gradient tint or inner source marks");
        auto noStroke = lens;
        noStroke.stroke_width = 0;
        noStroke.magnifier.source_outline = 0;
        require(
            alphaWeight(render({noStroke}, {})) == 0,
            "contained source geometry must omit the entire bridge, including antialias fringes");
        lens.center_x = 120;
        const QImage overlapping = render({lens}, {});
        require(pixel(overlapping, 112, 70).alpha() == 0,
                "overlapping source and lens interiors must never be filled by an odd-even cutout");
        lens.magnifier.magnification_factor = 1;
        lens.width = lens.magnifier.source_width;
        lens.height = lens.magnifier.source_height;
        lens.center_x = lens.magnifier.source_center_x;
        lens.center_y = lens.magnifier.source_center_y;
        lens.stroke.a = 128;
        lens.opacity = 0.5;
        const QImage coincidentOutline = render({lens}, {});
        lens.magnifier.source_outline = 0;
        require(render({lens}, {}) == coincidentOutline,
                "coincident one-times outlines must not double the magnifier border opacity");
    }
}

void extremeFactorsTouchingAndInvalidGeometryRemainSafe() {
    const auto requireClearBodies = [](const SnowCanvasSceneItem& lens, const QImage& output) {
        const auto source = snow_canvas_magnifier_renderer::sourcePath(lens);
        const auto target = snow_canvas_magnifier_renderer::lensPath(lens);
        for (int y = 1; y < 139; y += 2) {
            for (int x = 1; x < 199; x += 2) {
                // An integer-addressed physical pixel covers this full square.
                const QRectF guard(x, y, 1, 1);
                if ((source.contains(guard) || target.contains(guard)) &&
                    pixel(output, x, y).alpha() != 0) {
                    std::cerr << "compact gradient interior mismatch: shape "
                              << static_cast<int>(lens.rect_shape) << ", factor "
                              << lens.magnifier.magnification_factor << ", rotation "
                              << lens.rotation << ", lens " << lens.center_x << ',' << lens.center_y
                              << ", pixel " << x << ',' << y << ", alpha "
                              << pixel(output, x, y).alpha() << ", source "
                              << source.contains(guard) << ", lens " << target.contains(guard)
                              << '\n';
                    require(false,
                            "exact tangent connections must keep both true body interiors clear");
                }
            }
        }
    };
    for (const auto shape : {SNOW_DISPLAY_RECT_SHAPE_RECTANGLE, SNOW_DISPLAY_RECT_SHAPE_ELLIPSE,
                             SNOW_DISPLAY_RECT_SHAPE_DIAMOND}) {
        for (const double factor : {1.0, 10.0}) {
            auto lens = magnifier(factor, static_cast<std::uint8_t>(shape));
            lens.width = 60;
            lens.height = 40;
            lens.magnifier.source_width = 60 / factor;
            lens.magnifier.source_height = 40 / factor;
            lens.magnifier.connection_type = SNOW_MAGNIFIER_CONNECTION_GRADIENT;
            lens.stroke_width = 0;
            lens.corner_radii = {1000, 30, 500, 70};
            lens.rotation = 0.37;
            std::vector<SnowCanvasSceneItem> items{lens};
            const QImage output = render(items, {});
            require(alphaWeight(output) > 0 &&
                        !items[0].magnifierGeometryCache().disjointBridge.isEmpty(),
                    "one-times and ten-times connections must use their exact curved bridge");
            requireClearBodies(lens, output);
            lens.corner_radii = {};
            lens.rotation = 0;
            const QImage aligned = render({lens}, {});
            require(pixel(aligned, 80, 70).alpha() > 0,
                    "aligned sharp contours must resolve flat support ties into a visible bridge");
            requireClearBodies(lens, aligned);
        }
        for (const double gap : {-0.0001, 0.0, 0.0001}) {
            auto lens = magnifier(2, static_cast<std::uint8_t>(shape));
            lens.width = 60;
            lens.height = 40;
            lens.magnifier.source_width = 30;
            lens.magnifier.source_height = 20;
            lens.center_x = 80 + gap;
            lens.corner_radii = {};
            lens.stroke_width = 0;
            lens.magnifier.connection_type = SNOW_MAGNIFIER_CONNECTION_GRADIENT;
            const QImage output = render({lens}, {});
            require(alphaWeight(output) > 0,
                    "touching and nearly touching bodies must retain their exterior connection");
            requireClearBodies(lens, output);
        }
    }
    auto lens = magnifier();
    lens.stroke_width = 0;
    lens.magnifier.connection_type = SNOW_MAGNIFIER_CONNECTION_GRADIENT;
    const auto requireEmpty = [&](const SnowCanvasSceneItem& invalid) {
        require(alphaWeight(render({invalid}, {})) == 0,
                "degenerate or nonfinite magnifier geometry must paint safely empty");
    };
    for (const double extent : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                                std::numeric_limits<double>::quiet_NaN()}) {
        auto invalid = lens;
        invalid.width = extent;
        requireEmpty(invalid);
        invalid = lens;
        invalid.magnifier.source_height = extent;
        requireEmpty(invalid);
    }
    for (const double coordinate :
         {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        auto invalid = lens;
        invalid.center_x = coordinate;
        requireEmpty(invalid);
        invalid = lens;
        invalid.magnifier.source_center_y = coordinate;
        requireEmpty(invalid);
        invalid = lens;
        invalid.rotation = coordinate;
        requireEmpty(invalid);
    }
}

void decorationsPaintWhenOnlySourceOrBridgeIsExposed() {
    auto lens = magnifier();
    lens.center_x = 300;
    lens.magnifier.connection_type = SNOW_MAGNIFIER_CONNECTION_GRADIENT;
    lens.magnifier.source_outline = 1;
    for (const double dpr : {1.0, 1.25, 2.0}) {
        const QImage source = render({lens}, {}, dpr, projection(), QRegion(QRect(18, 65, 5, 10)));
        require(pixel(source, 20, 70).alpha() > 100,
                "source-only exposure must survive destination culling");
        lens.stroke_width = 0;
        const QImage bridge = render({lens}, {}, dpr, projection(), QRegion(QRect(70, 65, 10, 10)));
        require(pixel(bridge, 75, 70).alpha() > 0,
                "bridge-only exposure must paint even with no exposed lens or visible border");
        lens.stroke_width = 2;
    }
    lens.stroke.a = 0;
    require(alphaWeight(render({lens}, {})) == 0,
            "a transparent stroke must suppress both outline and gradient decorations");
}

void retainedGeometryReusesCopiesMovesAndAppearanceChanges() {
    std::vector<SnowCanvasSceneItem> items{magnifier()};
    auto& lens = items.front();
    lens.corner_radii = {10, 8, 6, 4};
    lens.magnifier.connection_type = SNOW_MAGNIFIER_CONNECTION_GRADIENT;
    lens.magnifier.source_outline = 1;
    snow_canvas_magnifier_renderer::resetDiagnosticsForCurrentThread();
    const QImage initial = render(items, {});
    auto diagnostics = snow_canvas_magnifier_renderer::diagnosticsForCurrentThread();
    require(diagnostics.geometryBuildCount == 1 && diagnostics.bridgeHullBuildCount == 1,
            "the first gradient paint must build its vector geometry once");
    snow_canvas_magnifier_renderer::resetDiagnosticsForCurrentThread();
    require(render(items, {}) == initial, "retained geometry must preserve static appearance");
    diagnostics = snow_canvas_magnifier_renderer::diagnosticsForCurrentThread();
    require(diagnostics.geometryBuildCount == 0 && diagnostics.bridgeHullBuildCount == 0 &&
                diagnostics.geometryReuseCount == 1 && diagnostics.bridgeHullReuseCount == 1,
            "static painting must reuse both contours and the cached hull");
    snow_canvas_magnifier_renderer::resetDiagnosticsForCurrentThread();
    render(items, {}, 2);
    diagnostics = snow_canvas_magnifier_renderer::diagnosticsForCurrentThread();
    require(diagnostics.geometryBuildCount == 0 && diagnostics.bridgeHullBuildCount == 1,
            "a higher physical scale must rebuild only hull precision");
    snow_canvas_magnifier_renderer::resetDiagnosticsForCurrentThread();
    lens.stroke = {220, 40, 20, 128};
    lens.stroke_width = 4;
    lens.opacity = 0.7;
    render(items, {});
    diagnostics = snow_canvas_magnifier_renderer::diagnosticsForCurrentThread();
    require(diagnostics.geometryBuildCount == 0 && diagnostics.bridgeHullBuildCount == 0,
            "appearance edits and reduced scale must retain vector geometry");
    auto copy = items;
    copy.front().magnifier.source_center_x += 5;
    snow_canvas_magnifier_renderer::resetDiagnosticsForCurrentThread();
    render(copy, {});
    require(snow_canvas_magnifier_renderer::diagnosticsForCurrentThread().geometryBuildCount == 1,
            "direct geometry mutations must invalidate a copied cache");
    snow_canvas_magnifier_renderer::resetDiagnosticsForCurrentThread();
    render(items, {});
    require(snow_canvas_magnifier_renderer::diagnosticsForCurrentThread().geometryBuildCount == 0,
            "copied cache invalidation must not alter the original item's retained geometry");
    auto moved = std::move(items);
    snow_canvas_magnifier_renderer::resetDiagnosticsForCurrentThread();
    render(moved, {});
    require(snow_canvas_magnifier_renderer::diagnosticsForCurrentThread().geometryBuildCount == 0,
            "moving an owned item must retain its vector cache");
    moved.front().width += 4;
    snow_canvas_magnifier_renderer::resetDiagnosticsForCurrentThread();
    render(moved, {});
    require(snow_canvas_magnifier_renderer::diagnosticsForCurrentThread().geometryBuildCount == 1,
            "direct lens dimension changes must invalidate vector geometry");
    const QImage normalScale = render(moved, {});
    auto extreme = projection();
    extreme.camera_zoom = 1e12;
    snow_canvas_magnifier_renderer::resetDiagnosticsForCurrentThread();
    {
        QImage boundedOutput(1, 1, QImage::Format_ARGB32_Premultiplied);
        boundedOutput.fill(Qt::transparent);
        QPainter painter(&boundedOutput);
        painter.setClipRect(QRect(0, 0, 1, 1));
        // Invoke directly: scene culling correctly rejects bounds outside the
        // integer update-region range before dispatching this extreme zoom.
        snow_canvas_magnifier_renderer::render(painter, extreme, moved.front(), nullptr);
    }
    require(snow_canvas_magnifier_renderer::diagnosticsForCurrentThread().bridgeHullBuildCount == 1,
            "pathological precision must stop at the bounded contour budget");
    snow_canvas_magnifier_renderer::resetDiagnosticsForCurrentThread();
    require(
        render(moved, {}) == normalScale &&
            snow_canvas_magnifier_renderer::diagnosticsForCurrentThread().bridgeHullBuildCount == 0,
        "a failed extreme-scale hull must preserve the valid zoomed-out cache");
}

void filterCachesInvalidateMagnifierDecorationsAndSampling() {
    auto lens = magnifier();
    lens.magnifier.connection_type = SNOW_MAGNIFIER_CONNECTION_GRADIENT;
    lens.magnifier.source_outline = 1;
    auto filter = lens;
    filter.kind = SNOW_SCENE_DISPLAY_ITEM_FILTER;
    filter.element_id = {2, 1};
    filter.center_x = 100;
    filter.center_y = 70;
    filter.width = 200;
    filter.height = 140;
    filter.stroke.a = 0;
    filter.filter = snow_filter_render_spec_resolve(2, 1);
    std::vector<SnowCanvasSceneItem> items{lens, filter};
    const QList<SnowCanvasBaseImageSource> sources{{sourceImage(), QRectF(0, 0, 200, 140), {}}};
    for (const std::uint64_t contentKey : {0u, 17u}) {
        snow_canvas_filter_tile_cache::clear();
        const auto verify = [&] {
            const QImage direct = render(items, sources);
            const QImage tiled =
                render(items, sources, 1, projection(), {}, true, &items, contentKey);
            require(direct == tiled,
                    "cached filter replay must follow every magnifier decoration and source edit");
        };
        verify();
        verify();
        items[0].magnifier.connection_type = SNOW_MAGNIFIER_CONNECTION_NONE;
        verify();
        items[0].magnifier.source_outline = 0;
        verify();
        items[0].stroke = {220, 50, 80, 180};
        items[0].stroke_width = 4;
        verify();
        items[0].magnifier.connection_type = SNOW_MAGNIFIER_CONNECTION_GRADIENT;
        items[0].magnifier.source_outline = 1;
        verify();
        items[0].magnifier.source_center_x += 4;
        items[0].magnifier.magnification_factor = 3;
        items[0].magnifier.source_width = items[0].width / 3;
        items[0].magnifier.source_height = items[0].height / 3;
        verify();
    }
    snow_canvas_filter_tile_cache::clear();
}

void gradientIncrementalRepaintsMatchFreshFrames() {
    for (const auto shape : {SNOW_RECTANGLE_SHAPE_RECTANGLE, SNOW_RECTANGLE_SHAPE_ELLIPSE,
                             SNOW_RECTANGLE_SHAPE_DIAMOND}) {
        for (double dpr : {1.0, 1.25, 2.0}) {
            SnowStyleDefaults defaults{};
            require(snow_runtime_style_defaults_default(&defaults) == SNOW_OK, "load defaults");
            defaults.magnifier.shape = shape;
            defaults.magnifier.connection_type = SNOW_MAGNIFIER_CONNECTION_GRADIENT;
            defaults.magnifier.source_outline = 1;
            defaults.magnifier.corner_radii = {12, 8, 20, 6};
            defaults.magnifier.stroke = {255, 32, 50, 255};
            ScopedRuntimeHandle runtime;
            SnowRuntimeConfig config{&defaults};
            require(snow_runtime_create_with_config(&config, runtime.outParam()) == SNOW_OK,
                    "create incremental magnifier runtime");
            SnowCanvasViewport viewport;
            require(viewport.create(runtime.get(), snow_canvas_viewport::defaultEngineConfig()) &&
                        snow_viewport_set_surface_size(runtime.get(), viewport.get(), 600, 420) ==
                            SNOW_OK &&
                        snow_viewport_set_camera(runtime.get(), viewport.get(), 300, 210, 1) ==
                            SNOW_OK,
                    "configure incremental magnifier viewport");
            SnowCanvasDisplayCache cache;
            require(cache.sync(runtime.get(), viewport.get()), "initialize incremental cache");
            QImage retained(QSize(qCeil(600 * dpr), qCeil(420 * dpr)),
                            QImage::Format_ARGB32_Premultiplied);
            retained.setDevicePixelRatio(dpr);
            retained.fill(Qt::transparent);
            const auto verify = [&](const char* stage) {
                require(cache.sync(runtime.get(), viewport.get()), "sync magnifier edit");
                const QRegion damage = snow_canvas_display::dirtyRectsToRegion(
                    cache.sceneDirtyRects(), cache.sceneDirtyRectCount(), QRect(0, 0, 600, 420));
                if (!damage.isEmpty()) {
                    QPainter painter(&retained);
                    painter.setClipRegion(damage);
                    painter.setCompositionMode(QPainter::CompositionMode_Source);
                    painter.fillRect(QRect(0, 0, 600, 420), Qt::transparent);
                    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
                    painter.setRenderHint(QPainter::Antialiasing);
                    std::vector<std::uint32_t> candidates;
                    cache.sceneCandidateIndices(damage, &candidates);
                    snow_canvas_renderer::SceneRenderRequest request;
                    request.painter = &painter;
                    request.displayInfo = &cache.sceneInfo();
                    request.sceneItems = cache.sceneItems();
                    request.sceneItemCount = cache.sceneItemCount();
                    request.exposedRegion = damage;
                    request.clearBackgroundEnabled = false;
                    request.candidateIndices = candidates.data();
                    request.candidateCount = static_cast<std::uint32_t>(candidates.size());
                    snow_canvas_renderer::renderSceneItems(request);
                }
                std::vector<SnowCanvasSceneItem> items;
                for (std::uint32_t index = 0; index < cache.sceneItemCount(); ++index)
                    items.push_back(cache.sceneItems()[index]);
                const QImage fresh = render(items, {}, dpr, cache.sceneInfo());
                for (int y = 0; y < fresh.height(); ++y) {
                    const auto* expected = reinterpret_cast<const QRgb*>(fresh.constScanLine(y));
                    const auto* actual = reinterpret_cast<const QRgb*>(retained.constScanLine(y));
                    for (int x = 0; x < fresh.width(); ++x) {
                        const QRgb a = expected[x];
                        const QRgb b = actual[x];
                        if (std::max({qAbs(qAlpha(a) - qAlpha(b)), qAbs(qRed(a) - qRed(b)),
                                      qAbs(qGreen(a) - qGreen(b)), qAbs(qBlue(a) - qBlue(b))}) >
                            1) {
                            std::cerr << "incremental magnifier mismatch: " << stage << ", shape "
                                      << shape << ", DPR " << dpr << ", pixel " << x << ',' << y
                                      << ", alpha " << qAlpha(a) << '/' << qAlpha(b) << '\n';
                            require(false, "scene damage must repaint every changed bridge, source "
                                           "outline and lens pixel without leaving trails");
                        }
                    }
                }
            };
            ScopedChangedViewportList changed;
            require(snow_viewport_set_active_tool_ex(runtime.get(), viewport.get(),
                                                     SNOW_ACTIVE_TOOL_MAGNIFIER,
                                                     changed.outParam()) == SNOW_OK,
                    "activate incremental magnifier");
            pointer(runtime.get(), viewport.get(), SNOW_POINTER_EVENT_DOWN, {60, 280});
            pointer(runtime.get(), viewport.get(), SNOW_POINTER_EVENT_MOVE, {140, 340});
            verify("creation preview");
            pointer(runtime.get(), viewport.get(), SNOW_POINTER_EVENT_UP, {140, 340});
            verify("creation commit");
            pointer(runtime.get(), viewport.get(), SNOW_POINTER_EVENT_DOWN, {100, 310});
            pointer(runtime.get(), viewport.get(), SNOW_POINTER_EVENT_UP, {100, 310});
            verify("selection");
            const QPointF grip(100, 394);
            pointer(runtime.get(), viewport.get(), SNOW_POINTER_EVENT_DOWN, grip);
            for (int step = 1; step <= 16; ++step) {
                const QPointF delta(340.0 * step / 16, -220.0 * step / 16);
                pointer(runtime.get(), viewport.get(), SNOW_POINTER_EVENT_MOVE, grip + delta);
                verify("lens drag preview");
            }
            pointer(runtime.get(), viewport.get(), SNOW_POINTER_EVENT_UP,
                    grip + QPointF(340, -220));
            verify("lens drag commit");
            require(std::abs(cache.sceneItems()[0].center_x - 440) < 0.01,
                    "incremental fixture must actually move the lens");
            pointer(runtime.get(), viewport.get(), SNOW_POINTER_EVENT_DOWN, {100, 310});
            pointer(runtime.get(), viewport.get(), SNOW_POINTER_EVENT_MOVE, {125, 280});
            verify("source drag preview");
            pointer(runtime.get(), viewport.get(), SNOW_POINTER_EVENT_UP, {125, 280});
            verify("source drag commit");
            for (double zoom : {0.5, 1.25, 2.0}) {
                require(snow_viewport_set_camera(runtime.get(), viewport.get(), 300, 210, zoom) ==
                            SNOW_OK,
                        "zoom and clip the incremental magnifier");
                verify("camera zoom");
                for (const auto mode :
                     {SNOW_MAGNIFIER_CONNECTION_NONE, SNOW_MAGNIFIER_CONNECTION_LEADER_LINE,
                      SNOW_MAGNIFIER_CONNECTION_GRADIENT}) {
                    for (bool outline : {false, true}) {
                        defaults.magnifier.connection_type = mode;
                        defaults.magnifier.source_outline = outline ? 1 : 0;
                        require(snow_viewport_set_magnifier_style_patch_ex(
                                    runtime.get(), viewport.get(), &defaults.magnifier,
                                    SNOW_MAGNIFIER_STYLE_PROPERTY_CONNECTION_TYPE |
                                        SNOW_MAGNIFIER_STYLE_PROPERTY_SOURCE_OUTLINE,
                                    0, changed.outParam()) == SNOW_OK,
                                "change incremental connection style");
                        verify("connection style");
                    }
                }
            }
            defaults.magnifier.factor = 3;
            require(snow_viewport_set_magnifier_style_patch_ex(
                        runtime.get(), viewport.get(), &defaults.magnifier,
                        SNOW_MAGNIFIER_STYLE_PROPERTY_FACTOR, 0, changed.outParam()) == SNOW_OK,
                    "resize incremental lens");
            verify("factor edit");
            require(snow_runtime_undo_ex(runtime.get(), changed.outParam()) == SNOW_OK,
                    "undo incremental factor edit");
            verify("undo");
            require(snow_runtime_redo_ex(runtime.get(), changed.outParam()) == SNOW_OK,
                    "redo incremental factor edit");
            verify("redo");
            require(snow_viewport_delete_selected_ex(runtime.get(), viewport.get(),
                                                     changed.outParam()) == SNOW_OK,
                    "delete incremental magnifier");
            verify("delete");
            require(cache.sceneItemCount() == 0, "deletion must clear the complete magnifier");
        }
    }
}

void saveDecorationInspectionSheet() {
    const QString directory = qEnvironmentVariable("SNOW_MAGNIFIER_RENDER_PREVIEW_DIR");
    if (directory.isEmpty())
        return;
    require(QDir().mkpath(directory), "create magnifier inspection output directory");
    QImage sheet(1200, 1110, QImage::Format_ARGB32_Premultiplied);
    sheet.fill(Qt::white);
    QPainter painter(&sheet);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QColor(35, 40, 50));
    QString family = QFontDatabase::systemFont(QFontDatabase::GeneralFont).family();
#if defined(Q_OS_WIN)
    const QString fontFile = QDir(qEnvironmentVariable("WINDIR", QStringLiteral("C:/Windows")))
                                 .filePath(QStringLiteral("Fonts/segoeui.ttf"));
    const int fontId = QFontDatabase::addApplicationFont(fontFile);
    if (fontId >= 0 && !QFontDatabase::applicationFontFamilies(fontId).isEmpty())
        family = QFontDatabase::applicationFontFamilies(fontId).front();
#endif
    painter.setFont(QFont(family, 14));
    painter.drawText(QPointF(25, 30),
                     QStringLiteral("Magnifier: gradient connection and source outline"));
    auto reference = magnifier();
    reference.center_x = 800;
    reference.center_y = 140;
    reference.width = 560;
    reference.height = 260;
    reference.magnifier = {170, 310, 280, 130, 2, SNOW_MAGNIFIER_CONNECTION_GRADIENT, 1, {}};
    reference.stroke = {230, 83, 70, 255};
    reference.stroke_width = 3;
    auto referenceInfo = projection();
    referenceInfo.surface_width = 1200;
    referenceInfo.surface_height = 400;
    referenceInfo.camera_center_x = 600;
    referenceInfo.camera_center_y = 200;
    const QImage referenceDrawing = render({reference}, {}, 1, referenceInfo);
    painter.drawImage(QPoint(0, 45), referenceDrawing);
    QImage referencePreview(referenceDrawing.size(), QImage::Format_ARGB32_Premultiplied);
    referencePreview.fill(Qt::white);
    {
        QPainter referencePainter(&referencePreview);
        referencePainter.drawImage(QPoint(), referenceDrawing);
    }
    require(
        referencePreview.save(QDir(directory).filePath(QStringLiteral("magnifier-reference.png"))),
        "save standalone magnifier reference preview");
    for (int row = 0; row < 3; ++row) {
        for (int shape = 0; shape < 3; ++shape) {
            auto lens = magnifier(2, static_cast<std::uint8_t>(shape));
            lens.width = 140;
            lens.height = 100;
            lens.magnifier = {75, 155, 70, 50, 2, SNOW_MAGNIFIER_CONNECTION_GRADIENT, 1, {}};
            lens.center_x = 270;
            lens.center_y = 70;
            lens.stroke = {33, 107, 165, 255};
            lens.stroke_width = 2.5;
            lens.corner_radii = {16, 10, 6, 12};
            if (row == 1) {
                lens.magnifier.source_center_x = 310;
                lens.center_x = 125;
                lens.rotation = 0.4;
            } else if (row == 2) {
                lens.magnifier.source_center_x = 155;
                lens.magnifier.source_center_y = 125;
                lens.center_x = 195;
                lens.center_y = 115;
                lens.rotation = -0.2;
            }
            auto info = projection();
            info.surface_width = 400;
            info.surface_height = 210;
            info.camera_center_x = 200;
            info.camera_center_y = 105;
            const QPoint origin(shape * 400, 440 + row * 220);
            painter.drawText(origin + QPoint(15, 20),
                             QStringLiteral("%1 · %2")
                                 .arg(shape == 0   ? QStringLiteral("Rectangle")
                                      : shape == 1 ? QStringLiteral("Ellipse")
                                                   : QStringLiteral("Diamond"))
                                 .arg(row == 0   ? QStringLiteral("Diagonal")
                                      : row == 1 ? QStringLiteral("Reversed and rotated")
                                                 : QStringLiteral("Overlap")));
            painter.drawImage(origin + QPoint(0, 25), render({lens}, {}, 1, info));
        }
    }
    painter.end();
    require(sheet.save(QDir(directory).filePath(QStringLiteral("magnifier-decorations.png"))),
            "save magnifier decoration inspection sheet");
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QApplication application(argc, argv);
    if (application.arguments().contains(QStringLiteral("--selection-guides-only"))) {
        magnifierHoverAndSelectionReuseCreationGuidesAtEveryShapeZoomAndDpr();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--gradient-damage-only"))) {
        gradientIncrementalRepaintsMatchFreshFrames();
        return 0;
    }
    saveDecorationInspectionSheet();
    connectionModesAndSourceOutlinesAreIndependent();
    gradientUsesStrokeColorAndCombinedOpacity();
    gradientContoursExcludeBodiesAcrossShapesDirectionsAndProjection();
    scaledSourceContoursAndOverlapRemainClean();
    extremeFactorsTouchingAndInvalidGeometryRemainSafe();
    decorationsPaintWhenOnlySourceOrBridgeIsExposed();
    retainedGeometryReusesCopiesMovesAndAppearanceChanges();
    filterCachesInvalidateMagnifierDecorationsAndSampling();
    magnifierHoverAndSelectionReuseCreationGuidesAtEveryShapeZoomAndDpr();
    samplesOnlyOriginalPixelsAtEveryFactorAndShape();
    shapesAndBorderClipCleanly();
    roundedCornersClipSamplingAndStroke();
    outerBordersSurvivePartialAndOffViewportPainting();
    missingPixelsStayTransparentAndCoverageIsRespected();
    multilayerOpacityComposesOriginalsBeforeElementAppearance();
    multilayerOpacityTilesKeepSparseClipsAndBoundedScratch();
    multilayerCompositingPreservesCallerStateAndEmptyClips();
    partialPaintPreservesMasksWhenOnlyOneOriginalLayerIsExposed();
    patternedMultilayerSamplingPreservesSpanEndpointsAcrossCompositingStripes();
    partialPaintAndProjectionRetainOriginalSampling();
    integerProjectionPartialPaintKeepsExactSamples();
    partialPaintKeepsCanonicalSamplingAtEverySourceDensity();
    tiledPaintKeepsCanonicalSamplingAtCoverageBoundaries();
    largeOffscreenSourcesKeepSharedPixelsAndCullUnexposedLayers();
    leaderRenderingUsesEveryArrowheadAndSceneBounds();
    samplingStaysPristineDuringFilterReplay();
    liveWidgetRetainsOriginalSourcesWithoutFilters();
    exportUsesDirectOriginalSourcesIncludingOutsideCrop();
    inheritedOpacityAppliesToTheCompleteMagnifierSceneItem();
    return 0;
}
