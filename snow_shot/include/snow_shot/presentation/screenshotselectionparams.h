#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONPARAMS_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONPARAMS_H

#include "snow_shot/image/screenshotlayoutcompaction.h"

#include <QColor>
#include <QRect>
#include "snow_shot/image/screenshotregiongeometry.h"
#include <optional>
#include <QString>
#include <QVector>

struct ScreenshotSelectionParams {
    QRect selection;
    int radius = 0;
    int shadowWidth = 0;
    QColor shadowColor = QColor(0x33, 0x33, 0x33);
    bool lockAspectRatio = false;
    bool lockDragAspectRatio = false;
    // Absent for legacy rectangles and named presets; present geometry is authoritative.
    std::optional<ScreenshotRegionGeometry> region;
    ScreenshotLayoutCompaction layoutCompaction = {};
    int backgroundOpacityPercent = 0;

    bool operator==(const ScreenshotSelectionParams& other) const {
        return selection == other.selection && radius == other.radius &&
               shadowWidth == other.shadowWidth && shadowColor == other.shadowColor &&
               lockAspectRatio == other.lockAspectRatio &&
               lockDragAspectRatio == other.lockDragAspectRatio && region == other.region &&
               layoutCompaction == other.layoutCompaction &&
               backgroundOpacityPercent == other.backgroundOpacityPercent;
    }

    bool operator!=(const ScreenshotSelectionParams& other) const {
        return !(*this == other);
    }
};

struct ScreenshotSelectionPreset {
    QString name;
    ScreenshotSelectionParams params;

    bool operator==(const ScreenshotSelectionPreset& other) const {
        return name == other.name && params == other.params;
    }

    bool operator!=(const ScreenshotSelectionPreset& other) const {
        return !(*this == other);
    }
};

[[nodiscard]] ScreenshotSelectionParams
clampScreenshotSelectionParams(const ScreenshotSelectionParams& params, const QRect& bounds);

// Restore commands must validate available geometry before replacing the current selection.
[[nodiscard]] std::optional<ScreenshotSelectionParams>
clampScreenshotSelectionParamsForRestore(const ScreenshotSelectionParams& params,
                                         const QRect& bounds);

[[nodiscard]] QVector<ScreenshotSelectionPreset>
sanitizeScreenshotSelectionPresets(const QVector<ScreenshotSelectionPreset>& presets,
                                   const QRect& bounds);

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONPARAMS_H
