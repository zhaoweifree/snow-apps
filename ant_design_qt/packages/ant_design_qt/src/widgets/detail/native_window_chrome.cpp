#include "native_window_chrome.h"

#include <QGuiApplication>
#include <QWidget>

#ifdef Q_OS_MACOS
#include "window_surface_mac_p.h"
#endif
#ifdef Q_OS_WIN
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 28301)
#endif
#include <qt_windows.h>
#include <dwmapi.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
#endif

namespace adqt::widgets::detail {

Qt::WindowFlags nativeWindowChromeFlags() {
#ifdef Q_OS_MACOS
  // AppKit supplies corner clipping and shadow around the expanded content area.
  return Qt::CustomizeWindowHint | Qt::WindowTitleHint | Qt::ExpandedClientAreaHint |
         Qt::NoTitleBarBackgroundHint;
#else
  return Qt::FramelessWindowHint;
#endif
}

void applyNativeWindowChrome(QWidget* surface, bool minimizeEnabled, bool maximizeEnabled) {
  if (!surface) return;
#ifdef Q_OS_MACOS
  if (QGuiApplication::platformName() == QStringLiteral("cocoa")) {
    applyMacWindowSurfaceChrome(surface, minimizeEnabled, maximizeEnabled);
  }
#endif
#ifdef Q_OS_WIN
  if (QGuiApplication::platformName() != QStringLiteral("windows")) return;
  // Qt transports the native HWND through its integer-valued WId type.
  const HWND hwnd = reinterpret_cast<HWND>(surface->winId());  // NOLINT(performance-no-int-to-ptr)
  if (!hwnd) return;

  LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
  style |= WS_CAPTION | WS_SYSMENU | WS_THICKFRAME;
  style &= ~(WS_MAXIMIZEBOX | WS_MINIMIZEBOX);
  if (minimizeEnabled) style |= WS_MINIMIZEBOX;
  if (maximizeEnabled) style |= WS_MAXIMIZEBOX;
  SetWindowLongPtr(hwnd, GWL_STYLE, style);

  LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
  exStyle &= ~WS_EX_LAYERED;
  SetWindowLongPtr(hwnd, GWL_EXSTYLE, exStyle);

  const DWMNCRENDERINGPOLICY renderingPolicy = DWMNCRP_ENABLED;
  DwmSetWindowAttribute(hwnd, DWMWA_NCRENDERING_POLICY, &renderingPolicy, sizeof(renderingPolicy));
  const MARGINS margins = {1, 1, 1, 1};
  DwmExtendFrameIntoClientArea(hwnd, &margins);
  SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
               SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
#endif
#if !defined(Q_OS_MACOS) && !defined(Q_OS_WIN)
  Q_UNUSED(minimizeEnabled)
  Q_UNUSED(maximizeEnabled)
#endif
}

}  // namespace adqt::widgets::detail
