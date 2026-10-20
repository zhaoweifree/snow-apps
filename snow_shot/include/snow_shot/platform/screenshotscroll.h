#ifndef SNOW_SHOT_PLATFORM_SCREENSHOTSCROLL_H
#define SNOW_SHOT_PLATFORM_SCREENSHOTSCROLL_H

#include <QPoint>
#include <QRect>
#include <QtGlobal>

namespace snow_shot::platform {

// Wheel injection for scrolling capture.
//
// This is the one part of the old screenshotnative.h that is genuinely platform neutral:
// every platform has an answer, and "there is no way to send a wheel step" is itself an
// answer (Unsupported) rather than a reason to leave the call site without a branch.
struct ScrollInputResult {
    enum class Status {
        Posted,
        InvalidRequest,
        TargetNotFound,
        CoordinateFailure,
        PostFailed,
        Unsupported
    };
    Status status = Status::Unsupported;
    quint32 error = 0;
};

// Whether this platform may synthesize wheel events at all. False means the scrolling
// capture feature must stay inert; it is not a hint that the user should grant something.
bool screenshotScrollPermission();

// `desktopSelection` is in desktop pixels, `wheelDelta` in wheel units.
ScrollInputResult sendScreenshotScroll(const QRect& desktopSelection, const QPoint& delta);

} // namespace snow_shot::platform

#endif // SNOW_SHOT_PLATFORM_SCREENSHOTSCROLL_H
