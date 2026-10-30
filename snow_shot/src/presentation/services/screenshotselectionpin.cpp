#include "snow_shot/presentation/screenshotselectionpin.h"

#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotresultcompositor.h"

ScreenshotPinnedSelectionRequest
screenshotSelectionPinRequest(const ScreenshotDisplaySession& displaySession,
                              const ScreenshotGeometryMapper& geometry, const QRect& selection,
                              const ScreenshotResultStyle& style,
                              ScreenshotLayoutCompaction compaction) {
    ScreenshotPinnedSelectionRequest request;
    request.resultStyle = ScreenshotResultCompositor::normalizedStyle(style);
    request.layoutCompaction = compaction.normalized();
    QSize contentSize = selection.size();
    auto outputStyle = request.resultStyle;
    if (compaction.enabled && style.region) {
        request.layoutPlan =
            screenshotLayoutCompactionPlan(*style.region, contentSize, style.regionScale,
                                           compaction, request.resultStyle.shadowWidth);
        if (!request.layoutPlan->isValid())
            return {};
        contentSize = request.layoutPlan->outputSize;
        outputStyle = compactedScreenshotResultStyle(outputStyle, *request.layoutPlan);
    }
    const ScreenshotResultLayout layout =
        ScreenshotResultCompositor::layoutForContent(contentSize, outputStyle);
    if (!layout.isValid()) {
        return request;
    }
    const int shadowPadding = layout.effectInsets.left();
    const ScreenshotPinnedImagePlacement placement = geometry.pinnedImagePlacement(
        displaySession, selection, layout.outputRect.size(), shadowPadding);
    if (!placement.valid) {
        return request;
    }
    request.selection = selection;
    request.contentCanvasRect = QRectF(selection.topLeft(), contentSize);
    request.surfaceCanvasRect = request.contentCanvasRect.adjusted(
        -static_cast<qreal>(shadowPadding), -static_cast<qreal>(shadowPadding),
        static_cast<qreal>(shadowPadding), static_cast<qreal>(shadowPadding));
    request.geometry = placement.geometry;
    request.geometry.canvasSourceRect = request.surfaceCanvasRect;
    request.initialWindowSize = layout.outputRect.size();
    request.screen = placement.screen;
    return request;
}
