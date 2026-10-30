#include "snow_shot/image/screenshotlayoutcompaction.h"
#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include <QMouseEvent>
#include "../../snow_draw_engine_qt/src/rendering/snow_canvas_export.h"
#include "../../test-support/memorysnapshot.h"

#include <QApplication>
#include <QElapsedTimer>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <vector>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    const int gap = argc > 1 ? std::atoi(argv[1]) : 4096;
    const int count = argc > 2 ? std::atoi(argv[2]) : 2;
    if (gap < 0 || gap > 1000000 || count < 2 || count > 8)
        return EXIT_FAILURE;
    constexpr int edge = 256;
    const QSize sourceSize(count * edge + (count - 1) * gap, count * edge + (count - 1) * gap);
    QRegion region;
    QList<CanvasExportSource> sources;
    QImage image(edge, edge, QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(43, 123, 219));
    for (int y = 0; y < count; ++y) {
        for (int x = 0; x < count; ++x) {
            const QRect rect(x * (edge + gap), y * (edge + gap), edge, edge);
            region += rect;
            sources.append({image, rect});
        }
    }
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    if (argc > 3) {
        canvas.resize(edge, edge);
        canvas.show();
        QCoreApplication::processEvents();
        if (!canvas.setViewportCamera(edge / 2, edge / 2, 1) ||
            !canvas.setCanvasTool(SnowCanvasTool::RectangleFilter))
            return EXIT_FAILURE;
        SnowCanvasFilterStyle style;
        style.type = SnowCanvasFilterType::GaussianBlur;
        style.strength = 0.2;
        if (!canvas.setCanvasFilterStyle(style, SnowCanvasFilterStylePropertyType |
                                                    SnowCanvasFilterStylePropertyStrength))
            return EXIT_FAILURE;
        for (auto type :
             {QEvent::MouseButtonPress, QEvent::MouseMove, QEvent::MouseButtonRelease}) {
            const QPointF point =
                type == QEvent::MouseButtonPress ? QPointF(20, 20) : QPointF(236, 236);
            QMouseEvent event(type, point, canvas.mapToGlobal(point.toPoint()),
                              type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                              type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                              Qt::NoModifier);
            QCoreApplication::sendEvent(&canvas, &event);
        }
        if (!canvas.canvasHistoryState().canUndo)
            return EXIT_FAILURE;
    }
    std::vector<double> samples;
    qsizetype outputBytes = 0;
    double planningMs = 0;
    for (int run = 0; run < 22; ++run) {
        QElapsedTimer timer;
        timer.start();
        const auto plan = makeScreenshotLayoutCompactionPlan(region, sourceSize, 1, {true, 0});
        QList<CanvasExportTile> tiles;
        for (const auto& tile : plan.tiles)
            tiles.append({tile.source, tile.destination});
        planningMs = static_cast<double>(timer.nsecsElapsed()) / 1000000.0;
        snow_canvas_export::resetDiagnosticsForCurrentThread();
        const auto result =
            plan.changed ? runtime.renderTilesToImage(QRect(QPoint(), sourceSize), sourceSize,
                                                      plan.outputSize, tiles, sources)
                         : runtime.renderToImage(QRect(QPoint(), sourceSize), sourceSize, sources);
        const double elapsed = static_cast<double>(timer.nsecsElapsed()) / 1000000.0;
        if (result.size() != QSize(count * edge, count * edge) ||
            result.pixelColor(result.width() - 1, result.height() - 1) != image.pixelColor(0, 0))
            return EXIT_FAILURE;
        outputBytes = result.sizeInBytes();
        if (run >= 2)
            samples.push_back(elapsed);
    }
    std::sort(samples.begin(), samples.end());
    const auto diagnostics = snow_canvas_export::diagnosticsForCurrentThread();
    const auto memory = snow::test_support::memorySnapshot();
    std::cout << "gap,regions,median_ms,p95_ms,output_raster_bytes,peak_scratch_pool_bytes,peak_"
                 "source_tile_bytes,"
                 "source_bounds_raster_bytes,process_peak_rss_bytes,planning_ms\n"
              << gap << ',' << count * count << ',' << samples[samples.size() / 2] << ','
              << samples[samples.size() - 2] << ',' << outputBytes << ','
              << diagnostics.peakScratchPoolBytes << ',' << diagnostics.peakSourceTileBytes << ','
              << qint64(sourceSize.width()) * sourceSize.height() * 4 << ','
              << memory.peakResidentBytes << ',' << planningMs << '\n';
}
