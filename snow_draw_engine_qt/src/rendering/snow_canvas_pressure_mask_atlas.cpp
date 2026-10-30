#include "snow_canvas_pressure_mask_atlas.h"

#include "snow_canvas_display_item.h"

#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QTransform>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <list>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace snow_canvas_pressure_mask {
namespace {

struct Key {
    const void* owner = nullptr;
    std::uint32_t elementIndex = 0;
    std::uint32_t elementGeneration = 0;
    std::array<std::uint64_t, 9> transform{};
    int x = 0;
    int y = 0;
    std::uint64_t chunkId = 0;
    std::uint64_t cacheId = 0;
    QRgb color = 0;
    bool isUnion = false;

    bool operator==(const Key& other) const {
        return owner == other.owner && elementIndex == other.elementIndex &&
               elementGeneration == other.elementGeneration && transform == other.transform &&
               x == other.x && y == other.y && chunkId == other.chunkId &&
               cacheId == other.cacheId && color == other.color && isUnion == other.isUnion;
    }
};

struct KeyHash {
    std::size_t operator()(const Key& key) const {
        std::size_t hash = std::hash<const void*>{}(key.owner);
        const auto append = [&hash](std::uint64_t value) {
            hash ^= static_cast<std::size_t>(value) + std::size_t{0x9e3779b9} + (hash << 6) +
                    (hash >> 2);
        };
        append(key.elementIndex);
        append(key.elementGeneration);
        for (const auto value : key.transform)
            append(value);
        append(static_cast<std::uint32_t>(key.x));
        append(static_cast<std::uint32_t>(key.y));
        append(key.chunkId);
        append(key.cacheId);
        append(key.color);
        append(key.isUnion ? 1u : 0u);
        return hash;
    }
};

struct Entry {
    QImage image;
    std::vector<std::uint64_t> chunkIds;
    std::list<Key>::iterator lru;
    std::size_t bytes = 0;
};

class Atlas {
  public:
    std::shared_ptr<const Entry> find(const Key& key) {
        const auto found = entries.find(key);
        if (found == entries.end())
            return {};
        auto& order = key.isUnion ? unionOrder : chunkOrder;
        order.splice(order.begin(), order, found->second->lru);
        return found->second;
    }

    void remove(const Key& key) {
        const auto found = entries.find(key);
        if (found == entries.end())
            return;
        bytes -= found->second->bytes;
        if (key.isUnion)
            unionBytes -= found->second->bytes;
        auto& order = key.isUnion ? unionOrder : chunkOrder;
        order.erase(found->second->lru);
        entries.erase(found);
    }

    void insert(const Key& key, const QImage& image, std::vector<std::uint64_t> chunkIds,
                Diagnostics& diagnostics) {
        if (budget == 0 || image.isNull())
            return;
        const std::size_t size = static_cast<std::size_t>(image.sizeInBytes()) +
                                 chunkIds.capacity() * sizeof(std::uint64_t) + sizeof(Entry) +
                                 sizeof(Key) * 2 + 256;
        if (size > budget)
            return;
        remove(key);
        auto& order = key.isUnion ? unionOrder : chunkOrder;
        order.push_front(key);
        auto entry = std::make_shared<Entry>();
        entry->image = image;
        entry->chunkIds = std::move(chunkIds);
        entry->lru = order.begin();
        entry->bytes = size;
        entries.emplace(key, std::move(entry));
        bytes += size;
        if (key.isUnion)
            unionBytes += size;
        trim(diagnostics);
    }

    void trim(Diagnostics& diagnostics) {
        while (bytes > budget && (!chunkOrder.empty() || !unionOrder.empty())) {
            // A full repaint touches many overlapping immutable masks while its colored tiles
            // are already complete. Protect union tiles within half the total budget so mask
            // churn cannot evict the next unchanged tile before the repaint reaches it.
            // Chunk masks may consume every byte not currently needed by this reservation.
            const bool evictUnion =
                !unionOrder.empty() && (unionBytes > budget / 2 || chunkOrder.empty());
            const Key key = evictUnion ? unionOrder.back() : chunkOrder.back();
            remove(key);
            ++diagnostics.evictions;
        }
    }

    void clear() {
        entries.clear();
        chunkOrder.clear();
        unionOrder.clear();
        bytes = 0;
        unionBytes = 0;
    }

