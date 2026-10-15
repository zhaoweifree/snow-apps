#include "snow_shot/platform/windows/monitorgeometry.h"

#if defined(Q_OS_WIN) || defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <QGuiApplication>
#include <QScreen>
#include <qt_windows.h>
#if QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
#include <QtGui/qscreen_platform.h>
#else
// Qt 5.15 has no public QNativeInterface. The QPA native interface is the equivalent source:
// the Windows plugin's QWindowsNativeInterface::nativeResourceForScreen("handle", screen)
// returns QWindowsScreen::handle(), i.e. the HMONITOR. The header ships with the QtGui private
// headers (include/QtGui/5.15.2/QtGui/qpa/), and this project already takes the same route for
// window handles in snow_shot/src/platform/windows/pinnedwindownative.cpp.
#include <QByteArray>
#include <qpa/qplatformnativeinterface.h>
#endif
#endif

namespace snow_shot::platform::windows {

void* nativeMonitorHandle(const QScreen* screen) {
#if defined(Q_OS_WIN) || defined(_WIN32)
    if (screen == nullptr) {
        return nullptr;
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
    const auto* native = screen->nativeInterface<QNativeInterface::QWindowsScreen>();
    return native != nullptr ? static_cast<void*>(native->handle()) : nullptr;
#else
    QPlatformNativeInterface* native = QGuiApplication::platformNativeInterface();
    if (native == nullptr) {
        return nullptr;
    }
    // Qt 5's accessor takes a non-const QScreen*; it only reads the screen. The resource key
    // must be exactly "handle" - that is the only screen resource the Windows plugin accepts.
    return native->nativeResourceForScreen(QByteArrayLiteral("handle"),
                                           const_cast<QScreen*>(screen));
#endif
#else
    static_cast<void>(screen);
    return nullptr;
#endif
}

QRect nativeMonitorRect(const QScreen& screen) {
#if defined(Q_OS_WIN) || defined(_WIN32)
    // Capture coordinates come from rcMonitor, which is only reachable through the native
    // monitor handle - Qt's logical geometry cannot reconstruct physical pixels at fractional DPI.
    const HMONITOR monitor = static_cast<HMONITOR>(nativeMonitorHandle(&screen));
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (monitor == nullptr || GetMonitorInfoW(monitor, &info) == FALSE) {
        return {};
    }
    const int width = static_cast<int>(info.rcMonitor.right - info.rcMonitor.left);
    const int height = static_cast<int>(info.rcMonitor.bottom - info.rcMonitor.top);
    if (width <= 0 || height <= 0) {
        return {};
    }
    return QRect(static_cast<int>(info.rcMonitor.left), static_cast<int>(info.rcMonitor.top), width,
                 height);
#else
    static_cast<void>(screen);
    return {};
#endif
}

} // namespace snow_shot::platform::windows
