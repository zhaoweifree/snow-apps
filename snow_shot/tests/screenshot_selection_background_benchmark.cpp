#include "snow_shot/presentation/screenshotselectionbackground.h"
#include "snow_shot/presentation/screenshotselectionshadowrenderer.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <iostream>
#include <vector>
#include <cstdlib>
#include <new>

namespace allocations {
// Qt malloc and managed pixel buffers are excluded; raster and shadow storage
// are reported separately so metadata allocations do not masquerade as images.
thread_local bool enabled = false;
thread_local std::size_t calls = 0;
thread_local std::size_t bytes = 0;
void* allocate(std::size_t size) {
    for (;;) {
        if (void* pointer = std::malloc(std::max<std::size_t>(size, 1))) {
            if (enabled) {
                ++calls;
                bytes += size;
            }
            return pointer;
        }
        if (const auto handler = std::get_new_handler())
            handler();
        else
            throw std::bad_alloc();
    }
}
} // namespace allocations
void* operator new(std::size_t size) {
    return allocations::allocate(size);
}
void* operator new[](std::size_t size) {
    return allocations::allocate(size);
}
void operator delete(void* pointer) noexcept {
    std::free(pointer);
}
void operator delete[](void* pointer) noexcept {
    std::free(pointer);
}
void operator delete(void* pointer, std::size_t) noexcept {
    std::free(pointer);
}
void operator delete[](void* pointer, std::size_t) noexcept {
    std::free(pointer);
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
#ifndef NDEBUG
    std::cerr << "Run this benchmark using the performance preset (Release).\n";
    return 1;
#else
    QJsonArray results;
    for (bool compact : {false, true}) {
        const QSize size = compact ? QSize(100000, 80000) : QSize(3840, 2160);
        const QSize contentSize = compact ? QSize(256, 256) : QSize(1600, 1500);
        const ScreenshotRegionGeometry region =
            QRegion(QRect(QPoint(), contentSize)) +
            QRect(QPoint(size.width() - contentSize.width(), size.height() - contentSize.height()),
                  contentSize);
        ScreenshotResultStyle style{24, 16, QColor(30, 30, 30, 180), region};
        const auto plan = makeScreenshotLayoutCompactionPlan(region, size, 1, {compact, 12}, 16);
        auto outputStyle = compactedScreenshotResultStyle(style, plan);
        QImage content(plan.outputSize, QImage::Format_ARGB32_Premultiplied);
        content.fill(QColor(40, 100, 180));
        QImage source(compact ? QSize(288, 288) : size + QSize(32, 32),
                      QImage::Format_ARGB32_Premultiplied);
        source.fill(QColor(50, 120, 190));
        QList<CanvasExportSource> sources{{source, QRectF(QPointF(-16, -16), source.size())}};
        if (compact)
            sources.append(
                {source, QRectF(QPointF(size.width() - 272, size.height() - 272), source.size())});
        for (int opacity : {-1, 0, 1, 50, 100}) {
            outputStyle.backgroundOpacityPercent = std::max(0, opacity);
            std::vector<double> samples;
            qsizetype bytes = 0;
            std::size_t allocationCalls = 0, allocationBytes = 0;
            for (int iteration = 0; iteration < 45; ++iteration) {
                QElapsedTimer timer;
                allocations::calls = allocations::bytes = 0;
                allocations::enabled = true;
                timer.start();
                const auto background = opacity < 0 ? QImage{}
                                                    : renderScreenshotSelectionBackground(
                                                          sources, QRectF(QPointF(), QSizeF(size)),
                                                          size, outputStyle, &plan);
                const auto result =
                    ScreenshotResultCompositor::compose(content, outputStyle, 1, 1, background);
                if (result.isNull())
                    return 1;
                bytes = result.sizeInBytes() + background.sizeInBytes();
                allocations::enabled = false;
                if (iteration >= 5) {
                    samples.push_back(timer.nsecsElapsed() / 1000000.0);
                    allocationCalls += allocations::calls;
                    allocationBytes += allocations::bytes;
                }
            }
            std::sort(samples.begin(), samples.end());
            results.append(
                QJsonObject{{QStringLiteral("scenario"), compact ? "sparse-compacted" : "4k-multi"},
                            {QStringLiteral("opacity_percent"), opacity},
                            {QStringLiteral("baseline_without_background_call"), opacity < 0},
                            {QStringLiteral("median_ms"), samples[samples.size() / 2]},
                            {QStringLiteral("p95_ms"), samples[samples.size() * 95 / 100]},
                            {QStringLiteral("raster_bytes"), static_cast<qint64>(bytes)},
                            {QStringLiteral("cpp_allocation_calls_per_export"),
                             double(allocationCalls) / samples.size()},
                            {QStringLiteral("cpp_allocation_bytes_per_export"),
                             double(allocationBytes) / samples.size()},
                            {QStringLiteral("shadow_scratch_peak_bytes"),
                             static_cast<qint64>(
                                 ScreenshotSelectionShadowRenderer::diagnosticsForCurrentThread()
                                     .regionScratchPeakBytes)},
                            {QStringLiteral("shadow_cache_bytes"),
                             static_cast<qint64>(
                                 ScreenshotSelectionShadowRenderer::diagnosticsForCurrentThread()
                                     .regionCacheRetainedBytes)}});
        }
    }
    std::cout << QJsonDocument(results).toJson().constData();
    return 0;
#endif
}
