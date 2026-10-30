#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTORDISPLAYGEOMETRY_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTORDISPLAYGEOMETRY_H

#include "snow_shot/presentation/screenshottypes.h"

// Selector snapshots own only the frozen coordinate mapping. Capture pixels,
// including the separately owned cursor patch, stay with the capture session.
[[nodiscard]] inline CapturedDisplayModel
screenshotSelectorDisplayGeometry(const CapturedDisplayModel& display) {
    CapturedDisplayModel geometry;
    geometry.physicalRect = display.physicalRect;
    geometry.capturedLogicalRect = display.capturedLogicalRect;
    geometry.nativeDisplayId = display.nativeDisplayId;
    geometry.canvasUsesPoints = display.canvasUsesPoints;
    return geometry;
}

[[nodiscard]] inline QVector<CapturedDisplayModel>
screenshotSelectorDisplayGeometry(const QVector<CapturedDisplayModel>& displays) {
    QVector<CapturedDisplayModel> geometry;
    geometry.reserve(displays.size());
    for (const auto& display : displays)
        geometry.push_back(screenshotSelectorDisplayGeometry(display));
    return geometry;
}

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTORDISPLAYGEOMETRY_H
