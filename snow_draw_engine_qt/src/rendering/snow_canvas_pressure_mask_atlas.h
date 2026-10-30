#pragma once

#include <QColor>
#include <QRegion>

#include <cstddef>

class QPainter;
class SnowCanvasSceneItem;

namespace snow_canvas_pressure_mask {

inline constexpr int kTileSize = 64;
inline constexpr std::size_t kDefaultBudgetBytes = 16u * 1024u * 1024u;

struct Diagnostics {
    std::size_t queriedTiles = 0;
    std::size_t queriedChunks = 0;
    std::size_t rasterizedMasks = 0;
    std::size_t hits = 0;
    std::size_t misses = 0;
    std::size_t unionTileHits = 0;
    std::size_t unionRebuilds = 0;
    std::size_t unionCopies = 0;
    std::size_t unionCopyBytes = 0;
    std::size_t drawnTiles = 0;
    std::size_t evictions = 0;
    std::size_t retainedBytes = 0;
    std::size_t entries = 0;
};

struct DrawOptions {
    const void* cacheNamespace = nullptr;
    bool cacheEnabled = true;
    // Optional sparse clip in physical device coordinates. Without it, use the painter's
    // precise clip path through deviceTransform(), including its DPR/viewport. Default queries
    // use outward subpath bounds and preserve the original painter clip for final pixels.
    const QRegion* physicalClip = nullptr;
    Diagnostics* diagnostics = nullptr;
};

// The painter is already in canvas coordinates. Immutable chunk coverage and colored union
// tiles share one thread-local LRU budget across every canvas/element on the rendering thread.
void draw(QPainter& canvasPainter, const SnowCanvasSceneItem& item, const QColor& stroke,
          const DrawOptions& options = {});
void clearCacheForCurrentThread();
void setCacheByteBudgetForCurrentThread(std::size_t byteBudget);
Diagnostics diagnosticsForCurrentThread();
void resetDiagnosticsForCurrentThread();

} // namespace snow_canvas_pressure_mask
