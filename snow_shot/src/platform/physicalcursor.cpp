#include "snow_shot/platform/physicalcursor.h"

#include <QtGlobal>
#include <QGuiApplication>
#include <qpa/qwindowsysteminterface.h>
#include <private/qhighdpiscaling_p.h>
#include <QWindow>

#include <cmath>
#include <limits>
#include <utility>

#if defined(Q_OS_WIN) || defined(_WIN32)
#include <qt_windows.h>
#elif defined(Q_OS_MACOS)
#include <CoreGraphics/CoreGraphics.h>
#include <QScreen>
#include <QPointer>
#include <memory>
#elif defined(Q_OS_LINUX)
#include <memory>
#include "snow_linux.h"
#endif

namespace snow_shot::platform {
namespace {

PhysicalCursorAccess nativeAccess() {
#if defined(Q_OS_WIN) || defined(_WIN32)
    return PhysicalCursorAccess{
        true,
        []() -> std::optional<QPoint> {
            POINT position{};
            if (GetPhysicalCursorPos(&position) == FALSE) {
                return std::nullopt;
            }
            return QPoint(position.x, position.y);
        },
        [](const QPoint& position) {
            return SetPhysicalCursorPos(position.x(), position.y()) != FALSE;
        },
    };
#elif defined(Q_OS_MACOS)
    if (!qGuiApp || QGuiApplication::platformName() != QStringLiteral("cocoa"))
        return {};
    auto display = std::make_shared<QPointer<QScreen>>();
    return PhysicalCursorAccess{
        true,
        [display]() -> std::optional<QPoint> {
            CGEventRef event = CGEventCreate(nullptr);
            if (!event)
                return std::nullopt;
            const CGPoint point = CGEventGetLocation(event);
            CFRelease(event);
            const QPointF desktop(point.x, point.y);
            *display = QGuiApplication::screenAt(QPoint(static_cast<int>(std::floor(desktop.x())),
                                                        static_cast<int>(std::floor(desktop.y()))));
            if (!*display)
                return std::nullopt;
            const QPoint origin = (*display)->geometry().topLeft();
            return origin + ((desktop - origin) * (*display)->devicePixelRatio()).toPoint();
        },
        [display](const QPoint& pixels) {
            if (!*display)
                return false;
            const QPoint origin = (*display)->geometry().topLeft();
            const QPointF desktop =
                QPointF(origin) + QPointF(pixels - origin) / (*display)->devicePixelRatio();
            if (!QGuiApplication::screenAt(QPoint(static_cast<int>(std::floor(desktop.x())),
                                                  static_cast<int>(std::floor(desktop.y())))))
                return false;
            if (CGWarpMouseCursorPosition(CGPointMake(desktop.x(), desktop.y())) != kCGErrorSuccess)
                return false;
            CGEventRef event = CGEventCreate(nullptr);
            if (!event)
                return false;
            const CGPoint actual = CGEventGetLocation(event);
            CFRelease(event);
            const qreal tolerance = .51 / (*display)->devicePixelRatio();
            return qAbs(actual.x - desktop.x()) < tolerance &&
                   qAbs(actual.y - desktop.y()) < tolerance;
        },
        []() -> std::optional<QPointF> {
            CGEventRef event = CGEventCreate(nullptr);
            if (!event)
                return std::nullopt;
            const CGPoint point = CGEventGetLocation(event);
            CFRelease(event);
            return QPointF(point.x, point.y);
        },
        false,
        [display] {
            // CoreGraphics warps floor desktop coordinates to whole points. A Retina
            // half-point request otherwise moves up/left but stalls down/right.
            return *display ? qRound((*display)->devicePixelRatio()) : 1;
        }};
#elif defined(Q_OS_LINUX)
    // X11 only. Under a Wayland session Qt uses the `wayland` platform even when
    // XWayland is running, and there XQueryPointer answers from XWayland's copy of
    // the pointer, which stops following it once it leaves an X window -- a stale
    // position rather than an absent one, which is worse than unsupported.
    if (!qGuiApp || QGuiApplication::platformName() != QStringLiteral("xcb"))
        return {};
    // One X11 connection for the process, kept by the Rust platform layer: Qt
    // constructs a PhysicalCursor as a temporary on hot paths, so a connection
    // per instance would handshake with the server on every mouse move. A null
    // handle means no X server is reachable, which is exactly "unsupported".
    std::shared_ptr<SnowLinuxPlatform> platform(snow_linux_platform_create(),
                                                &snow_linux_platform_destroy);
    if (!platform)
        return {};
    return PhysicalCursorAccess{
        true,
        [platform]() -> std::optional<QPoint> {
            int32_t x = 0;
            int32_t y = 0;
            if (snow_linux_platform_pointer_position(platform.get(), &x, &y) == 0)
                return std::nullopt;
            return QPoint(static_cast<int>(x), static_cast<int>(y));
        },
        [platform](const QPoint& pixels) {
            return snow_linux_platform_set_pointer_position(platform.get(), pixels.x(),
                                                            pixels.y()) != 0;
        },
        // `readLogicalPosition` stays empty on purpose: on X11 the root
        // coordinates are device pixels, and the logical ones depend on the
        // screen's scale factor, which Qt already applies to QCursor::pos().
        // Callers fall back to that, so mapping a second time here would be a
        // second opinion with worse information.
        {},
        // A warp is a real motion event for the server, so Qt's own pointer
        // state follows it; there is nothing to synthesize.
        true,
    };
#else
    return {};
#endif
}

QPoint offsetForDirection(PhysicalCursorDirection direction) {
    switch (direction) {
    case PhysicalCursorDirection::Up:
        return QPoint(0, -1);
    case PhysicalCursorDirection::Down:
        return QPoint(0, 1);
    case PhysicalCursorDirection::Left:
        return QPoint(-1, 0);
    case PhysicalCursorDirection::Right:
        return QPoint(1, 0);
    }
    Q_UNREACHABLE_RETURN(QPoint());
}

std::optional<QPoint> targetPosition(const QPoint& current, const QPoint& offset, qint64 distance) {
    if (distance <= 0 || offset.isNull())
        return std::nullopt;
    const qint64 x = static_cast<qint64>(current.x()) + static_cast<qint64>(offset.x()) * distance;
    const qint64 y = static_cast<qint64>(current.y()) + static_cast<qint64>(offset.y()) * distance;
    if (x < std::numeric_limits<int>::min() || x > std::numeric_limits<int>::max() ||
        y < std::numeric_limits<int>::min() || y > std::numeric_limits<int>::max()) {
        return std::nullopt;
    }
    return QPoint(static_cast<int>(x), static_cast<int>(y));
}

} // namespace

PhysicalCursor::PhysicalCursor() : m_access(nativeAccess()) {}

PhysicalCursor::PhysicalCursor(PhysicalCursorAccess access) : m_access(std::move(access)) {}

bool PhysicalCursor::isSupported() const noexcept {
    return m_access.supported && static_cast<bool>(m_access.readPosition) &&
           static_cast<bool>(m_access.writePosition);
}

bool PhysicalCursor::canRead() const noexcept {
    return m_access.supported && static_cast<bool>(m_access.readPosition);
}

std::optional<QPoint> PhysicalCursor::position() const {
    if (!canRead()) {
        return std::nullopt;
    }
    return m_access.readPosition();
}

std::optional<QPointF> PhysicalCursor::logicalPosition() const {
    return m_access.readLogicalPosition ? m_access.readLogicalPosition() : std::nullopt;
}

PhysicalCursorMoveResult PhysicalCursor::moveOnePixel(PhysicalCursorDirection direction) const {
    return movePixels(direction, 1);
}

PhysicalCursorMoveResult PhysicalCursor::movePixels(PhysicalCursorDirection direction,
                                                    int distance) const {
    return movePixels(offsetForDirection(direction), distance);
}

PhysicalCursorMoveResult PhysicalCursor::movePixels(const QPoint& direction, int distance) const {
    if (!isSupported()) {
        return {PhysicalCursorMoveStatus::Unsupported, std::nullopt};
    }

    const std::optional<QPoint> current = m_access.readPosition();
    if (!current.has_value()) {
        return {PhysicalCursorMoveStatus::ReadFailed, std::nullopt};
    }

    const int quantum = m_access.movementQuantum ? m_access.movementQuantum() : 1;
    if (distance <= 0 || quantum <= 0)
        return {PhysicalCursorMoveStatus::InvalidTarget, std::nullopt};
    const qint64 nativeDistance =
        ((static_cast<qint64>(distance) + quantum - 1) / quantum) * quantum;
    const std::optional<QPoint> target = targetPosition(current.value(), direction, nativeDistance);
    if (!target.has_value()) {
        return {PhysicalCursorMoveStatus::InvalidTarget, std::nullopt};
    }
    if (!m_access.writePosition(target.value())) {
        return {PhysicalCursorMoveStatus::WriteFailed, std::nullopt};
    }

    const std::optional<QPoint> actual = m_access.readPosition();
    bool mouseMoveDispatched = false;
    if (!m_access.generatesMouseMoveEvents && qGuiApp) {
        if (const auto global = logicalPosition()) {
            // Route through the native QWidget window so Qt retains implicit/explicit
            // mouse grabs and child hit testing. Sending directly to the widget under
            // the cursor would lose an in-progress drag when it crosses a child/window.
            QWindow* window =
                QGuiApplication::topLevelAt(QPoint(static_cast<int>(std::floor(global->x())),
                                                   static_cast<int>(std::floor(global->y()))));
            if (!window && QGuiApplication::mouseButtons() != Qt::NoButton)
                window = QGuiApplication::focusWindow();
            if (window) {
                const QPointF local = snowMapFromGlobal(window, *global);
                // Enter through QPA, not sendEvent(): Qt must also update its global
                // pointer position or it can discard the next real move back to the
                // pre-warp position as an unchanged-position event.
                QWindowSystemInterface::handleMouseEvent<
                    QWindowSystemInterface::SynchronousDelivery>(
                    window, QHighDpi::toNativeLocalPosition(local, window),
                    QHighDpi::toNativePixels(*global, window), QGuiApplication::mouseButtons(),
                    Qt::NoButton, QEvent::MouseMove, QGuiApplication::keyboardModifiers(),
                    Qt::MouseEventSynthesizedByApplication);
                mouseMoveDispatched = true;
            }
        }
    }
    return {actual ? PhysicalCursorMoveStatus::Applied
                   : PhysicalCursorMoveStatus::AppliedPositionUnavailable,
            actual, mouseMoveDispatched};
}

} // namespace snow_shot::platform
