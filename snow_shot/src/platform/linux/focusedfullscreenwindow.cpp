#include "snow_shot/platform/focusedfullscreenwindow.h"

#include "x11platform_p.h"

#include <QScreen>

namespace snow_shot::platform {
namespace {

using linuxdetail::onX11;
using linuxdetail::openPlatform;

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
