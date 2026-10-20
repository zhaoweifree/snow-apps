#include "snow_shot/platform/focusedfullscreenwindow.h"

#include "snow_linux.h"

#include <QGuiApplication>
#include <QScreen>

#include <memory>

namespace snow_shot::platform {
namespace {

// X11 only. Under a Wayland session Qt uses the `wayland` platform even when
// XWayland runs, and there the X server sees only XWayland's windows: the
// focused window would be whichever X client last had focus rather than the
// application the user is looking at, which is the wrong answer for suppressing
// shortcuts. Reporting "no full-screen window" leaves the shortcut working,
// which is the safe direction to be wrong in.
bool onX11() {
    return qGuiApp && QGuiApplication::platformName() == QStringLiteral("xcb");
}

// The handle is a capability token over the process-wide connection, so opening
// and closing one per query costs no handshake -- and the query runs at most
// once a second, from the shortcut manager and the floating toolbar.
std::shared_ptr<SnowLinuxPlatform> openPlatform() {
    return std::shared_ptr<SnowLinuxPlatform>(snow_linux_platform_create(),
                                              &snow_linux_platform_destroy);
}

} // namespace

bool focusedFullscreenWindowOnScreen(QScreen* screen) {
    if (screen == nullptr || !onX11()) {
        return false;
    }
    const std::shared_ptr<SnowLinuxPlatform> platform = openPlatform();
    if (!platform) {
        return false;
    }
    // Qt's rectangle and its ratio are handed over untouched: turning them into
    // root pixels is the platform layer's job, because that is where fractional
    // ratios can be tested without a desktop session.
    const QRect logical = screen->geometry();
    return snow_linux_platform_focused_fullscreen_on_display(
               platform.get(), logical.x(), logical.y(),
               static_cast<uint32_t>(qMax(logical.width(), 0)),
               static_cast<uint32_t>(qMax(logical.height(), 0)),
               static_cast<double>(screen->devicePixelRatio())) != 0;
}

bool focusedFullscreenWindowExists() {
    if (!onX11()) {
        return false;
    }
    const std::shared_ptr<SnowLinuxPlatform> platform = openPlatform();
    if (!platform) {
        return false;
    }
    return snow_linux_platform_focused_fullscreen(platform.get()) != 0;
}

} // namespace snow_shot::platform