    std::size_t budget = kDefaultBudgetBytes;
    std::size_t bytes = 0;
    std::size_t unionBytes = 0;
    std::unordered_map<Key, std::shared_ptr<Entry>, KeyHash> entries;
    std::list<Key> chunkOrder;
    std::list<Key> unionOrder;
};

thread_local Atlas atlas;
thread_local Diagnostics totals;

void accumulate(Diagnostics& destination, const Diagnostics& source) {
    destination.queriedTiles += source.queriedTiles;
    destination.queriedChunks += source.queriedChunks;
    destination.rasterizedMasks += source.rasterizedMasks;
    destination.hits += source.hits;
    destination.misses += source.misses;
    destination.unionTileHits += source.unionTileHits;
    destination.unionRebuilds += source.unionRebuilds;
    destination.unionCopies += source.unionCopies;
    destination.unionCopyBytes += source.unionCopyBytes;
    destination.drawnTiles += source.drawnTiles;
    destination.evictions += source.evictions;
    destination.retainedBytes = atlas.bytes;
    destination.entries = atlas.entries.size();
}

std::array<std::uint64_t, 9> transformBits(const QTransform& transform) {
    const std::array<qreal, 9> values{transform.m11(), transform.m12(), transform.m13(),
                                      transform.m21(), transform.m22(), transform.m23(),
                                      transform.m31(), transform.m32(), transform.m33()};
    std::array<std::uint64_t, 9> bits{};
    static_assert(sizeof(qreal) == sizeof(std::uint64_t));
    for (std::size_t index = 0; index < values.size(); ++index)
        std::memcpy(&bits[index], &values[index], sizeof(qreal));
    return bits;
}

QRect physicalDeviceRect(const QPaintDevice& device) {
    if (const auto* image = dynamic_cast<const QImage*>(&device))
        return image->rect();
    if (const auto* pixmap = dynamic_cast<const QPixmap*>(&device))
        return pixmap->rect();
    const qreal dpr = device.devicePixelRatioF();
    return {0, 0, qRound(device.width() * dpr), qRound(device.height() * dpr)};
}

int tileCoordinate(int physicalCoordinate) {
    return static_cast<int>(std::floor(static_cast<double>(physicalCoordinate) / kTileSize));
}

std::uint64_t tileId(int x, int y) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) |
           static_cast<std::uint32_t>(y);
}

QImage rasterize(const SnowCanvasSceneItem::PathChunk& chunk, const QTransform& canvasToDevice,
                 int tileX, int tileY) {
    QImage mask(QSize(kTileSize, kTileSize), QImage::Format_Alpha8);
    mask.fill(0);
    QPainter painter(&mask);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setWorldTransform(canvasToDevice *
                              QTransform::fromTranslate(-tileX * kTileSize, -tileY * kTileSize));
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::white);
    if (chunk.canvasPath.fillRule() == Qt::WindingFill) {
        painter.drawPath(chunk.canvasPath);
    } else {
        QPainterPath winding = chunk.canvasPath;
        winding.setFillRule(Qt::WindingFill);
        painter.drawPath(winding);
    }
    return mask;
}

QImage colorize(const QImage& alpha, const QColor& stroke) {
    QImage colored(alpha.size(), QImage::Format_ARGB32_Premultiplied);
    const int red = stroke.red();
    const int green = stroke.green();
    const int blue = stroke.blue();
    const int opacity = stroke.alpha();
    for (int y = 0; y < alpha.height(); ++y) {
        const auto* source = alpha.constScanLine(y);
        auto* destination = reinterpret_cast<QRgb*>(colored.scanLine(y));
        for (int x = 0; x < alpha.width(); ++x) {
            const int value = (source[x] * opacity + 127) / 255;
            destination[x] = qPremultiply(qRgba(red, green, blue, value));
        }
    }
    return colored;
}

QRegion conservativePhysicalClip(const QPainter& painter, const QTransform& canvasToDevice) {
    if (!painter.hasClipping())
        return QRegion(physicalDeviceRect(*painter.device()));
    // clipRegion() quantizes the inverse camera transform before returning its integer logical
    // rectangles. Mapping those integers back loses device edge strips at fractional placement.
    // The precise clip path retains the original edges; outward bounds per subpath preserve
    // sparse rectangles while leaving the painter's original device clip responsible for pixels.
    const auto path = canvasToDevice.map(painter.clipPath());
    QRegion region;
    for (const auto& polygon : path.toSubpathPolygons())
        region += polygon.boundingRect().adjusted(-1e-7, -1e-7, 1e-7, 1e-7).toAlignedRect();
    return region;
}

} // namespace

