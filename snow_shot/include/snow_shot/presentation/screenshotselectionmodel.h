#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONMODEL_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONMODEL_H

#include "snow_shot/presentation/screenshotselectionaspectratio.h"
#include "snow_shot/presentation/screenshotselectiongeometry.h"
#include "snow_shot/presentation/screenshotselectionparams.h"
#include "snow_shot/presentation/screenshotresultcompositor.h"
#include "snow_draw_engine_qt/snow_canvas_snapping.h"

#include <QColor>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QtGlobal>

class ScreenshotSelectionModel final {
  public:
    enum class RegionOperation { Replace, Add, Subtract };
    ScreenshotSelectionModel() = default;
    ScreenshotSelectionModel(const ScreenshotSelectionModel&) = default;
    ScreenshotSelectionModel(ScreenshotSelectionModel&&) = default;
    ScreenshotSelectionModel& operator=(const ScreenshotSelectionModel& other);
    ScreenshotSelectionModel& operator=(ScreenshotSelectionModel&& other);

    [[nodiscard]] quint64 changeRevision() const {
        return m_changeRevision;
    }
    void reset();
    ScreenshotRegionType regionType() const {
        return m_regionType;
    }
    void setRegionType(ScreenshotRegionType type);
    bool constructionActive() const {
        return m_draftRegion.has_value();
    }
    bool cornerRadiusApplicable() const {
        return !selectionRegion().custom();
    }
    void setDraftRegion(const ScreenshotRegionGeometry& region,
                        const QVector<QPointF>& vertices = {});
    QPainterPath draftPath() const {
        return m_draftRegion ? m_draftRegion->path() : QPainterPath();
    }
    const QVector<QPointF>& draftVertices() const {
        return m_draftVertices;
    }
    void clearDraftRegion();
    void commitDraftRegion(const QRect& canvasBounds = {});
    [[nodiscard]] ScreenshotRegionGeometry selectionRegion() const;
    // Compose a displayed smart-selection frame without changing the capture operand.
    [[nodiscard]] ScreenshotRegionGeometry selectionRegionForMarquee(const QRectF& marquee) const;
    [[nodiscard]] ScreenshotRegionGeometry confirmedRegion() const;
    [[nodiscard]] bool rectangular() const;
    [[nodiscard]] bool multiRegionEditing() const {
        return m_multiRegionEditing;
    }
    [[nodiscard]] bool resizable() const {
        return !m_multiRegionEditing && rectangular();
    }
    [[nodiscard]] qsizetype createdRegionCount() const {
        return m_createdRegions.size();
    }
    [[nodiscard]] std::optional<quint64> regionAt(const QPointF& position) const;
    bool setHoveredRegion(const QPointF& position);
    [[nodiscard]] std::optional<ScreenshotRegionGeometry> highlightedRegion() const;
    [[nodiscard]] std::optional<ScreenshotRegionGeometry> stationaryRegionsDuringMove() const {
        return m_regionMove ? std::optional(m_regionMove->stationary) : std::nullopt;
    }
    [[nodiscard]] bool duplicateLatestRegion(const QPointF& center, const QRect& bounds);
#if defined(SNOW_SHOT_BENCH_INTERNALS)
    [[nodiscard]] quint64 regionEditCompositionsForTesting() const {
        return m_regionEditCompositions;
    }
    // Conservative estimate: shared immutable storage may be counted more than once.
    [[nodiscard]] qsizetype regionStorageBytesForTesting() const {
        qsizetype bytes = m_createdRegions.capacity() * qsizetype(sizeof(CreatedRegion));
        for (const auto& region : m_createdRegions)
            bytes += region.geometry.retainedBytesEstimate();
        if (m_regionMove)
            bytes += m_regionMove->stationary.retainedBytesEstimate() +
                     m_regionMove->original.retainedBytesEstimate() +
                     m_regionMove->preview.retainedBytesEstimate();
        return bytes;
    }
#endif
    [[nodiscard]] bool regionOperationActive() const;
    [[nodiscard]] RegionOperation regionOperation() const;
    [[nodiscard]] QRectF pendingMarquee() const;
    void beginRegionOperation(RegionOperation operation);
    void commitRegionOperation();
    void cancelRegionOperation();
    void setSelectionRegion(const ScreenshotRegionGeometry& region);
    void setDraggedSelectionRect(const QRectF& rect, ScreenshotSelectionDragMode mode);
    [[nodiscard]] ScreenshotResultStyle resultStyle() const;

