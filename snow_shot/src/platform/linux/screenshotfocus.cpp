#include "snow_shot/platform/screenshotfocus.h"

#include "x11platform_p.h"

#include <QGuiApplication>
#include <QScreen>
#include <QtMath>

#include <limits>

namespace snow_shot::platform {
namespace {

using linuxdetail::onX11;
using linuxdetail::openPlatform;

// Undo Qt's scaling for one window rectangle.
//
// Qt reports a screen in logical coordinates and gives the ratio that turns them
// into the server's pixels. The inverse is applied per edge rather than to the
// size, so a fractional ratio cannot shorten the rectangle -- the same rule
// `snow_linux::fullscreen::logical_to_native` follows in the other direction, and
// the two have to agree or a captured window and its history entry disagree about
// where it was.
QRect logicalRectForRootPixels(const QRect& frame) {
    for (const QScreen* screen : QGuiApplication::screens()) {
        const qreal ratio = screen->devicePixelRatio();
        if (!(ratio > 0.0)) {
            continue;
        }
        const QRect logical = screen->geometry();
        const QRect native(
            QPoint(qRound(logical.x() * ratio), qRound(logical.y() * ratio)),
            QSize(qRound(logical.width() * ratio), qRound(logical.height() * ratio)));
        if (!native.contains(frame)) {
            continue;
        }
        const qreal left = frame.left() / ratio;
        const qreal top = frame.top() / ratio;
        const qreal right = (frame.right() + 1) / ratio;
        const qreal bottom = (frame.bottom() + 1) / ratio;
        return QRect(QPoint(qFloor(left), qFloor(top)),
                     QPoint(qCeil(right) - 1, qCeil(bottom) - 1));
    }
    // A window straddling two heads has no single ratio, and picking one would
    // place it on the wrong head. An empty rectangle tells the caller to fall back
    // to device pixels, which the history canvas already does for it.
    return {};
}

} // namespace

bool screenshotFocusedWindowTarget(quint32* id, QRect* frame, QRect* logicalBounds) {
    if (id != nullptr) {
        *id = 0;
    }
    if (frame != nullptr) {
        *frame = QRect();
    }
    if (logicalBounds != nullptr) {
        *logicalBounds = QRect();
    }
    if (!onX11()) {
        return false;
    }
    const std::shared_ptr<SnowLinuxPlatform> platform = openPlatform();
    if (!platform) {
        return false;
    }
    quint32 windowId = 0;
    qint32 x = 0;
    qint32 y = 0;
    quint32 width = 0;
    quint32 height = 0;
    if (snow_linux_platform_focused_window(platform.get(), &windowId, &x, &y, &width, &height) ==
        0) {
        return false;
    }
    // A window larger than a QRect can express is not a window any server
    // produced; reporting "no target" beats letting the cast wrap into a
    // negative size.
    if (width > static_cast<quint32>(std::numeric_limits<int>::max()) ||
        height > static_cast<quint32>(std::numeric_limits<int>::max())) {
        return false;
    }
    const QRect rootFrame(x, y, static_cast<int>(width), static_cast<int>(height));
    if (id != nullptr) {
        *id = windowId;
    }
    if (frame != nullptr) {
        *frame = rootFrame;
    }
    if (logicalBounds != nullptr) {
        *logicalBounds = logicalRectForRootPixels(rootFrame);
    }
    return true;
}

} // namespace snow_shot::platform