void draw(QPainter& canvasPainter, const SnowCanvasSceneItem& item, const QColor& stroke,
          const DrawOptions& options) {
    if (stroke.alpha() == 0 || item.pressurePathChunks().empty() ||
        canvasPainter.device() == nullptr)
        return;
    const QTransform canvasToDevice = canvasPainter.deviceTransform();
    bool invertible = false;
    const QTransform deviceToCanvas = canvasToDevice.inverted(&invertible);
    if (!invertible)
        return;
    QRegion clip = options.physicalClip != nullptr
                       ? *options.physicalClip
                       : conservativePhysicalClip(canvasPainter, canvasToDevice);
    clip &= physicalDeviceRect(*canvasPainter.device());
    clip &=
        canvasToDevice.mapRect(item.pressureCanvasBounds()).adjusted(-1, -1, 1, 1).toAlignedRect();
    if (clip.isEmpty())
        return;
    Diagnostics diagnostics;
    Key base;
    base.owner =
        options.cacheNamespace != nullptr ? options.cacheNamespace : canvasPainter.device();
    base.elementIndex = item.element_id.index;
    base.elementGeneration = item.element_id.generation;
    base.transform = transformBits(canvasToDevice);
    const auto& chunks = item.pressurePathChunks();
    std::unordered_set<std::uint64_t> visited;
    std::vector<std::uint32_t> candidates;
    canvasPainter.save();
    // Keep the caller's viewport/window and device clip intact. Cancel their remaining base
    // device transform so cached DPR=1 pixels are positioned in physical device coordinates.
    canvasPainter.setWorldTransform(QTransform());
    const QTransform baseToDevice = canvasPainter.deviceTransform();
    canvasPainter.setWorldTransform(baseToDevice.inverted());
    if (options.physicalClip != nullptr)
        canvasPainter.setClipRegion(*options.physicalClip, Qt::IntersectClip);
    canvasPainter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    for (const QRect& rect : clip) {
        for (int tileY = tileCoordinate(rect.top()); tileY <= tileCoordinate(rect.bottom());
             ++tileY) {
            for (int tileX = tileCoordinate(rect.left()); tileX <= tileCoordinate(rect.right());
                 ++tileX) {
                if (!visited.insert(tileId(tileX, tileY)).second)
                    continue;
                ++diagnostics.queriedTiles;
                const QRect physicalTile(tileX * kTileSize, tileY * kTileSize, kTileSize,
                                         kTileSize);
                const QRectF canvasTile =
                    deviceToCanvas.mapRect(QRectF(physicalTile).adjusted(-1, -1, 1, 1))
                        .adjusted(-1e-6, -1e-6, 1e-6, 1e-6);
                item.queryPressurePathChunks(canvasTile, &candidates);
                diagnostics.queriedChunks += candidates.size();
                if (candidates.empty())
                    continue;
                std::vector<std::uint64_t> chunkIds;
                chunkIds.reserve(candidates.size() * 2);
                for (const auto index : candidates) {
                    chunkIds.push_back(chunks[index].stableId);
                    chunkIds.push_back(chunks[index].cacheId);
                }
                Key unionKey = base;
                unionKey.x = tileX;
                unionKey.y = tileY;
                unionKey.isUnion = true;
                unionKey.color = stroke.rgba();
                const auto cached = options.cacheEnabled ? atlas.find(unionKey) : nullptr;
                QImage colored;
                if (cached != nullptr && cached->chunkIds == chunkIds) {
                    colored = cached->image;
                    ++diagnostics.unionTileHits;
                } else {
                    if (options.cacheEnabled)
                        atlas.remove(unionKey);
                    QImage alpha(QSize(kTileSize, kTileSize), QImage::Format_Alpha8);
                    alpha.fill(0);
                    for (const auto index : candidates) {
                        Key chunkKey = base;
                        chunkKey.x = tileX;
                        chunkKey.y = tileY;
                        chunkKey.chunkId = chunks[index].stableId;
                        chunkKey.cacheId = chunks[index].cacheId;
                        const auto cachedChunk =
                            options.cacheEnabled ? atlas.find(chunkKey) : nullptr;
                        QImage mask;
                        if (cachedChunk != nullptr) {
                            mask = cachedChunk->image;
                            ++diagnostics.hits;
                        } else {
                            mask = rasterize(chunks[index], canvasToDevice, tileX, tileY);
                            ++diagnostics.misses;
                            ++diagnostics.rasterizedMasks;
                            if (options.cacheEnabled)
                                atlas.insert(chunkKey, mask, {}, diagnostics);
                        }
                        unsigned char coverageBits = 255;
                        for (int y = 0; y < kTileSize; ++y) {
                            auto* destination = alpha.scanLine(y);
                            const auto* source = mask.constScanLine(y);
                            for (int x = 0; x < kTileSize; ++x) {
                                destination[x] = std::max(destination[x], source[x]);
                                coverageBits &= destination[x];
                            }
                        }
                        ++diagnostics.unionCopies;
                        diagnostics.unionCopyBytes += kTileSize * kTileSize;
                        // MAX cannot change a completely opaque tile. Keep the full candidate
                        // ID list above so even skipped chunks still invalidate this union.
                        if (coverageBits == 255)
                            break;
                    }
                    colored = colorize(alpha, stroke);
                    ++diagnostics.unionRebuilds;
                    if (options.cacheEnabled)
                        atlas.insert(unionKey, colored, std::move(chunkIds), diagnostics);
                }
                canvasPainter.drawImage(physicalTile.topLeft(), colored);
                ++diagnostics.drawnTiles;
            }
        }
    }
    canvasPainter.restore();
    accumulate(totals, diagnostics);
    if (options.diagnostics != nullptr)
        accumulate(*options.diagnostics, diagnostics);
}

void clearCacheForCurrentThread() {
    atlas.clear();
    totals = {};
}

void setCacheByteBudgetForCurrentThread(std::size_t byteBudget) {
    atlas.budget = byteBudget;
    atlas.trim(totals);
    totals.retainedBytes = atlas.bytes;
    totals.entries = atlas.entries.size();
}

Diagnostics diagnosticsForCurrentThread() {
    auto result = totals;
    result.retainedBytes = atlas.bytes;
    result.entries = atlas.entries.size();
    return result;
}

void resetDiagnosticsForCurrentThread() {
    totals = {};
}

} // namespace snow_canvas_pressure_mask
