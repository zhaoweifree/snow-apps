#include "snow_shot/platform/screenshotscroll.h"

#if !defined(Q_OS_MACOS)

#if defined(Q_OS_WIN)
#include "snow_shot/platform/windows/scrollinput.h"
#endif

namespace snow_shot::platform {

bool screenshotScrollPermission() {
#if defined(Q_OS_WIN)
    // No accessibility prompt stands between a process and SendInput on Windows.
    return true;
#else
    // X11 has no permission model for synthetic input either, but wheel injection is not
    // implemented in the snow-linux backend yet - snow-capture *observes* wheel events,
    // nothing posts them. Reporting "no permission" keeps the scrolling capture inert
    // instead of letting the auto scroller believe its wheel steps landed. Replace this
    // with `true` together with a real sendScreenshotScroll().
    return false;
#endif
}

ScrollInputResult sendScreenshotScroll(const QRect& desktopSelection, const QPoint& delta) {
#if defined(Q_OS_WIN)
    return windows::sendScrollingWheelStep(desktopSelection, delta);
#else
    Q_UNUSED(desktopSelection)
    Q_UNUSED(delta)
    // Unsupported rather than PostFailed: nothing was attempted, and the callers already
    // surface this status as a distinct failure instead of "the scroll did not take".
    return {};
#endif
}

} // namespace snow_shot::platform

#endif // !Q_OS_MACOS
