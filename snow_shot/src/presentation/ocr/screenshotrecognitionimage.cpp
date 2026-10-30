#include "snow_draw_engine_qt/snow_canvas_image.h"
#include "snow_shot/presentation/screenshotrecognitionimage.h"
#include "snow_shot/presentation/screenshotselectionbackground.h"
#include "snow_shot/presentation/screenshotocrtextlayout.h"
#include <QPainter>
#include <cmath>
#include <utility>

QImage renderScreenshotRecognitionImage(const ScreenshotRecognitionImageSnapshot& snapshot,
                                        const std::function<bool()>& cancelled) {
    const auto stopped = [&]() { return cancelled && cancelled(); };
    const QRectF rect = snapshot.canvasRect;
    if (snapshot.image.isNull() || !rect.isValid() || rect.isEmpty() || !std::isfinite(rect.x()) ||
        !std::isfinite(rect.y()) || !std::isfinite(rect.width()) || !std::isfinite(rect.height()) ||
        stopped()) {
        return {};
    }
    QImage image = snowCanvasConvertImage(snapshot.image, QImage::Format_ARGB32_Premultiplied);
    if (!snowCanvasDetachImage(image))
        return {};
    image.setDevicePixelRatio(1.0);
    QPainter painter(&image);
    if (!painter.isActive())
        return {};
    const QTransform canvasToPixels(
        image.width() / rect.width(), 0.0, 0.0, image.height() / rect.height(),
        -rect.x() * image.width() / rect.width(), -rect.y() * image.height() / rect.height());
    painter.setClipRect(image.rect());
    if (!snapshot.filteredImage.isNull() && snapshot.filteredCanvasRect.isValid()) {
        painter.drawImage(canvasToPixels.mapRect(snapshot.filteredCanvasRect),
                          snapshot.filteredImage);
    }
    ScreenshotOcrTextLayout layout;
    for (const ScreenshotOcrLine& line : snapshot.lines) {
        if (stopped())
            return {};
        if (!line.quad.boundingRect().intersects(rect))
            continue;
        QTransform textToPixels;
        if (!configureScreenshotOcrTextLayout(layout, line, canvasToPixels, snapshot.font,
                                              snapshot.textColor, {}, &textToPixels))
            continue;
        painter.save();
        painter.setTransform(textToPixels);
        layout.paint(&painter);
        painter.restore();
    }
    painter.end();
    auto style = snapshot.resultStyle;
    if (style.region) {
        const qreal scale = image.width() / rect.width();
        style.regionScale *= scale;
        style.cornerRadius = qRound(style.cornerRadius * scale);
        style.shadowWidth = qRound(style.shadowWidth * scale);
    }
    std::shared_ptr<const ScreenshotLayoutCompactionPlan> backgroundPlan;
    const QSize sourcePixelSize = image.size();
    if (snapshot.layoutCompaction.enabled && style.region) {
        const auto plan = screenshotLayoutCompactionPlan(
            *style.region, image.size(), style.regionScale, snapshot.layoutCompaction,
            ScreenshotResultCompositor::normalizedStyle(style).shadowWidth);
        backgroundPlan = plan;
        if (!plan->isValid())
            return {};
        if (plan->changed) {
            QImage compacted = snowCanvasAllocateZeroedImage(plan->outputSize, image.format());
            if (compacted.isNull())
                return {};
            compacted.setColorSpace(image.colorSpace());
            QPainter compactPainter(&compacted);
            for (const auto& tile : plan->tiles)
                compactPainter.drawImage(tile.destination, image, tile.source);
            compactPainter.end();
            image = std::move(compacted);
            style = compactedScreenshotResultStyle(style, *plan);
        }
    }
    const auto background = style.backgroundOpacityPercent > 0
                                ? renderScreenshotSelectionBackground(
                                      snapshot.backgroundSources.isEmpty()
                                          ? QList<CanvasExportSource>{{snapshot.image, rect}}
                                          : snapshot.backgroundSources,
                                      rect, sourcePixelSize, style, backgroundPlan.get(), cancelled)
                                : QImage{};
    if (style.backgroundOpacityPercent > 0 && background.isNull())
        return {};
    return stopped() ? QImage{}
                     : ScreenshotResultCompositor::compose(image, style, 1.0,
                                                           snapshot.outputOpacity, background);
}
