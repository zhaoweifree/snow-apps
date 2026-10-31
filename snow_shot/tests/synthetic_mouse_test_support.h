#ifndef SNOW_SHOT_TESTS_SYNTHETIC_MOUSE_TEST_SUPPORT_H
#define SNOW_SHOT_TESTS_SYNTHETIC_MOUSE_TEST_SUPPORT_H

#include <QDateTime>
#include <QEvent>
#include <QPointF>
#include <QWindow>
#include <qpa/qwindowsysteminterface.h>

// Qt 5.15 has no timestamp-free QWindowSystemInterface::handleMouseEvent
// template: the argument after the window is `ulong timestamp` there, so the
// Qt 6 spelling (window, local, global, state, button, type) finds no candidate
// at all. The timestamp only orders deliveries inside the platform queue, and
// the routing these tests exercise keys off the arguments, so "now" is a
// faithful stand-in for it.
template <typename Delivery>
bool sendSyntheticMouse(QWindow* window, const QPointF& local, const QPointF& global,
                        Qt::MouseButtons state, Qt::MouseButton button, QEvent::Type type) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return QWindowSystemInterface::handleMouseEvent<Delivery>(window, local, global, state, button,
                                                              type);
#else
    return QWindowSystemInterface::handleMouseEvent<Delivery>(
        window, static_cast<ulong>(QDateTime::currentMSecsSinceEpoch()), local, global, state,
        button, type);
#endif
}

#endif // SNOW_SHOT_TESTS_SYNTHETIC_MOUSE_TEST_SUPPORT_H
