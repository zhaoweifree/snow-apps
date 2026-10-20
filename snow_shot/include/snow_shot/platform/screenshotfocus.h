#ifndef SNOW_SHOT_PLATFORM_SCREENSHOTFOCUS_H
#define SNOW_SHOT_PLATFORM_SCREENSHOTFOCUS_H

#include <QRectF>
#include <QtGlobal>

namespace snow_shot::platform {

// "Which window and which display is the user looking at" - the answer Windows gets
// from GetForegroundWindow()/MonitorFromPoint() and macOS gets from AppKit.
//
// macOS only for now, and the guard is deliberate: the Windows call sites reach Win32
// directly, so a neutral declaration here would be a second, unused way to ask the same
// question. Linux has the machinery (the X11 window tree walk in snow-linux already
// answers it for the fullscreen check) but no C ABI exposes it yet; until it does, the
// Linux call sites keep their own branch and this header stays macOS-only rather than
// offering a stub that returns "no window" - that would read as "nothing is focused".
#ifdef Q_OS_MACOS
quint32 screenshotDisplayAtCursor();
quint32 screenshotFocusedWindow();
QRectF screenshotFocusedWindowBounds();
#endif

} // namespace snow_shot::platform

#endif // SNOW_SHOT_PLATFORM_SCREENSHOTFOCUS_H