    [[nodiscard]] QRectF normalizedSelection() const;
    [[nodiscard]] QRect pixelSelection() const;
    [[nodiscard]] bool hasPixelSelection() const;

    void clearSelection();
    // Stores a selection rectangle as provided. Detection sources (intelligent selection,
    // persisted params) may provide sub-pixel rects that pixel conversion rounds outward.
    // Pointer drags must use setDraggedSelectionRect with the shared drag geometry
    // (draggedScreenshotSelectionRect / grabAdjustedScreenshotSelectionRect) so confirmation
    // preserves any gesture-specific aspect ratio.
    // Returns whether the displayed geometry changed, including replacement of a custom region.
    bool setSelectionRect(const QRectF& selection);
    // Stores a selection spanning the pointer cells under the press and
    // release positions: both cells are inclusive, exactly like a marquee
    // drag, so pointer-seeded state always addresses whole canvas pixels.
    void setSelectionStartEnd(const QPointF& start, const QPointF& end);

    // Multi-region drags normally target the hit region; the movement shortcut targets all.
    void beginMoveDrag(const QPointF& startPosition, bool entireSelection = false);
    // Rebase a move/resize gesture without changing the visible selection.
    // This is used when a transient modifier switches an active resize into
    // moving the whole selection, avoiding a cursor jump.
    void rebaseMoveDrag(const QPointF& startPosition, bool entireSelection = false);
    void finishMoveDrag();
    QRectF snapIndividualRegionMove(const QRectF& candidate, const QRectF& bounds,
                                    qreal canvasToViewScale, bool enabled);
    void clearMoveSnapGuides();
    const QVector<SnowCanvasSnapGuide>& moveSnapGuides() const {
        return m_moveSnapGuides;
    }
    [[nodiscard]] QRectF moveOriginalSelection() const;
    [[nodiscard]] QRectF selectionRectForDrag(ScreenshotSelectionDragMode dragMode,
                                              const QPointF& position, const QRectF& bounds,
                                              qreal minimumSelectionSize,
                                              qreal lockedAspectRatioOverride = -1.0) const;

    [[nodiscard]] QRectF boundedSelectionRect(const QRectF& selection, const QRectF& bounds,
                                              bool preserveSize, qreal minimumSelectionSize) const;
    [[nodiscard]] bool adjustFromToolbar(int minDx, int minDy, int maxDx, int maxDy,
                                         const QRectF& bounds, qreal minimumSelectionSize);

    [[nodiscard]] bool layoutCompactionAvailable() const;
    [[nodiscard]] ScreenshotLayoutCompaction layoutCompaction() const {
        return m_layoutCompaction;
    }
    [[nodiscard]] ScreenshotLayoutCompaction exportLayoutCompaction() const {
        auto options = m_layoutCompaction;
        options.enabled = options.enabled && layoutCompactionAvailable();
        return options;
    }
    bool setLayoutCompaction(ScreenshotLayoutCompaction options);
    [[nodiscard]] int backgroundOpacityPercent() const {
        return m_backgroundOpacityPercent;
    }
    bool setBackgroundOpacityPercent(int percent);
    [[nodiscard]] int cornerRadius() const;
    [[nodiscard]] int shadowWidth() const;
    [[nodiscard]] QColor shadowColor() const;
    [[nodiscard]] bool aspectRatioLocked() const;
    [[nodiscard]] ScreenshotSelectionAspectRatioPreset aspectRatioPreset() const;

