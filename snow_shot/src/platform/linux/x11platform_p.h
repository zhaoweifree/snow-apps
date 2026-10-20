#ifndef SNOW_SHOT_PLATFORM_LINUX_X11PLATFORM_P_H
#define SNOW_SHOT_PLATFORM_LINUX_X11PLATFORM_P_H

#include "snow_linux.h"

#include <QGuiApplication>

#include <memory>

namespace snow_shot::platform::linuxdetail {

// X11 only. Under a Wayland session Qt uses the `wayland` platform even when
// XWayland runs, and there the X server sees only XWayland's windows: the focused
// window would be whichever X client last had focus rather than the application
// the user is looking at. Every query in this directory would then answer about
// the wrong window, so each one reports "unknown" instead -- which leaves the
// feature on its Qt-only behaviour, the safe direction to be wrong in.
inline bool onX11() {
    return qGuiApp != nullptr && QGuiApplication::platformName() == QStringLiteral("xcb");
}

// The handle is a capability token over the process-wide connection, so opening
// and closing one per query costs no handshake. The queries here run at most a
// few times a second, from the shortcut manager, the floating toolbar and the
// capture path.
inline std::shared_ptr<SnowLinuxPlatform> openPlatform() {
    return std::shared_ptr<SnowLinuxPlatform>(snow_linux_platform_create(),
                                              &snow_linux_platform_destroy);
}

} // namespace snow_shot::platform::linuxdetail

#endif // SNOW_SHOT_PLATFORM_LINUX_X11PLATFORM_P_H
