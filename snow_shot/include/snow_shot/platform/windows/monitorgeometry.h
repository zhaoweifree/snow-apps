#ifndef SNOW_SHOT_PLATFORM_WINDOWS_MONITORGEOMETRY_H
#define SNOW_SHOT_PLATFORM_WINDOWS_MONITORGEOMETRY_H

#include <QRect>

class QScreen;

namespace snow_shot::platform::windows {

// Physical monitor rectangle for screen in the native desktop coordinates the capture backend
// reports. Empty when the monitor cannot be queried; callers must not fall back to Qt logical
// geometry, because the two spaces disagree under mixed-DPI scaling. Returns an empty rectangle
// on platforms without a native monitor space (macOS uses its point-based capture canvas).
[[nodiscard]] QRect nativeMonitorRect(const QScreen& screen);

// Native monitor handle behind `screen` (HMONITOR on Windows), or nullptr when it cannot be
// resolved. Returned as void* on purpose: this header is also included from cross-platform
// translation units (mcpapplicationservice.cpp / screenshotgeometry.cpp / historypinplacement.cpp)
// that must not pull in <qt_windows.h>. Windows callers cast explicitly:
//     const HMONITOR monitor = static_cast<HMONITOR>(nativeMonitorHandle(screen));
//
// Why this exists: Qt 6.4 promoted the platform screen handle into the public
// QNativeInterface::QWindowsScreen::handle(). Qt 5 has no public equivalent, and the
// QWindowsScreen type itself lives in the platform plugin, whose private headers are never
// installed. The Qt 5 route therefore goes through
// QPlatformNativeInterface::nativeResourceForScreen("handle", screen), which the Windows
// platform plugin implements as exactly QWindowsScreen::handle() - same HMONITOR, same
// semantics. Both spellings are unified here so call sites stay version-agnostic.
[[nodiscard]] void* nativeMonitorHandle(const QScreen* screen);

} // namespace snow_shot::platform::windows

#endif // SNOW_SHOT_PLATFORM_WINDOWS_MONITORGEOMETRY_H
