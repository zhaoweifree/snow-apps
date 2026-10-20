#ifndef SNOW_SHOT_PLATFORM_SCREENSHOTFOCUS_H
#define SNOW_SHOT_PLATFORM_SCREENSHOTFOCUS_H

#include <QRect>
#include <QRectF>
#include <QtGlobal>

namespace snow_shot::platform {

// "Which window and which display is the user looking at" - the answer Windows gets
// from GetForegroundWindow()/MonitorFromPoint() and macOS gets from AppKit.
//
// Windows does not appear here at all: its call sites reach Win32 directly, so a
// neutral declaration would be a second, unused way to ask the same question.
//
// Each platform declares only what it can answer. In particular there is no stub
// that reports "no window", because that reads as "nothing is focused" and the
// caller acts on it. Linux has no `screenshotDisplayAtCursor()` for the same
// reason: an X11 display has no id this layer could hand out, so its call sites ask
// Qt for the screen instead of being given a number that means nothing.
#ifdef Q_OS_MACOS
quint32 screenshotDisplayAtCursor();
quint32 screenshotFocusedWindow();
QRectF screenshotFocusedWindowBounds();
#elif defined(Q_OS_LINUX)
// The focused window as a capture target: window id, frame rectangle and the same
// rectangle in Qt's coordinates, all from one read.
//
// `frame` is in root-window pixels -- device pixels, decorations included -- which
// is the space captured frames are addressed in. `logicalBounds` is that rectangle
// in Qt's coordinate system, which is what the overlay and the history canvas use;
// it is empty when the window straddles two heads, because no single ratio turns
// root pixels into points there.
//
// Any output may be null; the caller that only needs one of them says so. Returns
// false, leaving every output null, when nothing capturable has focus: no X server
// reachable, nothing focused, or a desktop/panel window. The three answers come from
// a single query rather than three, because three reads could describe three
// different windows if the focus changed in between.
[[nodiscard]] bool screenshotFocusedWindowTarget(quint32* id, QRect* frame, QRect* logicalBounds);
#endif

} // namespace snow_shot::platform

#endif // SNOW_SHOT_PLATFORM_SCREENSHOTFOCUS_H
