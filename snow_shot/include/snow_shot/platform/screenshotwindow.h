#ifndef SNOW_SHOT_PLATFORM_SCREENSHOTWINDOW_H
#define SNOW_SHOT_PLATFORM_SCREENSHOTWINDOW_H

class QWidget;

namespace snow_shot::platform {

// Hooks for the windows this application owns: overlay, toolbars, recording frames.
//
// Qt's window flags already express the intent on every platform, and that is enough on
// Windows. macOS needs native help on top (window levels, collection behaviour, input
// routing that masks do not provide), so it implements these in
// src/platform/macos/screenshotnative.mm.
//
// Everything else falls through to a no-op on purpose. A no-op is the correct answer for
// "do whatever this platform additionally needs": it makes the hook callable from shared
// code without a platform guard at every call site, and it keeps the X11 behaviour of
// these windows exactly as Qt configures them. X11 has not been verified for window
// levels or input shapes yet - when it is, the implementation replaces the stub in one
// file instead of touching ten call sites.
#ifdef Q_OS_MACOS
// Give Qt exclusive drag ownership, including after native surface recreation.
void configureControlledWindowDragging(QWidget* widget, bool controlResizing = false);
void configureScreenshotOverlayWindow(QWidget* widget);
void configureScreenshotColorPickerWindow(QWidget* widget);
void configureGlobalCanvasWindow(QWidget* widget);
void configureScreenRecordingAreaWindow(QWidget* widget);
void configureScreenRecordingToolbarWindow(QWidget* widget);
void configureScreenshotRecognitionWindow(QWidget* widget);
// Order an already-visible passive tool without activating the application or changing levels.
[[nodiscard]] bool stackScreenshotWindowBelow(QWidget* widget, QWidget* sibling);
// Cocoa masks clip drawing, but do not route input to windows underneath.
void setScreenshotInputTransparent(QWidget* widget, bool transparent);
void configureScreenshotToolbarWindow(QWidget* widget);
// Desktop tools receive content drags; an explicitly visible capture tool must
// temporarily stay above its capture overlay instead.
void configureFloatingToolbarWindow(QWidget* widget, bool captureActive = false);
#else
inline void configureControlledWindowDragging(QWidget*, bool = false) {}
inline void configureScreenshotOverlayWindow(QWidget*) {}
inline void configureScreenshotColorPickerWindow(QWidget*) {}
inline void configureGlobalCanvasWindow(QWidget*) {}
inline void configureScreenRecordingAreaWindow(QWidget*) {}
inline void configureScreenRecordingToolbarWindow(QWidget*) {}
inline void configureScreenshotRecognitionWindow(QWidget*) {}
inline bool stackScreenshotWindowBelow(QWidget*, QWidget*) {
    return false;
}
inline void setScreenshotInputTransparent(QWidget*, bool) {}
inline void configureScreenshotToolbarWindow(QWidget*) {}
inline void configureFloatingToolbarWindow(QWidget*, bool = false) {}
#endif

} // namespace snow_shot::platform

#endif // SNOW_SHOT_PLATFORM_SCREENSHOTWINDOW_H
