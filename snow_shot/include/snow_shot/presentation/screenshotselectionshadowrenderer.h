#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONSHADOWRENDERER_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONSHADOWRENDERER_H

#include <QColor>
#include <QImage>
#include <QRectF>

#include <cstddef>
#include <memory>

class QPainter;
class QRegion;
class QTransform;
class QWidget;
class ScreenshotRegionGeometry;

struct ScreenshotSelectionShadowDiagnostics {
    std::size_t cacheHits = 0;
    std::size_t cacheBuilds = 0;
    std::size_t retainedBytes = 0;
    std::size_t retainedEntries = 0;
    std::size_t selectionSizedTransientAllocations = 0;
    std::size_t regionMaskBuilds = 0;
    std::size_t regionShadowBuilds = 0;
    std::size_t regionStripBuilds = 0;
    std::size_t regionScratchPeakBytes = 0;
    std::size_t regionCacheRetainedBytes = 0;
    std::size_t regionPathCacheElements = 0;
};

class ScreenshotSelectionShadowRenderer final {
  public:
    static void renderPreview(QPainter& painter, const QRectF& selectionBounds, qreal cornerRadius,
                              qreal shadowWidth, const QColor& shadowColor, qreal devicePixelRatio,
                              const QWidget* widget = nullptr);

    static QImage composeExport(const QImage& content, int cornerRadius, int shadowWidth,
                                const QColor& shadowColor);

    static void renderResultShadow(QPainter& painter, const QRectF& contentBounds,
                                   qreal cornerRadius, qreal shadowWidth, const QColor& shadowColor,
                                   qreal devicePixelRatio);

    static ScreenshotSelectionShadowDiagnostics diagnosticsForCurrentThread();
    static void resetDiagnosticsForCurrentThread();
    // Clears rectangle shadow assets, rounded region contours, and export masks/shadows.
    // Must run on each owning thread after its session/export work has finished.
    static void resetCacheForCurrentThread();
};

// Session-owned, canvas-resolution shadow tiles. Raster sampling matches the
// export compositor; view transforms only position/scale those source pixels.
class ScreenshotRegionShadowRenderer final {
  public:
    ScreenshotRegionShadowRenderer();
    ~ScreenshotRegionShadowRenderer();
    ScreenshotRegionShadowRenderer(const ScreenshotRegionShadowRenderer&) = delete;
    ScreenshotRegionShadowRenderer& operator=(const ScreenshotRegionShadowRenderer&) = delete;

    void render(QPainter& painter, const ScreenshotRegionGeometry& region, int cornerRadius,
                int shadowWidth, const QColor& shadowColor, const QTransform& canvasToView,
                const QRegion& exposedView);
    void reset();
    [[nodiscard]] std::size_t retainedBytes() const;

  private:
    struct Cache;
    std::unique_ptr<Cache> m_cache;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONSHADOWRENDERER_H