    [[nodiscard]] bool setCornerRadius(int radius);
    [[nodiscard]] bool setShadowWidth(int shadowWidth);
    void setShadowColor(const QColor& color);
    [[nodiscard]] bool setAspectRatioLockEnabled(bool enabled, qreal minimumSelectionSize);
    void toggleAspectRatioLock(qreal minimumSelectionSize);
    [[nodiscard]] bool setAspectRatioPreset(ScreenshotSelectionAspectRatioPreset preset,
                                            const QRectF& bounds, qreal minimumSelectionSize);
    // Commit a drag's already-constrained geometry without reshaping its preview.
    [[nodiscard]] bool setDraggedAspectRatioPreset(ScreenshotSelectionAspectRatioPreset preset);
    [[nodiscard]] bool finalizeAspectRatio(const QRectF& bounds, qreal minimumSelectionSize);
    // Exact incoming geometry remains authoritative without losing a custom lock.
    void clearAspectRatioPresetForReplacement();

    [[nodiscard]] ScreenshotSelectionParams params(const QRect& bounds) const;
    [[nodiscard]] bool applyParams(const ScreenshotSelectionParams& params, const QRect& bounds);

  private:
    class ChangeRevisionGuard;
    [[nodiscard]] bool hasSameSelectionState(const ScreenshotSelectionModel& other) const;
    [[nodiscard]] bool hasSameCreatedRegions(const ScreenshotSelectionModel& other) const;
    // Store derived geometry without replacing the editable region collection.
    void storeRegionGeometry(const ScreenshotRegionGeometry& region);
    void beginRegionMove(qsizetype index);
    void clearCreatedRegions();
    void appendCreatedRegion(const ScreenshotRegionGeometry& region);
    struct CreatedRegion {
        quint64 id;
        ScreenshotRegionGeometry geometry;
        QRect bounds;
        bool operator==(const CreatedRegion&) const = default;
    };
    struct RegionMove {
        qsizetype index;
        ScreenshotRegionGeometry original;
        ScreenshotRegionGeometry stationary;
        ScreenshotRegionGeometry preview;
        QPoint offset;
        QVector<QRectF> snapReferences;
    };
    quint64 m_regionEditCompositions = 0;
    QVector<CreatedRegion> m_createdRegions;
    bool m_multiRegionEditing = false;
    bool m_multiRegionBeforeOperation = false;
    quint64 m_nextRegionId = 1;
    std::optional<quint64> m_hoveredRegion;
    std::optional<RegionMove> m_regionMove;
    QVector<SnowCanvasSnapGuide> m_moveSnapGuides;
    bool m_moveEntireSelection = false;
    std::optional<qsizetype> m_regionBeforeEntireMove;

    struct MarqueeRegion {
        QRect pixels;
        ScreenshotRegionGeometry region;
    };
    QVector<QPointF> m_draftVertices;
    // Keep the capture target pinned while animation replaces its displayed frame.
    // Both caches belong to the current confirmed-region/operation transaction.
    mutable std::optional<ScreenshotRegionGeometry> m_cachedSelectionRegion;
    mutable std::optional<MarqueeRegion> m_cachedMarqueeRegion;
    ScreenshotRegionType m_regionType = ScreenshotRegionType::Rectangle;
    std::optional<ScreenshotRegionGeometry> m_draftRegion;
    std::optional<ScreenshotRegionGeometry> m_region;
    ScreenshotRegionGeometry m_confirmedRegion;
    ScreenshotRegionGeometry m_moveOriginalRegion;
    RegionOperation m_regionOperation = RegionOperation::Replace;
    QPointF m_start;
    QPointF m_end;
    QPointF m_moveStart;
    QRectF m_moveOriginalSelection;
    ScreenshotLayoutCompaction m_layoutCompaction;
    mutable std::optional<ScreenshotRegionGeometry> m_componentGeometry;
    mutable bool m_multipleComponents = false;
    int m_backgroundOpacityPercent = 0;
    int m_cornerRadius = 0;
    int m_shadowWidth = 0;
    QColor m_shadowColor = QColor(0x33, 0x33, 0x33);
    bool m_aspectRatioLockEnabled = false;
    ScreenshotSelectionAspectRatioPreset m_aspectRatioPreset =
        ScreenshotSelectionAspectRatioPreset::Free;
    // Pointer drags already resolve their ratio; confirmation must preserve their geometry.
    bool m_selectionFromDrag = false;
    double m_lockedAspectRatio = 0.0;
    // Replacements preserve the destination's revision, including candidate-copy validation.
    quint64 m_changeRevision = 0;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONMODEL_H
