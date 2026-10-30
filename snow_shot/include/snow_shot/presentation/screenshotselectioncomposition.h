#pragma once

#include "snow_shot/presentation/screenshotselectionbackground.h"
#include "snow_draw_engine_qt/snow_canvas_runtime.h"

// Region masks describe captured pixels, not document annotations. Compose the
// capture first, then draw the document inside the overall content rectangle.
// The style is already scaled to output pixels and, if needed, compacted.
inline QImage
renderAnnotatedScreenshotSelection(SnowCanvasRuntime& runtime, const QRectF& selection,
                                   const QSize& pixelSize, const QList<CanvasExportSource>& sources,
                                   const ScreenshotResultStyle& outputStyle,
                                   const ScreenshotLayoutCompactionPlan* plan = nullptr) {
    auto captureStyle = outputStyle;
    captureStyle.backgroundOpacityPercent = 100;
    const auto capture =
        renderScreenshotSelectionBackground(sources, selection, pixelSize, captureStyle, plan);
    const auto layout = ScreenshotResultCompositor::layoutForContent(
        plan ? plan->outputSize : pixelSize, outputStyle);
    if (capture.isNull() || !layout.isValid())
        return {};
    QImage result = ScreenshotResultCompositor::compose(capture.copy(layout.contentRect),
                                                        outputStyle, 1.0, 1.0, capture);
    if (result.isNull())
        return {};
    const auto backdrop = result.copy(layout.contentRect);
    QImage annotated;
    if (plan && plan->changed) {
        QList<CanvasExportTile> tiles;
        tiles.reserve(plan->tiles.size());
        for (const auto& tile : plan->tiles)
            tiles.append({tile.source, tile.destination});
        annotated = runtime.renderTilesToImage(selection, pixelSize, plan->outputSize, tiles,
                                               sources, backdrop);
    } else {
        annotated = runtime.renderToImage(selection, pixelSize, sources, backdrop);
    }
    if (annotated.isNull())
        return {};
    QPainter painter(&result);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.drawImage(layout.contentRect.topLeft(), annotated);
    return result;
}
