#pragma once

// =============================================================================
// Snow Apps - Qt 5 / Qt 6 dual-version compatibility shim.
//
// Purpose: let the same sources compile against both Qt 5.15 and Qt 6.12,
//          without scattering `#if QT_VERSION` branches through business code.
//
// Usage: injected ONLY for Qt5 builds, by snow_find_qt() in cmake/SnowQt.cmake,
//        as a directory-scope force-include:
//          MSVC      : /FI"snow_shot/compat/qt_compat.h"
//          GCC/Clang : -include snow_shot/compat/qt_compat.h
//        Not injected for Qt6 builds (including it explicitly is still safe and
//        resolves to the SNOW_QT6 branch, i.e. native Qt6 APIs).
//
// *** NOTE: this header is injected into EVERY C++ translation unit ***
//   including third-party (FetchContent / add_subdirectory) targets and
//   QtCore-only targets. It must therefore degrade to nothing when Qt is
//   absent or when only QtCore is available - that is what the __has_include
//   probes below are for.
//
// *** NOTE: keep this file 100% ASCII ***
//   MSVC parses sources using the system code page unless /utf-8 is given
//   (this machine is code page 936 / GBK). Non-ASCII bytes in a force-included
//   header corrupt the preprocessor on such setups - verified failure:
//     warning C4819 (cannot be represented in code page 936)
//     fatal error C1020 (unexpected #endif)
//   Business sources rely on the project's per-target /utf-8 instead.
//
// Coverage (snapshot from grep over the real tree, see docs-qt5-downgrade-plan.md 1.1;
// the counts drift as the tree changes - the point is that each row is covered):
//   QKeyCombination      52 hits / 16 files   introduced in Qt 6.0, no Qt5 peer
//   QEnterEvent sig      99 hits / 51 files   enterEvent takes QEnterEvent* since 6.0
//   QList::removeIf      49 hits / 22 files   introduced in Qt 6.1
//   QLatin1StringView    26 hits /  8 files   renamed in Qt 6.4 (Qt5: QLatin1String)
//   QMetaType::fromType   8 hits /  2 files   introduced in Qt 6.0
//   QVariant::metaType()  9 hits /  2 files   introduced in Qt 6.0
//
// Explicitly NOT needed (already ruled out during planning - do not add):
//   QImage::sizeInBytes()            exists in Qt 5.15 (returns int, not qsizetype)
//   Qt::SkipEmptyParts               available since Qt 5.14
//   QAction / QShortcut              already in QtWidgets in Qt5
//   QWindow::startSystemResize/Move  available since Qt 5.15
//   QFileInfo::isJunction()          available since Qt 5.15 (verified in qfileinfo.h:116)
//   QNetworkRequest::setTransferTimeout(int)  available since Qt 5.15 (qnetworkrequest.h:194)
// =============================================================================

#if defined(__has_include)
#  if __has_include(<QtGlobal>) || __has_include(<QtCore/QtGlobal>)
#    define SNOW_COMPAT_HAS_QT 1
#  else
#    define SNOW_COMPAT_HAS_QT 0
#  endif
#else
#  define SNOW_COMPAT_HAS_QT 0
#endif

#if !defined(__cplusplus) || !SNOW_COMPAT_HAS_QT
// Not C++, or this translation unit has no Qt include path at all
// (third-party target): this header expands to nothing.
#  define SNOW_COMPAT_INERT 1
#endif

#if !defined(SNOW_COMPAT_INERT)

#include <QtGlobal>
#include <QEvent>
#include <QFuture>
#include <QHash>
#include <QLine>
#include <QLibraryInfo>
#include <QList>
#include <QMargins>
#include <QMetaObject>
#include <QMetaType>
#include <QObject>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QSize>
#include <QTimeZone>
#include <QtMath>
#include <QVariant>

#include <cstddef>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

#if SNOW_QT5
// Qt5 keeps QVector as a distinct class; Qt6 aliases it to QList.
#  include <QVector>
#endif

// QtGui is not visible to QtCore-only targets, so probe it separately.
#if defined(__has_include)
#  if __has_include(<QKeySequence>)
#    define SNOW_COMPAT_HAS_GUI 1
#  else
#    define SNOW_COMPAT_HAS_GUI 0
#  endif
#else
#  define SNOW_COMPAT_HAS_GUI 0
#endif

#if SNOW_COMPAT_HAS_GUI
#  include <QKeySequence>
#endif

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#  define SNOW_QT6 1
#  define SNOW_QT5 0
#  define SNOW_COMPAT_HAS_ENTER_EVENT 1
#  include <QKeyCombination>
#  include <QEnterEvent>
#  include <QLatin1StringView>
#else
#  define SNOW_QT6 0
#  define SNOW_QT5 1
#  include <QLatin1String>
// QEnterEvent exists in QtGui since Qt 5.14, but it is NOT part of the Qt5
// virtual signature (QWidget::enterEvent() still takes a QEvent*). The class is
// needed only so the position helpers below can downcast to it, hence the
// separate availability flag rather than reusing SNOW_COMPAT_HAS_GUI.
#  if defined(__has_include) && __has_include(<QEnterEvent>)
#    define SNOW_COMPAT_HAS_ENTER_EVENT 1
#    include <QEnterEvent>
#  elif defined(__has_include) && __has_include(<QtGui/qevent.h>)
#    define SNOW_COMPAT_HAS_ENTER_EVENT 1
#    include <QtGui/qevent.h>
#  endif
#endif

#if !defined(SNOW_COMPAT_HAS_ENTER_EVENT)
#  define SNOW_COMPAT_HAS_ENTER_EVENT 0
#endif

// ---------------------------------------------------------------------------
// 1. QKeyCombination - introduced in Qt 6.0, no equivalent class in Qt5.
//
//    Qt5 idiom was to OR Qt::Key with Qt::KeyboardModifiers into an int and pass
//    that around directly; Qt6's fromCombined()/toCombined() pair exists exactly
//    to provide a migration path. Signatures below are replicated 1:1 from the
//    Qt 6.12 documentation so call sites can be replaced textually.
// ---------------------------------------------------------------------------
#if SNOW_QT5
namespace snow::compat
{

class QKeyCombination
{
public:
    // [constexpr noexcept] QKeyCombination(Qt::Key key = Qt::Key_unknown)
    constexpr QKeyCombination(Qt::Key key = Qt::Key_unknown) noexcept
        : m_key(key), m_modifiers(Qt::NoModifier)
    {
    }

    // [explicit constexpr noexcept]
    // QKeyCombination(Qt::KeyboardModifiers modifiers, Qt::Key key = Qt::Key_unknown)
    // Note: Qt6 also has a Qt::Modifiers overload; Qt5 has no Qt::Modifiers alias,
    // so it is intentionally not provided here.
    explicit constexpr QKeyCombination(Qt::KeyboardModifiers modifiers,
                                       Qt::Key key = Qt::Key_unknown) noexcept
        : m_key(key), m_modifiers(modifiers)
    {
    }

    [[nodiscard]] constexpr Qt::Key key() const noexcept { return m_key; }

    [[nodiscard]] constexpr Qt::KeyboardModifiers keyboardModifiers() const noexcept
    {
        return m_modifiers;
    }

    // ORs key() and keyboardModifiers() together (matches Qt6 semantics).
    [[nodiscard]] constexpr int toCombined() const noexcept
    {
        return static_cast<int>(m_key) | static_cast<int>(m_modifiers);
    }

    [[nodiscard]] static constexpr QKeyCombination fromCombined(int combined) noexcept
    {
        return QKeyCombination(Qt::KeyboardModifiers(combined & Qt::KeyboardModifierMask),
                               Qt::Key(combined & ~Qt::KeyboardModifierMask));
    }

    [[nodiscard]] friend constexpr bool operator==(const QKeyCombination& lhs,
                                                   const QKeyCombination& rhs) noexcept
    {
        return lhs.m_key == rhs.m_key && lhs.m_modifiers == rhs.m_modifiers;
    }

    [[nodiscard]] friend constexpr bool operator!=(const QKeyCombination& lhs,
                                                   const QKeyCombination& rhs) noexcept
    {
        return !(lhs == rhs);
    }

private:
    Qt::Key m_key;
    Qt::KeyboardModifiers m_modifiers;
};

// Qt5 qHash convention is uint return / uint seed (Qt6 switched to size_t).
[[nodiscard]] inline uint qHash(const QKeyCombination& key, uint seed = 0) noexcept
{
    return ::qHash(static_cast<uint>(key.toCombined()), seed);
}

} // namespace snow::compat

// Global injection: no same-named type exists in Qt5, so this cannot conflict.
using snow::compat::QKeyCombination;
#endif // SNOW_QT5

#if SNOW_COMPAT_HAS_GUI

// Qt5 QShortcut/QAction::setShortcut only accept QKeySequence, so bridging is
// needed. Mechanical rewrite rule R9:
//   `setShortcut(combination)` -> `setShortcut(snowKeySequence(combination))`
[[nodiscard]] inline QKeySequence snowKeySequence(const QKeyCombination& combination)
{
#if SNOW_QT6
    return QKeySequence(combination);
#else
    return QKeySequence(combination.toCombined());
#endif
}

[[nodiscard]] inline QKeySequence snowKeySequence(const QList<QKeyCombination>& combinations)
{
    // A QKeySequence holds at most 4 key combinations
    // (Qt5 exposes exactly this as QKeySequence(int, int, int, int)).
    // Note: Qt5/Qt6 QKeySequence::count() is a NON-static member reporting how
    // many keys that particular sequence holds, so it cannot be used here.
    // [[maybe_unused]]: only the Qt6 branch below loops with it; the Qt5 branch
    // relies on the 4-argument constructor instead, so /W4 would otherwise report
    // C4189 (and SNOW_APPS_STRICT_COMPILE promotes warnings to errors).
    [[maybe_unused]] constexpr int kSnowMaxKeySequenceLength = 4;
#if SNOW_QT6
    QKeySequence sequence;
    for (int i = 0; i < combinations.size() && i < kSnowMaxKeySequenceLength; ++i) {
        sequence.setKeyCombination(i, combinations.at(i));
    }
    return sequence;
#else
    // Qt5 has no usable public mutator: setKey() is private and operator[]
    // returns a plain int rvalue, so the 4-argument constructor is the only
    // public route (verified against Qt 5.15.2 QtGui/qkeysequence.h).
    const auto combinedAt = [&combinations](int index) -> int {
        return index < combinations.size() ? combinations.at(index).toCombined() : 0;
    };
    return QKeySequence(combinedAt(0), combinedAt(1), combinedAt(2), combinedAt(3));
#endif
}

// Reverse conversion: Qt6 QKeySequence::operator[] returns QKeyCombination while
// Qt5 returns int. The only consumer in the real tree is
// snow_shot/src/presentation/services/windowshortcutmanager.cpp
// (keyCombinationsFromBindings).
[[nodiscard]] inline QKeyCombination snowKeyCombination(const QKeySequence& sequence, int index = 0)
{
#if SNOW_QT6
    return sequence[index];
#else
    return QKeyCombination::fromCombined(sequence[index]);
#endif
}

#endif // SNOW_COMPAT_HAS_GUI

// ---------------------------------------------------------------------------
// 2. QEnterEvent - QWidget::enterEvent() takes QEnterEvent* since Qt 6.0.
//
//    Qt 5.14+ already has the QEnterEvent class, but the virtual function
//    signature still uses QEvent* (verified in Qt 5.15.2 QtWidgets/qwidget.h).
//    Spelling the parameter as SnowEnterEvent* lets a single override
//    declaration be correct on both versions.
// ---------------------------------------------------------------------------
#if SNOW_QT6
using SnowEnterEvent = QEnterEvent;
#else
using SnowEnterEvent = QEvent;
#endif

// ---------------------------------------------------------------------------
// 2b. Event position accessors.
//
//     Qt 6.0 renamed the coordinate accessors on the input events:
//         Qt5.x                            Qt6
//         QMouseEvent::localPos()          -> position()
//         QMouseEvent::globalPos()         -> globalPosition()
//         QHoverEvent::posF()              -> position()
//         QEnterEvent::localPos()          -> position()
//         QEnterEvent::globalPos()         -> globalPosition()
//     QWheelEvent is the exception: Qt 5.15 already backported position() /
//     globalPosition() to it, so no shim is needed for that type.
//     All of the above verified against Qt 5.15.2 QtGui/qevent.h.
//
//     Mechanical rewrite rules R10 / R11:
//         <recv>->position()        ->  snowPos(<recv>)
//         <recv>->globalPosition()  ->  snowGlobalPos(<recv>)
//
//     NOTE: Qt5's QHoverEvent carries NO global coordinate at all - it exposes
//     only pos()/posF()/oldPos()/oldPosF(). There is deliberately no
//     snowGlobalPos(const QHoverEvent*) overload, so such a call site fails to
//     compile instead of silently returning a wrong point; adapt it by hand
//     where the receiving widget is available (widget->mapToGlobal(...)).
// ---------------------------------------------------------------------------
#if SNOW_COMPAT_HAS_GUI

#  include <QHoverEvent>
#  include <QMouseEvent>
#  include <QNativeGestureEvent>
#  include <QWheelEvent>

[[nodiscard]] inline QPointF snowPos(const QMouseEvent* event)
{
#if SNOW_QT6
    return event->position();
#else
    return event->localPos();
#endif
}

[[nodiscard]] inline QPointF snowGlobalPos(const QMouseEvent* event)
{
#if SNOW_QT6
    return event->globalPosition();
#else
    // Qt5 returns QPoint here; the pixel values these events carry are integral,
    // so promoting to QPointF is lossless.
    return event->globalPos();
#endif
}

[[nodiscard]] inline QPointF snowPos(const QHoverEvent* event)
{
#if SNOW_QT6
    return event->position();
#else
    return event->posF();
#endif
}

// QWheelEvent needs no version branch at all.
[[nodiscard]] inline QPointF snowPos(const QWheelEvent* event)
{
    return event->position();
}

[[nodiscard]] inline QPointF snowGlobalPos(const QWheelEvent* event)
{
    return event->globalPosition();
}

// QNativeGestureEvent: Qt6 derives it from QSinglePointEvent and therefore has
// position()/globalPosition(); Qt5 has only localPos() / windowPos() /
// screenPos() / globalPos() (QtGui/qevent.h:343-362). Semantic mapping:
//     Qt6 position()       == Qt5 localPos()   (both are widget-local)
//     Qt6 globalPosition() == Qt5 globalPos()  (Qt5 returns QPoint, lossless here)
[[nodiscard]] inline QPointF snowPos(const QNativeGestureEvent* event)
{
#if SNOW_QT6
    return event->position();
#else
    return event->localPos();
#endif
}

[[nodiscard]] inline QPointF snowGlobalPos(const QNativeGestureEvent* event)
{
#if SNOW_QT6
    return event->globalPosition();
#else
    return event->globalPos();
#endif
}

#  if SNOW_COMPAT_HAS_ENTER_EVENT
[[nodiscard]] inline QPointF snowPos(const QEnterEvent* event)
{
#if SNOW_QT6
    return event->position();
#else
    return event->localPos();
#endif
}

[[nodiscard]] inline QPointF snowGlobalPos(const QEnterEvent* event)
{
#if SNOW_QT6
    return event->globalPosition();
#else
    return event->globalPos();
#endif
}
#  endif // SNOW_COMPAT_HAS_ENTER_EVENT

// Fallback for receivers whose static type is merely QEvent* - which is exactly
// the case for `enterEvent(SnowEnterEvent* event)` on Qt5, where SnowEnterEvent
// degrades to QEvent. Dispatches on the runtime event type (Qt's own pattern)
// rather than guessing.
[[nodiscard]] inline QPointF snowPos(const QEvent* event)
{
#if SNOW_QT6
    if (event == nullptr) {
        return {};
    }
    switch (event->type()) {
    case QEvent::Enter:
        return static_cast<const QEnterEvent*>(event)->position();
    case QEvent::HoverEnter:
    case QEvent::HoverLeave:
    case QEvent::HoverMove:
        return static_cast<const QHoverEvent*>(event)->position();
    case QEvent::Wheel:
        return static_cast<const QWheelEvent*>(event)->position();
    case QEvent::NativeGesture:
        return static_cast<const QNativeGestureEvent*>(event)->position();
    default:
        if (QEvent::MouseButtonPress <= event->type() && event->type() <= QEvent::MouseMove) {
            return static_cast<const QMouseEvent*>(event)->position();
        }
        return {};
    }
#else
    if (event == nullptr) {
        return {};
    }
    switch (event->type()) {
#  if SNOW_COMPAT_HAS_ENTER_EVENT
    case QEvent::Enter:
        return static_cast<const QEnterEvent*>(event)->localPos();
#  endif
    case QEvent::HoverEnter:
    case QEvent::HoverLeave:
    case QEvent::HoverMove:
        return static_cast<const QHoverEvent*>(event)->posF();
    case QEvent::Wheel:
        return static_cast<const QWheelEvent*>(event)->position();
    case QEvent::NativeGesture:
        return static_cast<const QNativeGestureEvent*>(event)->localPos();
    default:
        if (QEvent::MouseButtonPress <= event->type() && event->type() <= QEvent::MouseMove) {
            return static_cast<const QMouseEvent*>(event)->localPos();
        }
        return {};
    }
#endif
}

// Kept for the R4b rewrite documented in docs-qt5-downgrade-plan.md: a plain
// QEvent* alias of snowPos().
[[nodiscard]] inline QPointF snowEnterPosition(const QEvent* event)
{
    return snowPos(event);
}

// QEvent*-static-type counterpart of snowGlobalPos(). Same runtime dispatch as
// snowPos(const QEvent*) above, and the same caveat: there is no hover branch
// because Qt5's QHoverEvent has no global coordinate.
[[nodiscard]] inline QPointF snowGlobalPos(const QEvent* event)
{
#if SNOW_QT6
    if (event == nullptr) {
        return {};
    }
    switch (event->type()) {
    case QEvent::Enter:
        return static_cast<const QEnterEvent*>(event)->globalPosition();
    case QEvent::Wheel:
        return static_cast<const QWheelEvent*>(event)->globalPosition();
    case QEvent::NativeGesture:
        return static_cast<const QNativeGestureEvent*>(event)->globalPosition();
    default:
        if (QEvent::MouseButtonPress <= event->type() && event->type() <= QEvent::MouseMove) {
            return static_cast<const QMouseEvent*>(event)->globalPosition();
        }
        return {};
    }
#else
    if (event == nullptr) {
        return {};
    }
    switch (event->type()) {
#  if SNOW_COMPAT_HAS_ENTER_EVENT
    case QEvent::Enter:
        return static_cast<const QEnterEvent*>(event)->globalPos();
#  endif
    case QEvent::Wheel:
        return static_cast<const QWheelEvent*>(event)->globalPosition();
    case QEvent::NativeGesture:
        return static_cast<const QNativeGestureEvent*>(event)->globalPos();
    default:
        if (QEvent::MouseButtonPress <= event->type() && event->type() <= QEvent::MouseMove) {
            return static_cast<const QMouseEvent*>(event)->globalPos();
        }
        return {};
    }
#endif
}

#endif // SNOW_COMPAT_HAS_GUI

// ---------------------------------------------------------------------------
// 3b. QJsonValue::toInteger - introduced in Qt 6.0.
//
//     Qt5's QJsonValue stores everything as double and has no integer accessor;
//     Qt6's toInteger(defaultValue) returns the default for non-numeric values.
//     This reproduces the Qt6 semantics 1:1.
//     Mechanical rewrite rule R12:
//       <v>.toInteger(d)  ->  snowJsonInteger(<v>, d)
//       <v>.toInteger()   ->  snowJsonInteger(<v>)
// ---------------------------------------------------------------------------
#if SNOW_COMPAT_HAS_QT
#include <QJsonValue>

namespace snow::compat
{

[[nodiscard]] inline qint64 snowJsonInteger(const QJsonValue& value, qint64 defaultValue = 0)
{
#if SNOW_QT6
    // This line must stay as Qt6 spells it (toInteger), otherwise the shim makes itself
    // recursive through the mechanical rewrite.
    return value.toInteger(defaultValue);
#else
    return value.isDouble() ? static_cast<qint64>(value.toDouble()) : defaultValue;
#endif
}

} // namespace snow::compat

using snow::compat::snowJsonInteger;
#endif // SNOW_COMPAT_HAS_QT

// ---------------------------------------------------------------------------
// 3a. QDataStream::Version - the highest enumerator keeps moving.
//
//     Qt 6.11 added QDataStream::Qt_6_11; Qt 5.15's newest enumerator is Qt_5_15.
//     Hard-coding Qt_6_11 in a Qt5 build fails as
//       error C2039: "Qt_6_11": is not a member of "QDataStream"
//     Because the value is persisted into on-disk records, it cannot simply be
//     lowered everywhere: Qt6 records must keep the version they were written
//     with. So the shim answers "highest version this build supports" and every
//     call site stays version-agnostic.
//
//     Mechanical rewrite rule R13:
//       stream.setVersion(QDataStream::Qt_6_11)  ->  stream.setVersion(snowDataStreamVersion())
// ---------------------------------------------------------------------------
#if SNOW_COMPAT_HAS_QT
#include <QDataStream>

namespace snow::compat
{

[[nodiscard]] inline QDataStream::Version snowDataStreamVersion() noexcept
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 11, 0)
    return QDataStream::Qt_6_11;
#else
    return QDataStream::Qt_5_15;
#endif
}

} // namespace snow::compat

using snow::compat::snowDataStreamVersion;
#endif // SNOW_COMPAT_HAS_QT

// ---------------------------------------------------------------------------
// 3. QList::removeIf - introduced in Qt 6.1.
//
//    Mechanical rewrite rule R5:
//      list.removeIf(pred)      -> snow::compat::removeIf(list, pred)
//      positions.removeIf(pred) -> snow::compat::removeIf(positions, pred)
// ---------------------------------------------------------------------------
namespace snow::compat
{

template <typename Container, typename Predicate>
qsizetype removeIf(Container& container, Predicate predicate)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 1, 0)
    return container.removeIf(predicate);
#else
    // **每轮都要重新取 end()，不能缓存在循环外。** Qt5 的 erase() 让容器变短，
    // 缓存的 end 随即变成"当前 end 的下一格"：循环会在 it 已经走到新 end() 时
    // 仍然满足 it != 旧end，接着解引用一过尾位置并调 erase(it)。erase(iterator)
    // 内部展开成 erase(it, it + 1)，那个 it + 1 就是非法 aend，于是 debug 版 Qt5
    // 直接 qFatal：
    //   ASSERT failure in QVector::erase: "The specified iterator argument 'aend'
    //   is invalid", file qvector.h, line 905
    // Qt6 分支走的是容器自带的 removeIf，所以这条只在 Qt5 上炸，属于移植引入的缺陷。
    auto it = container.begin();
    qsizetype removed = 0;
    while (it != container.end()) {
        if (predicate(*it)) {
            it = container.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
#endif
}

// Associative containers are different: QHash::removeIf (Qt 6.1) hands the
// predicate an *iterator*, not an element reference, because a QHash element is
// not a std::pair and callers need key()/value(). The generic overload above
// passes `*it`, which fails with C2664 for call sites written against Qt6, so
// this more specialized overload keeps the Qt6 calling convention.
// (QMap, QSet and QMultiHash behave the same way; add an overload here if one of
// them ever needs removeIf.)
template <typename Key, typename Value, typename Predicate>
qsizetype removeIf(QHash<Key, Value>& container, Predicate predicate)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 1, 0)
    return container.removeIf(predicate);
#else
    qsizetype removed = 0;
    for (auto it = container.begin(); it != container.end();) {
        if (predicate(it)) {
            it = container.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
#endif
}

namespace detail
{
// Calls `functor` with no arguments, discarding the signal's payload. Kept as a
// separate template so that the call is *dependent* on the deduced argument pack:
// see the note at the call site in snowConnectSingleShot().
template <typename Functor, typename... Args>
void invokeDiscardingArguments(Functor& functor, Args&&...)
{
    static_assert(std::is_invocable_v<Functor&>,
                  "snowConnectSingleShot: the functor must be callable with the signal's "
                  "arguments, or with none of them");
    functor();
}
} // namespace detail

// ---------------------------------------------------------------------------
// 3b. Qt::SingleShotConnection - introduced in Qt 6.5.
//     Qt5 has no such connection flag, so the connection is emulated: the
//     functor disconnects itself before running. The handle is shared between
//     the caller and the lambda so it is released together with the connection.
//
//     Mechanical rewrite rule R25:
//       connect(s, sig, ctx, fn, Qt::SingleShotConnection)
//         -> snowConnectSingleShot(s, sig, ctx, fn)
// ---------------------------------------------------------------------------
template <typename Sender, typename Signal, typename Context, typename Functor>
QMetaObject::Connection snowConnectSingleShot(Sender sender, Signal signal, Context context,
                                             Functor functor)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    return QObject::connect(sender, signal, context, functor, Qt::SingleShotConnection);
#else
    auto connection = std::make_shared<QMetaObject::Connection>();
    *connection = QObject::connect(
        sender, signal, context,
        [connection, functor](auto&&... arguments) {
            QObject::disconnect(*connection);
            // Qt5 delivers the signal's full argument list here, while the caller
            // may have supplied a functor that takes none of them - a shape Qt6
            // accepts (a slot/functor may ignore trailing arguments) but Qt5
            // rejects at the call itself.
            //
            // The discarded branch must call a *dependent* function: a plain
            // `functor()` there is independent of the lambda's deduced pack, and
            // MSVC diagnoses it eagerly even in a discarded if-constexpr branch
            // ("function declaration must be available because none of the
            // arguments depends on the template parameters", C3889) - which is
            // exactly what a generic lambda such as `[](const auto&) {}` provokes.
            if constexpr (std::is_invocable_v<Functor&, decltype(arguments)...>) {
                functor(std::forward<decltype(arguments)>(arguments)...);
            } else {
                detail::invokeDiscardingArguments(
                    functor, std::forward<decltype(arguments)>(arguments)...);
            }
        });
    return *connection;
#endif
}

} // namespace snow::compat

// Call sites are plain business code that does not (and should not have to) know
// that this helper lives in a namespace: the header is force-included, so the
// name has to be reachable unqualified. removeIf is deliberately NOT exported
// here - every call site already spells it snow::compat::removeIf.
using snow::compat::snowConnectSingleShot;

// ---------------------------------------------------------------------------
// 3c. Key combinations: Qt6 changed the *element type* of QKeySequence and gave
//     QKeyEvent a keyCombination() accessor.
//
//     Qt5: QKeySequence::operator[](int) -> int      (key code | modifiers)
//          QKeyEvent has key() and modifiers(), no keyCombination()
//     Qt6: QKeySequence::operator[](qsizetype) -> QKeyCombination
//          QKeyEvent::keyCombination() -> QKeyCombination
//
//     So `QList<QKeyCombination> keys; keys.append(sequence[0]);` fails on Qt5
//     with "cannot convert argument 1 from int to const QKeyCombination&", and
//     `event->keyCombination()` fails outright.
//
//     Mechanical rewrite rules R35 / R36:
//       <seq>[<i>]            -> snowKeySequenceAt(<seq>, <i>)
//       <event>->keyCombination() -> snowKeyEventCombination(<event>)
// ---------------------------------------------------------------------------
#if SNOW_COMPAT_HAS_GUI
[[nodiscard]] inline QKeyCombination snowKeySequenceAt(const QKeySequence& sequence, int index)
{
#if SNOW_QT6
    return sequence[index];
#else
    // Qt5 stores the packed int that Qt6's fromCombined()/toCombined() pair was
    // introduced to replace, so this is a lossless re-interpretation.
    return QKeyCombination::fromCombined(sequence[index]);
#endif
}

[[nodiscard]] inline QKeyCombination snowKeyEventCombination(const QKeyEvent* event)
{
#if SNOW_QT6
    return event->keyCombination();
#else
    // Qt6 defines keyCombination() as exactly this pair of accessors.
    return QKeyCombination(event->modifiers(), static_cast<Qt::Key>(event->key()));
#endif
}
#endif // SNOW_COMPAT_HAS_GUI

// ---------------------------------------------------------------------------
// 4. QLatin1StringView - official name of QLatin1String since Qt 6.4.
// ---------------------------------------------------------------------------
#if SNOW_QT5
using QLatin1StringView = QLatin1String;
#endif

// ---------------------------------------------------------------------------
// 5. QMetaType / QVariant type queries.
//
//    Mechanical rewrite rule R7: QMetaType::fromType<T>() -> snowMetaType<T>()
//    Mechanical rewrite rule R8: value.metaType()         -> snowMetaTypeOf(value)
// ---------------------------------------------------------------------------
template <typename T>
[[nodiscard]] inline QMetaType snowMetaType()
{
#if SNOW_QT6
    return QMetaType::fromType<T>();
#else
    // Qt5: relies on Q_DECLARE_METATYPE registration; both built-in and declared
    // types resolve through qMetaTypeId.
    return QMetaType(qMetaTypeId<T>());
#endif
}

[[nodiscard]] inline QMetaType snowMetaTypeOf(const QVariant& value)
{
#if SNOW_QT6
    return value.metaType();
#else
    return QMetaType(value.userType());
#endif
}

// ---------------------------------------------------------------------------
// 6. Miscellaneous Qt6-only members that a header-level shim CAN cover.
//
//    Every entry below was confirmed present in Qt6 and absent from Qt 5.15.2
//    by reading this machine's Qt5 headers, not from memory. Nothing is added
//    speculatively - if it is not listed here, the call site needs a real edit.
// ---------------------------------------------------------------------------

// QWidget::nativeEvent() third parameter: Qt6 passes qintptr*, Qt5 passes long*
// (Qt 5.15.2 QtWidgets/qwidget.h). qintptr itself exists in Qt5 too - it is just
// not the signature the virtual function uses - so including QtGlobal is not
// enough. Mechanical rewrite rule R12:
//     nativeEvent(..., qintptr *result) -> nativeEvent(..., snowNativeEventResult *result)
#if SNOW_QT6
using snowNativeEventResult = qintptr;
#else
using snowNativeEventResult = long;
#endif

// QAbstractItemModel::dataChanged() takes its role list as QVector<int> in Qt5
// and as QList<int> in Qt6. QVector is a distinct class in Qt5 (verified in
// Qt 5.15.2 QtCore/qvector.h - no alias), so the two are NOT interchangeable
// there. This affects both explicit QOverload<...> spellings and direct calls.
#if SNOW_QT6
using SnowRoleList = QList<int>;
#else
using SnowRoleList = QVector<int>;
#endif

#if SNOW_COMPAT_HAS_GUI
#  include <QFont>
#  include <QImageIOHandler>
#  include <QImageReader>
#  include <QPainter>
#  include <QPixmap>
#  include <QWindow>

// QPainterStateGuard is Qt 6.0+; Qt 5.15 has no such class (nothing in
// QtGui/qpainter.h matches "StateGuard"), so `#include <QPainterStateGuard>`
// fails with C1083 and the RAII usage does not exist at all.
//
// This is the Qt6 contract as used by this tree: save() in the constructor,
// restore() in the destructor unless restore()/release() was called first.
// The pointer overload exists because the tree constructs it both ways
// (`QPainterStateGuard guard(painter)` and `... guard(&painter)`), including
// through std::optional::emplace.
#if SNOW_QT5
class QPainterStateGuard
{
public:
    enum InitialState { Save, NoSave };

    explicit QPainterStateGuard(QPainter& painter, InitialState initial = Save)
        : m_painter(initial == Save ? &painter : nullptr)
    {
        if (m_painter != nullptr) {
            m_painter->save();
        }
    }

    explicit QPainterStateGuard(QPainter* painter, InitialState initial = Save)
        : m_painter(initial == Save ? painter : nullptr)
    {
        if (m_painter != nullptr) {
            m_painter->save();
        }
    }

    ~QPainterStateGuard() { restore(); }

    QPainterStateGuard(const QPainterStateGuard&) = delete;
    QPainterStateGuard& operator=(const QPainterStateGuard&) = delete;

    void restore()
    {
        if (m_painter != nullptr) {
            m_painter->restore();
            m_painter = nullptr;
        }
    }

    void release() { m_painter = nullptr; }

private:
    QPainter* m_painter = nullptr;
};
#endif // SNOW_QT5

// QPixmap::deviceIndependentSize() is Qt 6.2+; Qt5 requires the manual division.
// Free function because a member cannot be injected into QPixmap.
[[nodiscard]] inline QSizeF snowDeviceIndependentSize(const QPixmap& pixmap)
{
    const qreal ratio = pixmap.devicePixelRatio();
    return ratio > 0.0 ? QSizeF(pixmap.size()) / ratio : QSizeF(pixmap.size());
}

// Font variation axes (Qt 6.7: QFont::setVariableAxis(QFont::Tag, float)) and
// OpenType features (Qt 6.1: QFont::setFeature(QFont::Tag, quint32)) do not exist
// in Qt 5.15 at all - grep over the installed QtGui/qfont.h finds no `Tag`,
// `setVariableAxis`, `setFeature` or `featureTags`. There is no Qt5 equivalent to
// fall back to: the Qt5 QFont::Weight enum uses the legacy 0..99 scale, so it
// cannot carry an arbitrary `wght` axis value such as 650, and Qt5 exposes no
// font-feature API whatsoever.
//
// Macros rather than inline functions, deliberately. The Qt5 expansion must not
// even *name* QFont::Tag (it does not exist there), while the Qt6 expansion has
// to stay byte-identical to the expression the project already compiles against
// Qt 6.12. A macro is the only form that satisfies both without guessing
// QFont::Tag's constructor overloads.
//
// Coverage caveat: the only callers live in the filter-tile-cache key test, which
// wants a non-default font so that QFont::operator== takes its slow path. Under
// Qt5 the font stays default there, so that particular coverage is lost - the
// test's actual assertion (cache hits must not allocate) still runs unchanged.
#if SNOW_QT6
#  define SNOW_FONT_SET_VARIABLE_AXIS(font, tag, value) \
      (font).setVariableAxis(QFont::Tag(tag), (value))
#  define SNOW_FONT_SET_FEATURE(font, tag, value) (font).setFeature(QFont::Tag(tag), (value))
#else
#  define SNOW_FONT_SET_VARIABLE_AXIS(font, tag, value) (void)(font)
#  define SNOW_FONT_SET_FEATURE(font, tag, value) (void)(font)
#endif
#endif // SNOW_COMPAT_HAS_GUI

// ---------------------------------------------------------------------------
// 6b. qHash() seed / result type for user-supplied overloads.
//
//     Qt6 widened qHash()'s seed parameter and return type to std::size_t; Qt5
//     uses uint. A user-supplied overload is selected by QHash's own internals,
//     so it must carry the signature of the Qt version it is compiled against.
//
//     Qt5 adds a second requirement: the seed MUST have a default value. Qt5's
//     generic fallback (QtCore/qhashfunctions.h:117) is
//         template <typename T> uint qHash(const T &t, uint seed)
//             noexcept(noexcept(qHash(t)))
//         { return qHash(t) ^ seed; }
//     so it re-dispatches through a ONE-argument qHash(t) call, which only
//     resolves if the user's overload is callable with a single argument. Qt6
//     dropped that trick and calls qHash(key, seed) straight through.
//
//     Hence a Qt6-era overload
//         std::size_t qHash(const Key &key, std::size_t seed)
//     must be spelled
//         SnowHashSeed qHash(const Key &key, SnowHashSeed seed = 0)
//     to compile and hash on both. The defaulted seed is harmless on Qt6.
// ---------------------------------------------------------------------------
#if SNOW_QT6
using SnowHashSeed = std::size_t;
#else
using SnowHashSeed = uint;
#endif

// ---------------------------------------------------------------------------
// 7. qHash() overloads for the QtCore geometry value types.
//
//    Qt 5.15 ships qHash() only for scalars, strings, containers, pointers and
//    a handful of QtGui types (QFont / QTransform / QMatrix / QUrl / ...).
//    Verified against this machine's kit with
//      grep -rhoE "qHash\((const )?Q[A-Za-z0-9_]+" QtCore/ QtGui/
//    -> QSize / QPoint / QRect / QMargins / QLine and their float variants
//       are ALL absent. Qt6 added them.
//
//    Consequence in Qt6-era code: a call such as
//      `::qHash(value.physicalSize, 0)`        // physicalSize is a QSize
//    fails on Qt5 with
//      error C2665: 'qHash': none of the overloads can convert all arguments
//    (MSVC then dumps every visible candidate from qhashfunctions.h).
//
//    Only the Qt5 direction needs shimming - Qt6 builds keep Qt6's own
//    overloads. Return type / seed type follow Qt5's uint convention; the bit
//    pattern is deliberately NOT required to equal Qt6's, only to be stable
//    and well distributed within a single build.
// ---------------------------------------------------------------------------
#if SNOW_QT5
namespace snow::compat
{
namespace detail
{
// One QHashCombine step - identical to Qt6's QtPrivate::QHashCombine (and
// therefore to the combiner used by Qt6's qHashMulti()).
[[nodiscard]] inline uint hashStep(uint seed, uint value) noexcept
{
    return seed ^ (value + 0x9e3779b9u + (seed << 6) + (seed >> 2));
}
} // namespace detail
} // namespace snow::compat

// Integer-valued geometry types (mirror Qt6's qHashMulti(seed, a, b, ...)).
[[nodiscard]] inline uint qHash(const QSize& key, uint seed = 0) noexcept
{
    return snow::compat::detail::hashStep(
        snow::compat::detail::hashStep(seed, static_cast<uint>(key.width())),
        static_cast<uint>(key.height()));
}

[[nodiscard]] inline uint qHash(const QPoint& key, uint seed = 0) noexcept
{
    return snow::compat::detail::hashStep(
        snow::compat::detail::hashStep(seed, static_cast<uint>(key.x())),
        static_cast<uint>(key.y()));
}

[[nodiscard]] inline uint qHash(const QMargins& key, uint seed = 0) noexcept
{
    return snow::compat::detail::hashStep(
        snow::compat::detail::hashStep(
            snow::compat::detail::hashStep(seed, static_cast<uint>(key.left())),
            static_cast<uint>(key.top())),
        snow::compat::detail::hashStep(static_cast<uint>(key.right()),
                                       static_cast<uint>(key.bottom())));
}

[[nodiscard]] inline uint qHash(const QRect& key, uint seed = 0) noexcept
{
    return snow::compat::detail::hashStep(
        snow::compat::detail::hashStep(
            snow::compat::detail::hashStep(seed, static_cast<uint>(key.x())),
            static_cast<uint>(key.y())),
        snow::compat::detail::hashStep(static_cast<uint>(key.width()),
                                       static_cast<uint>(key.height())));
}

[[nodiscard]] inline uint qHash(const QLine& key, uint seed = 0) noexcept
{
    return snow::compat::detail::hashStep(
        snow::compat::detail::hashStep(
            snow::compat::detail::hashStep(seed, static_cast<uint>(key.x1())),
            static_cast<uint>(key.y1())),
        snow::compat::detail::hashStep(static_cast<uint>(key.x2()),
                                       static_cast<uint>(key.y2())));
}

// Float-valued geometry types. Qt5 exports uint qHash(double, uint), so the
// fractional part is hashed rather than truncated away.
[[nodiscard]] inline uint qHash(const QSizeF& key, uint seed = 0) noexcept
{
    return snow::compat::detail::hashStep(
        snow::compat::detail::hashStep(seed, ::qHash(key.width())),
        ::qHash(key.height()));
}

[[nodiscard]] inline uint qHash(const QPointF& key, uint seed = 0) noexcept
{
    return snow::compat::detail::hashStep(
        snow::compat::detail::hashStep(seed, ::qHash(key.x())),
        ::qHash(key.y()));
}

[[nodiscard]] inline uint qHash(const QMarginsF& key, uint seed = 0) noexcept
{
    return snow::compat::detail::hashStep(
        snow::compat::detail::hashStep(
            snow::compat::detail::hashStep(seed, ::qHash(key.left())),
            ::qHash(key.top())),
        snow::compat::detail::hashStep(::qHash(key.right()), ::qHash(key.bottom())));
}

[[nodiscard]] inline uint qHash(const QRectF& key, uint seed = 0) noexcept
{
    return snow::compat::detail::hashStep(
        snow::compat::detail::hashStep(
            snow::compat::detail::hashStep(seed, ::qHash(key.x())),
            ::qHash(key.y())),
        snow::compat::detail::hashStep(::qHash(key.width()), ::qHash(key.height())));
}

[[nodiscard]] inline uint qHash(const QLineF& key, uint seed = 0) noexcept
{
    return snow::compat::detail::hashStep(
        snow::compat::detail::hashStep(
            snow::compat::detail::hashStep(seed, ::qHash(key.x1())),
            ::qHash(key.y1())),
        snow::compat::detail::hashStep(::qHash(key.x2()), ::qHash(key.y2())));
}

// Scoped enums as QHash / QSet keys: NOT shimmable from here.
//
// Qt 6.0 added an enum overload of qHash
//     template <typename T, std::enable_if_t<std::is_enum_v<T>, bool> = true>
//     constexpr uint qHash(T key, uint seed = 0) noexcept
// and Qt 5.15 has none, so `QSet<SomeEnum>` / `QHash<SomeEnum, V>` fails with
//   error C2665: "qHash": none of the overloads could convert all the argument types
//
// The obvious fix - declaring the same constrained template at global scope so
// ADL finds it for global-scope enums - does NOT work: it is ambiguous against
// Qt5's own generic qHash(const T&, uint) in qhashfunctions.h:117. Verified
// against the real 5.15.2 headers:
//   error C2668: "qHash": ambiguous call to overloaded function
//     candidate: uint qHash<Key,true>(T, uint) noexcept   [our shim]
//     candidate: uint qHash<Key>(const T &, uint) noexcept [Qt5]
// (Neither is more specialized: after reference removal both are `T`/`const T`,
// and [temp.deduct.partial] does not order them.)
//
// So each enum needs a NON-template overload - a non-template always wins over
// Qt5's template - declared in the same namespace as the enum so that ADL finds
// it. See snow_draw_engine_qt/snow_canvas_types.h (SnowCanvasTool) for the
// canonical example.
#endif // SNOW_QT5

// ---------------------------------------------------------------------------
// 8. Qt6-only APIs of QtCore types.
// ---------------------------------------------------------------------------

// Qt6's qHashMulti(). Qt5 has no equivalent, which breaks the project's own
// `std::size_t qHash(T, std::size_t)` helpers that combine sub-keys with it.
// Uses the same combination step as Qt6's QHashCombine.
// Mechanical rewrite rule R14: qHashMulti( -> snowHashMulti(
template <typename... Args>
[[nodiscard]] inline std::size_t snowHashMulti(std::size_t seed, const Args&... args) noexcept
{
    const auto step = [](std::size_t accumulated, const auto& value) -> std::size_t {
        return accumulated ^ (static_cast<std::size_t>(::qHash(value)) + 0x9e3779b9u +
                              (accumulated << 6) + (accumulated >> 2));
    };
    ((seed = step(seed, args)), ...);
    return seed;
}

// QEvent has no DevicePixelRatioChange in Qt5 - Qt6 added it for QWindow/QWidget
// device-pixel-ratio changes. Qt5 has no *publicly documented* replacement, but
// QWidget itself signals exactly this transition with QEvent::ScreenChangeInternal
// (= 216 in Qt 5.15.2 QtCore/qcoreevent.h), which is what QWidgetPrivate sends when
// a widget moves to a screen with a different ratio. Mapping to it keeps the
// DPR-refresh branches alive instead of turning them into dead code.
// Mechanical rewrite rule R17: QEvent::DevicePixelRatioChange -> snowDevicePixelRatioChange
#if SNOW_QT6
constexpr int snowDevicePixelRatioChange = QEvent::DevicePixelRatioChange;
#else
constexpr int snowDevicePixelRatioChange = QEvent::ScreenChangeInternal;
#endif

// Qt 6.0 renamed QLibraryInfo::location() to QLibraryInfo::path(); Qt 5.15.2 has
// only location() - its own header carries the marker
//   "static QString location(LibraryLocation); // ### Qt 6: consider renaming it to path()"
// (QtCore/qlibraryinfo.h:105), and grep over the installed 5.15.2 headers finds no
// `path(` at all. The enumerator (e.g. LibrariesPath) has the same meaning on both
// sides, so this is an exact mapping rather than an approximation.
//
// Deliberately a template: Qt6 carries two enums here (the current
// QLibraryInfo::LibraryLocation used by path(), plus the legacy
// QLibraryInfo::Qt6LibraryLocation used by the deprecated location()). Deduction
// forwards whatever the call site spelled, so this compiles whichever enum an
// installed kit exposes - no need to name it.
template <typename Location>
[[nodiscard]] inline QString snowLibraryInfoPath(Location location)
{
#if SNOW_QT6
    return QLibraryInfo::path(location);
#else
    return QLibraryInfo::location(location);
#endif
}

#if SNOW_COMPAT_HAS_GUI

#  include <QAccessible>
#  include <QColor>
#  include <QFont>
#  include <QIcon>
#  include <QPalette>
#  include <QPixmap>
#  include <QTouchEvent>
#  if SNOW_QT6
// QPointingDevice has no Qt5 counterpart at all (verified: Qt 5.15.2 ships
// QTouchDevice only). Needed by snowNativeGestureEvent() below.
#    include <QPointingDevice>
#  endif

// Qt6's QFont::resolveMask() is spelled resolve() in Qt5 (returns uint, public -
// verified in Qt 5.15.2 QtGui/qfont.h:300). Same semantics, so this is an exact
// mapping rather than an approximation.
// Mechanical rewrite rule R23: <f>.resolveMask() -> snowFontResolveMask(<f>)
[[nodiscard]] inline uint snowFontResolveMask(const QFont& font)
{
#if SNOW_QT6
    return static_cast<uint>(font.resolveMask());
#else
    return font.resolve();
#endif
}

// Symmetric setter. Qt6 exposes QFont::setResolveMask(); Qt 5.15 only has the
// public getter (resolve(), qfont.h:300) and keeps resolve_mask private
// (qfont.h:345) with no setter at all.
//
// Qt5 fallback is therefore a no-op: the "unresolve the size so the platform
// picks it again" behaviour is lost, and a font whose size was assigned stays
// explicitly sized. This only affects ThemeManager's "font scale back to 100%"
// path, which is cosmetic; everything else in that path still works because the
// size itself is set from the base font.
inline void snowSetFontResolveMask(QFont& font, uint mask)
{
#if SNOW_QT6
    font.setResolveMask(mask);
#else
    Q_UNUSED(font)
    Q_UNUSED(mask)
#endif
}

// Qt6 added QPointF overloads of QWidget/QWindow::mapFromGlobal() and
// mapToGlobal(); Qt 5.15 has only the QPoint forms (QtGui/qwindow.h,
// QtWidgets/qwidget.h) and QPointF has no implicit conversion to QPoint, so a
// QPointF argument fails with
//   error C2664: "QPoint QWindow::mapFromGlobal(const QPoint &) const":
//   cannot convert argument 1 from "const QPointF" to "const QPoint &"
//
// The Qt5 fallback is exact, not approximate: on Qt5 both mapFromGlobal() and
// mapToGlobal() are pure translations along the parent-window chain
// (QWindow::mapFromGlobal is `pos - d->globalPosition()`), so mapping the
// rounded point and then adding back the dropped fractional part gives the same
// result as Qt6's floating-point overload.
//
// Templated on the receiver so it covers QWidget and QWindow alike.
// Mechanical rewrite rules R33 / R34:
//   <w>->mapFromGlobal(<p>) -> snowMapFromGlobal(<w>, <p>)
//   <w>->mapToGlobal(<p>)   -> snowMapToGlobal(<w>, <p>)
template <typename Receiver>
[[nodiscard]] inline QPointF snowMapFromGlobal(const Receiver* receiver, const QPointF& point)
{
#if SNOW_QT6
    return receiver->mapFromGlobal(point);
#else
    const QPoint rounded = point.toPoint();
    return QPointF(receiver->mapFromGlobal(rounded)) + (point - QPointF(rounded));
#endif
}

template <typename Receiver>
[[nodiscard]] inline QPointF snowMapToGlobal(const Receiver* receiver, const QPointF& point)
{
#if SNOW_QT6
    return receiver->mapToGlobal(point);
#else
    const QPoint rounded = point.toPoint();
    return QPointF(receiver->mapToGlobal(rounded)) + (point - QPointF(rounded));
#endif
}

// Same story for the three-argument QWidget::mapTo(QWidget*, QPointF).
// Mechanical rewrite rule R33b:
//   <w>->mapTo(<parent>, <p>) -> snowMapTo(<w>, <parent>, <p>)
template <typename Receiver, typename Target>
[[nodiscard]] inline QPointF snowMapTo(const Receiver* receiver, const Target* target,
                                       const QPointF& point)
{
#if SNOW_QT6
    return receiver->mapTo(target, point);
#else
    const QPoint rounded = point.toPoint();
    return QPointF(receiver->mapTo(target, rounded)) + (point - QPointF(rounded));
#endif
}

// QWidget::mapTo(parent, pos) requires `parent` to be an ancestor of the
// receiver. When it is not, the QPoint overload in Qt 5.15 does not return a
// wrong value - it hits
//   Q_ASSERT_X(..., "QWidget::mapTo(...)", "parent must be in parent hierarchy")
// and aborts the process whole. Qt6 keeps the same contract.
//
// Several call sites map between widgets that only share a top-level window
// (a pinned window forwards input to a different host widget, a popup asks a
// sibling subtree for coordinates), which is exactly the case that aborts.
// Route those calls through this helper: keep mapTo() when the target really
// is an ancestor, otherwise fall back to the global coordinate space and say
// once which pair triggered it.
//
// Templated on both widgets so this header still parses in translation units
// that only see QtCore (it is force-included into every TU).
template <typename Receiver, typename Target>
[[nodiscard]] inline QPointF snowMapToWithinHierarchy(const Receiver* receiver,
                                                     const Target* target, const QPointF& point)
{
#if SNOW_QT6
    return receiver->mapTo(target, point);
#else
    if (receiver == nullptr || target == nullptr || receiver == target) {
        return point;
    }
    if (!target->isAncestorOf(receiver)) {
        static bool reported = false;
        if (!reported) {
            reported = true;
            qWarning("snowMapToWithinHierarchy: %s -> %s is not an ancestor of the receiver; "
                     "using global coordinates",
                     receiver->metaObject()->className(), target->metaObject()->className());
        }
        return snowMapFromGlobal(target, snowMapToGlobal(receiver, point));
    }
    const QPoint rounded = point.toPoint();
    return QPointF(receiver->mapTo(target, rounded)) + (point - QPointF(rounded));
#endif
}

// Qt 6.2 added QFlags::testAnyFlags(); Qt 5.15 only has testFlag() (single flag)
// - QtCore/qflags.h:151 is the whole API surface there.
// Mechanical rewrite rule R37:
//   <flags>.testAnyFlags(<mask>) -> snowTestAnyFlags(<flags>, <mask>)
template <typename Enum>
[[nodiscard]] inline bool snowTestAnyFlags(const QFlags<Enum>& flags, const QFlags<Enum>& mask)
{
#if SNOW_QT6
    return flags.testAnyFlags(mask);
#else
    // "any of the mask bits set", which is what testAnyFlags() means.
    return (flags & mask) != 0;
#endif
}

// Qt 6.9 added QWindow::safeAreaMargins() and QEvent::SafeAreaMarginsChange (the
// display-cutout / notch API). Qt 5.15 knows neither, and there is no Qt5 source
// for the information: the platform plugin simply never reported it.
//
// The Qt5 answers are therefore "no inset" and "this event never arrives", which
// is exactly the pre-6.9 behaviour, not a guess. Note the event value is -1: a
// real QEvent::Type could collide with an existing case label in a switch (the
// first attempt reused QEvent::ScreenChangeInternal, which collides with the
// explicit ScreenChangeInternal case those same switches already have).
#if SNOW_QT6
constexpr int snowSafeAreaMarginsChange = QEvent::SafeAreaMarginsChange;
#else
constexpr int snowSafeAreaMarginsChange = -1;
#endif

[[nodiscard]] inline QMargins snowSafeAreaMargins(const QWindow* window)
{
#if SNOW_QT6
    return window != nullptr ? window->safeAreaMargins() : QMargins();
#else
    Q_UNUSED(window)
    return {};
#endif
}

// Qt6 added QImageReader::effectiveSize(); Qt 5.15 has only size()
// (QtGui/qimagereader.h:91). The two differ in exactly two ways:
//   * effectiveSize() applies setScaledSize() - not used by any call site here;
//   * effectiveSize() applies the EXIF orientation when autoTransform() is on.
// So the Qt5 fallback is size() with the quarter-turn transposition replayed from
// QImageReader::transformation() (available since Qt 5.5, qimagereader.h:115).
//
// The enumeration is bit-encoded: TransformationRotate270 is
// TransformationRotate180 | TransformationRotate90, and the Mirror/Flip variants
// OR Rotate90 in as well (QtGui/qimageiohandler.h:104-112), so a single
// `& TransformationRotate90` test selects exactly the odd-quarter-turn cases -
// the ones whose width/height swap.
//
// Mechanical rewrite rule R32: <r>.effectiveSize() -> snowImageReaderEffectiveSize(<r>)
[[nodiscard]] inline QSize snowImageReaderEffectiveSize(QImageReader& reader)
{
#if SNOW_QT6
    return reader.effectiveSize();
#else
    QSize size = reader.size();
    if (reader.autoTransform() &&
        (reader.transformation() & QImageIOHandler::TransformationRotate90)) {
        size.transpose();
    }
    return size;
#endif
}

// Qt6 added QIcon::pixmap(const QSize&, qreal devicePixelRatio, Mode, State);
// Qt 5.15 only has (QSize, Mode, State), (int, int, Mode, State) and
// (QWindow*, QSize, Mode, State) - verified in QtGui/qicon.h. Fetch the pixmap at
// the physical pixel size instead and tag it with the ratio, which is what the
// Qt5 high-DPI documentation recommends.
//
// Mechanical rewrite rule R24:
//   <icon>.pixmap(<size>, <dpr>) -> snowIconPixmap(<icon>, <size>, <dpr>)
[[nodiscard]] inline QPixmap snowIconPixmap(const QIcon& icon, const QSize& size,
                                            qreal devicePixelRatio,
                                            QIcon::Mode mode = QIcon::Normal,
                                            QIcon::State state = QIcon::Off)
{
#if SNOW_QT6
    return icon.pixmap(size, devicePixelRatio, mode, state);
#else
    const QSize physicalSize(qRound(size.width() * devicePixelRatio),
                             qRound(size.height() * devicePixelRatio));
    QPixmap pixmap = icon.pixmap(physicalSize, mode, state);
    pixmap.setDevicePixelRatio(devicePixelRatio);
    return pixmap;
#endif
}

// Qt6 QColor accessors return float; Qt5 returns qreal (double). The mismatch
// breaks std::clamp(color.alphaF(), 0.0F, 1.0F), which requires one common type.
// Mechanical rewrite rule R13:
//   std::clamp(<c>.alphaF(), 0.0F, 1.0F) -> std::clamp(snowAlphaF(<c>), 0.0F, 1.0F)
[[nodiscard]] inline float snowAlphaF(const QColor& color)
{
    return static_cast<float>(color.alphaF());
}

// Qt6 QColor::getHslF()/getRgbF()/getHsvF() take float*; Qt5 takes qreal*.
// Always writes qreal so call sites need no version branch.
//
// Both overloads return void - verified against the pre-downgrade Qt6 source,
// which called QColor::getHslF() as a statement and never read a result, and
// against Qt5's own header (qcolor.h:202 `void getHslF(qreal*, qreal*, qreal*,
// qreal*)`). The helper therefore returns void too; giving it a bool result
// would have been a fabricated API.
inline void snowGetHslF(const QColor& color, qreal* hue, qreal* saturation,
                        qreal* lightness, qreal* alpha)
{
#if SNOW_QT6
    float h = 0.0F;
    float s = 0.0F;
    float l = 0.0F;
    float a = 0.0F;
    color.getHslF(&h, &s, &l, &a);
    if (hue != nullptr) {
        *hue = h;
    }
    if (saturation != nullptr) {
        *saturation = s;
    }
    if (lightness != nullptr) {
        *lightness = l;
    }
    if (alpha != nullptr) {
        *alpha = a;
    }
#else
    color.getHslF(hue, saturation, lightness, alpha);
#endif
}

// Qt 6.6 added the Accent colour role. Qt5 has no equivalent role; Highlight is
// the closest semantic peer and is what this project already uses to carry the
// brand colour (see theme_palette.cpp). This IS a semantic downgrade: a palette
// entry written to Accent on Qt6 lands on Highlight on Qt5.
// Mechanical rewrite rule R18: QPalette::Accent -> snowAccentRole
#if SNOW_QT6
constexpr QPalette::ColorRole snowAccentRole = QPalette::Accent;
#else
constexpr QPalette::ColorRole snowAccentRole = QPalette::Highlight;
#endif

// Qt 6.8 added QAccessibleAnnouncementEvent and QAccessible::AnnouncementPoliteness.
// Qt5 has neither. Emulated with a plain QAccessibleEvent of type Alert, which is
// the signalling path Qt5 screen readers already react to; politeness and message
// text are dropped because Qt5 cannot express them.
#if SNOW_QT5
namespace snow::compat
{
enum class AnnouncementPoliteness { Polite, Assertive };

class AnnouncementEvent : public QAccessibleEvent
{
public:
    AnnouncementEvent(QObject* object, const QString& message)
        : QAccessibleEvent(object, QAccessible::Alert)
    {
        Q_UNUSED(message)
    }

    void setPoliteness(AnnouncementPoliteness politeness) { Q_UNUSED(politeness) }
};
} // namespace snow::compat

using SnowAnnouncementPoliteness = snow::compat::AnnouncementPoliteness;
using SnowAccessibleAnnouncementEvent = snow::compat::AnnouncementEvent;
#else
using SnowAnnouncementPoliteness = QAccessible::AnnouncementPoliteness;
using SnowAccessibleAnnouncementEvent = QAccessibleAnnouncementEvent;
#endif

// Qt6 QTouchEvent::points() returns QList<QEventPoint>; Qt5 touchPoints() returns
// QList<QTouchEvent::TouchPoint> - different types, no common base. Both expose
// the first touch point's global position, so expose exactly that.
//
// Qt6 names it QEventPoint::globalPosition(); Qt5's QTouchEvent::TouchPoint has
// no globalPosF() member at all - the screen-coordinate accessor is screenPos()
// (verified in Qt 5.15.2 QtGui/qevent.h, which declares pos/scenePos/screenPos
// and their start*/last* variants, all returning QPointF).
[[nodiscard]] inline std::optional<QPointF> snowFirstTouchGlobalPos(const QTouchEvent* event)
{
    if (event == nullptr) {
        return std::nullopt;
    }
#if SNOW_QT6
    const QList<QEventPoint>& points = event->points();
    if (points.isEmpty()) {
        return std::nullopt;
    }
    return points.constFirst().globalPosition();
#else
    const QList<QTouchEvent::TouchPoint>& points = event->touchPoints();
    if (points.isEmpty()) {
        return std::nullopt;
    }
    return points.constFirst().screenPos();
#endif
}

// Qt6 appended a pointing device and a finger count to QNativeGestureEvent's
// constructor and dropped the trailing (sequenceId, intArgument) pair; Qt5 takes
// a QTouchDevice* instead. nullptr is accepted there and nothing in Snow Shot
// ever calls device() (the only device() call sites in the tree belong to
// QDataStream), so the Qt5 branch keeps the trailing pair at 0.
//
// Verified against the installed headers:
//   Qt 5.15.2 QtGui/qevent.h : (type, const QTouchDevice *dev, localPos, windowPos,
//                               screenPos, value, ulong sequenceId, quint64 intArgument)
//   Qt 6      QtGui/qevent.h : (type, const QPointingDevice *device, int fingerCount,
//                               localPos, scenePos, globalPos, value, delta,
//                               quint64 sequenceId = 0, quint64 intArgument = 0)
// The first three QPointFs map to local/window/screen in Qt5 and to
// local/scene/global in Qt6; snowPos() reads localPos() on both, which is the
// only member the gesture handlers consume.
[[nodiscard]] inline QNativeGestureEvent snowNativeGestureEvent(Qt::NativeGestureType type,
                                                              const QPointF& localPos,
                                                              const QPointF& windowPos,
                                                              const QPointF& screenPos,
                                                              qreal value)
{
#if SNOW_QT6
    return QNativeGestureEvent(type, QPointingDevice::primaryPointingDevice(), 2, localPos,
                               windowPos, screenPos, value, QPointF());
#else
    return QNativeGestureEvent(type, nullptr, localPos, windowPos, screenPos, value, 0, 0);
#endif
}

#endif // SNOW_COMPAT_HAS_GUI

// ---------------------------------------------------------------------------
// 9. Qt6-only APIs of QtWidgets types.
// ---------------------------------------------------------------------------
#if defined(__has_include)
#  if __has_include(<QWidget>)
#    define SNOW_COMPAT_HAS_WIDGETS 1
#  else
#    define SNOW_COMPAT_HAS_WIDGETS 0
#  endif
#else
#  define SNOW_COMPAT_HAS_WIDGETS 0
#endif

#if SNOW_COMPAT_HAS_WIDGETS

#  include <QAction>
#  include <QLayoutItem>
#  include <QMessageBox>
#  include <QScreen>
#  include <QWidget>
#  include <QWindow>

// Qt6 added QWidget::setScreen(). Qt5 only has QWindow::setScreen(), and
// windowHandle() returns nullptr until the widget has a native window - so make
// sure one exists first, otherwise the screen change would be silently dropped.
// Mechanical rewrite rule R19:
//   <w>->setScreen(<s>) / setScreen(<s>) -> snowSetWidgetScreen(<w>|this, <s>)
inline void snowSetWidgetScreen(QWidget* widget, QScreen* screen)
{
#if SNOW_QT6
    if (widget != nullptr) {
        widget->setScreen(screen);
    }
#else
    if (widget == nullptr || screen == nullptr) {
        return;
    }
    widget->createWinId();
    if (QWindow* handle = widget->windowHandle()) {
        handle->setScreen(screen);
    }
#endif
}

// Qt6 added QWidget::setAccessibleIdentifier(). Qt5 has no equivalent: the
// identifier is simply not conveyed to the platform accessibility layer.
// Mechanical rewrite rule R20:
//   <w>->setAccessibleIdentifier(<id>) -> snowSetAccessibleIdentifier(<w>, <id>)
inline void snowSetAccessibleIdentifier(QWidget* widget, const QString& identifier)
{
#if SNOW_QT6
    if (widget != nullptr) {
        widget->setAccessibleIdentifier(identifier);
    }
#else
    Q_UNUSED(widget)
    Q_UNUSED(identifier)
#endif
}

// Qt6 replaced QAction::associatedWidgets()/associatedGraphicsWidgets() with
// associatedObjects() (QObject*, so it can also cover graphics widgets).
// The Qt5 fallback covers plain widgets only - which is all this project's call
// sites use (they qobject_cast the result to a QWidget subclass).
// Mechanical rewrite rule R21: <a>->associatedObjects() -> snowAssociatedObjects(<a>)
[[nodiscard]] inline QList<QObject*> snowAssociatedObjects(const QAction* action)
{
    QList<QObject*> objects;
    if (action == nullptr) {
        return objects;
    }
#if SNOW_QT6
    return action->associatedObjects();
#else
    const QList<QWidget*> widgets = action->associatedWidgets();
    for (QWidget* widget : widgets) {
        objects.append(widget);
    }
    return objects;
#endif
}

// Qt6 added QMessageBox::Option and QMessageBox::setOption(Option). Qt 5.15.2's
// QMessageBox has neither - its header declares only the StandardButton /
// ButtonRole enums, the constructor's StandardButtons, and the static
// convenience functions (QtWidgets/qmessagebox.h), so
//   dialog.setOption(QMessageBox::Option::DontUseNativeDialog);
// fails with C2039 "setOption is not a member of QMessageBox" plus C2039 on the
// Option enumerator itself.
//
// The Qt5 branch is a faithful no-op rather than an approximation: on Qt5 a
// QMessageBox *instance* is always drawn by Qt itself - the native-dialog path
// exists only for the static convenience functions (and only on macOS, which
// this port does not target). The call site here builds an instance and calls
// exec(), so "do not use the native dialog" is already true.
// Mechanical rewrite rule R31:
//   <d>.setOption(QMessageBox::Option::DontUseNativeDialog)
//     -> snowMessageBoxDontUseNativeDialog(<d>)
inline void snowMessageBoxDontUseNativeDialog(QMessageBox& dialog)
{
#if SNOW_QT6
    dialog.setOption(QMessageBox::Option::DontUseNativeDialog);
#else
    Q_UNUSED(dialog)
#endif
}

// Qt6 turned QLayoutItem::widget() into a const member function. Qt 5.15.2's own
// header spells the split out explicitly (QtWidgets/qlayoutitem.h:78):
//     #if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
//         virtual QWidget *widget();
//     #else
//         virtual QWidget *widget() const;
//     #endif
// so on Qt5 the call is simply not available on a `const QLayoutItem*` and the
// const has to be cast away. Mechanical rewrite rule:
//   layoutItem->widget() -> snowLayoutItemWidget(layoutItem)
[[nodiscard]] inline QWidget* snowLayoutItemWidget(const QLayoutItem* item)
{
    if (item == nullptr) {
        return nullptr;
    }
#if SNOW_QT6
    return item->widget();
#else
    return const_cast<QLayoutItem*>(item)->widget();
#endif
}

#endif // SNOW_COMPAT_HAS_WIDGETS

// ---------------------------------------------------------------------------
// 10. QPromise (Qt 6.0+).
//
//     Qt6's QPromise<T> is a thin, move-only wrapper over QFutureInterface<T>:
//         start()        -> reportStarted()
//         addResult()    -> reportResult()
//         finish()       -> reportFinished()
//         setException() -> reportException()
//         isCanceled()   -> isCanceled()
//     Every one of those primitives is public API in Qt5
//     (QtCore/qfutureinterface.h: reportStarted/reportFinished/reportResult/
//     reportException/reportCanceled/isCanceled - verified in this kit), so the
//     Qt5 fallback is faithful, not approximate.
//
//     Two deliberate differences from Qt6, neither relied on by this project:
//       * addResult() copies instead of moving (Qt5's reportResult takes const T&);
//       * setException() takes std::exception_ptr or QException rather than the
//         Qt6 set of throw-like helpers, because Qt5's QFutureInterface has no
//         std::exception_ptr overload - an unknown exception type is wrapped in
//         QUnhandledException (which QFutureInterface can report).
// ---------------------------------------------------------------------------
#if SNOW_QT6
#  if defined(__has_include)
#    if __has_include(<QPromise>)
#      include <QPromise>
#    endif
#  endif
template <typename T> using SnowPromise = QPromise<T>;
#else
#  include <QException>
#  include <QFutureInterface>
#  include <QUnhandledException>
#  include <exception>

template <typename T> class SnowPromise
{
public:
    SnowPromise() = default;

    // Qt6 asserts when a QPromise is destroyed without finish(). Mirror that
    // discipline without throwing from a destructor.
    ~SnowPromise()
    {
        if (!m_finished) {
            m_interface.reportFinished();
        }
    }

    [[nodiscard]] QFuture<T> future() const { return m_interface.future(); }

    void start() { m_interface.reportStarted(); }

    void finish()
    {
        if (m_finished) {
            return;
        }
        m_finished = true;
        m_interface.reportFinished();
    }

    [[nodiscard]] bool isCanceled() const { return m_interface.isCanceled(); }

    void addResult(const T& value) { m_interface.reportResult(value); }

    void setException(const QException& exception) { m_interface.reportException(exception); }

    void setException(const std::exception_ptr& exception)
    {
        if (exception == nullptr) {
            return;
        }
        try {
            std::rethrow_exception(exception);
        } catch (const QException& known) {
            m_interface.reportException(known);
        } catch (...) {
            // QFutureInterface can only carry a QException; Qt5 has no
            // std::exception_ptr overload, so anything else is wrapped.
            m_interface.reportException(QUnhandledException());
        }
    }

private:
    // mutable: Qt6's QPromise::future() is const, and call sites rely on that
    // (they take SnowPromise by const reference), but Qt5's
    // QFutureInterface<T>::future() is a *non-const* member
    // (QtCore/qfutureinterface.h: "QFuture<T> future()" - no const qualifier),
    // so a const future() can only be implemented by mutating from a const this.
    mutable QFutureInterface<T> m_interface;
    bool m_finished = false;
};
#endif

// ---------------------------------------------------------------------------
// 11. QStringDecoder (Qt 6.0+ QStringConverter family).
//
//     Qt5's equivalent is QTextCodec, whose HTML/UTF-text sniffers are
//     QTextCodec::codecForHtml() / codecForUtfText() (QtCore/qtextcodec.h:74,77)
//     and whose error signal is ConverterState::invalidChars.
//
//     Usage is version-neutral from the call side:
//         SnowStringDecoder decoder = snowTextDecoder(bytes, isHtml);
//         const QString text = decoder.decode(bytes);
//         if (decoder.hasError()) { ... }
// ---------------------------------------------------------------------------
#if SNOW_QT6
#  if defined(__has_include)
#    if __has_include(<QStringConverter>)
#      include <QStringConverter>
#    endif
#  endif

class SnowStringDecoder
{
public:
    SnowStringDecoder() = default;
    explicit SnowStringDecoder(QStringDecoder decoder) : m_decoder(std::move(decoder)) {}

    [[nodiscard]] QString decode(const QByteArray& data) { return m_decoder.decode(data); }
    [[nodiscard]] bool hasError() const { return m_decoder.hasError(); }

private:
    QStringDecoder m_decoder;
};

[[nodiscard]] inline SnowStringDecoder snowTextDecoder(const QByteArray& data, bool html)
{
    const auto encoding = html ? QStringDecoder::encodingForHtml(data)
                               : QStringDecoder::encodingForData(data);
    return SnowStringDecoder(encoding.value_or(QStringDecoder::Utf8));
}
#else
#  include <QTextCodec>

class SnowStringDecoder
{
public:
    SnowStringDecoder() = default;
    explicit SnowStringDecoder(const QTextCodec* codec) : m_codec(codec) {}

    [[nodiscard]] QString decode(const QByteArray& data)
    {
        if (m_codec == nullptr) {
            m_error = true;
            return QString();
        }
        QTextCodec::ConverterState state;
        const QString text = m_codec->toUnicode(data.constData(), data.size(), &state);
        m_error = state.invalidChars != 0;
        return text;
    }

    [[nodiscard]] bool hasError() const { return m_error; }

private:
    const QTextCodec* m_codec = nullptr;
    bool m_error = false;
};

// Qt6's encodingForHtml()/encodingForData() return an optional and the call site
// resolves it with value_or(QStringDecoder::Utf8). Qt5's single-argument
// codecForHtml()/codecForUtfText() instead fall back to the *locale* codec, which
// would make the decoded text depend on the machine's locale -- on a GBK-configured
// host that silently changes what gets pasted. The two-argument overloads are
// therefore used with an explicit UTF-8 codec so that the Qt5 branch reproduces the
// Qt6 fallback exactly (Qt 5.15 QTextCodec::codecForMib(106) == UTF-8).
[[nodiscard]] inline SnowStringDecoder snowTextDecoder(const QByteArray& data, bool html)
{
    QTextCodec* const utf8 = QTextCodec::codecForMib(106);
    if (html) {
        return SnowStringDecoder(QTextCodec::codecForHtml(data, utf8));
    }
    return SnowStringDecoder(QTextCodec::codecForUtfText(data, utf8));
}
#endif

// ---------------------------------------------------------------------------
// 12. QTimeZone::fromSecondsAheadOfUtc (Qt 6.5+).
//     Qt5 spells the same construction as an explicit constructor:
//         explicit QTimeZone(int offsetSeconds);      // QtCore/qtimezone.h:94
// ---------------------------------------------------------------------------
[[nodiscard]] inline QTimeZone snowTimeZoneFromSecondsAheadOfUtc(int secondsAheadOfUtc)
{
#if SNOW_QT6
    return QTimeZone::fromSecondsAheadOfUtc(secondsAheadOfUtc);
#else
    return QTimeZone(secondsAheadOfUtc);
#endif
}

// ---------------------------------------------------------------------------
// 13. QChronoTimer (Qt 6.8+).
//
//     QChronoTimer is QTimer with a std::chrono interval API. Qt5 has no such
//     class (QtCore/QChronoTimer does not exist in this kit).
//
//     LOSSY DOWNGRADE: Qt5's QTimer has only millisecond resolution, so a
//     sub-millisecond interval is rounded UP to 1 ms (never to 0, which QTimer
//     treats as "stop"). The project's only user is the screenshot presentation
//     frame scheduler, which already falls back to a native timer when the
//     platform can arm one, so the rounding only affects the fallback path.
//
//     QChronoTimer's timeout signal is inherited unchanged, so
//         QObject::connect(&timer, &SnowChronoTimer::timeout, ...)
//     resolves to QTimer::timeout on Qt5 and to the real signal on Qt6.
// ---------------------------------------------------------------------------
#if SNOW_QT6 && defined(__has_include)
#  if __has_include(<QChronoTimer>)
#    include <QChronoTimer>
#    define SNOW_COMPAT_HAS_NATIVE_CHRONO_TIMER 1
#  else
#    define SNOW_COMPAT_HAS_NATIVE_CHRONO_TIMER 0
#  endif
#else
#  define SNOW_COMPAT_HAS_NATIVE_CHRONO_TIMER 0
#endif

#if SNOW_COMPAT_HAS_NATIVE_CHRONO_TIMER
using SnowChronoTimer = QChronoTimer;
#else
#  include <QTimer>
#  include <chrono>

class SnowChronoTimer : public QTimer
{
public:
    using QTimer::QTimer;
    using QTimer::setInterval;
    using QTimer::start;

    void setInterval(std::chrono::nanoseconds interval)
    {
        const qint64 nanoseconds = interval.count();
        const qint64 milliseconds = nanoseconds <= 0 ? 0 : (nanoseconds + 999999) / 1000000;
        QTimer::setInterval(static_cast<int>(milliseconds));
    }

    void start(std::chrono::nanoseconds interval)
    {
        setInterval(interval);
        QTimer::start();
    }

    [[nodiscard]] std::chrono::nanoseconds interval() const
    {
        return std::chrono::milliseconds(QTimer::interval());
    }
};
#endif

// ---------------------------------------------------------------------------
// 14. System colour scheme: Qt::ColorScheme + QStyleHints::colorScheme().
//
//     Introduced in Qt 6.5. Qt5 has neither: this kit's QtGui/qstylehints.h
//     contains no colorScheme member and QtCore/qnamespace.h has no ColorScheme
//     enumerator (both verified by grep against 5.15.2).
//
//     LOSSY DOWNGRADE. Qt5 offers no QStyleHints::colorScheme*C*hanged signal at
//     all, and a signal name cannot be shimmed with a free function because
//     connect() needs a pointer-to-member. Qt5 call sites must therefore
//     subscribe to QGuiApplication::paletteChanged() instead (manual edit), and
//     this helper derives the scheme from the application palette's window
//     colour - the same heuristic the project already uses for theme detection.
//     A Windows theme switch that does not refresh the application palette will
//     not be observed on Qt5.
// ---------------------------------------------------------------------------
#if SNOW_QT6
using SnowColorScheme = Qt::ColorScheme;
constexpr SnowColorScheme SnowColorSchemeUnknown = Qt::ColorScheme::Unknown;
constexpr SnowColorScheme SnowColorSchemeLight = Qt::ColorScheme::Light;
constexpr SnowColorScheme SnowColorSchemeDark = Qt::ColorScheme::Dark;
#else
enum class SnowColorScheme { Unknown, Light, Dark };
constexpr SnowColorScheme SnowColorSchemeUnknown = SnowColorScheme::Unknown;
constexpr SnowColorScheme SnowColorSchemeLight = SnowColorScheme::Light;
constexpr SnowColorScheme SnowColorSchemeDark = SnowColorScheme::Dark;
#endif

#if SNOW_COMPAT_HAS_GUI
#  include <QGuiApplication>
#  include <QObject>
#  include <QStyleHints>

[[nodiscard]] inline SnowColorScheme snowColorScheme(const QStyleHints* hints)
{
#if SNOW_QT6
    return hints != nullptr ? hints->colorScheme() : SnowColorSchemeUnknown;
#else
    Q_UNUSED(hints)
    const QColor windowColor = QGuiApplication::palette().color(QPalette::Window);
    return windowColor.lightness() < 128 ? SnowColorSchemeDark : SnowColorSchemeLight;
#endif
}

// ---------------------------------------------------------------------------
// 14b. Version-neutral connection for "the system colour scheme changed".
//
//     Qt 6.5 : QStyleHints::colorSchemeChanged(SnowColorScheme)      [signal]
//     Qt 5.15: QGuiApplication::paletteChanged(const QPalette&)      [signal]
//              -- verified at Qt 5.15.2 QtGui/qguiapplication.h:191. Qt5 has no
//                 QStyleHints::colorSchemeChanged at all, and a signal cannot be
//                 emulated with a free function because connect() needs a
//                 pointer-to-member, so call sites must be switched by hand.
//
//     The functor still takes a SnowColorScheme on both versions: the Qt5 branch
//     re-derives it from the application palette via snowColorScheme().
//
//     LOSSY BY DESIGN. On Qt5 *any* application-palette change re-invokes the
//     functor, not only an OS light/dark switch. Both call sites in this tree
//     re-apply the theme and are idempotent, so the extra wake-ups are harmless;
//     a site that is not idempotent must not use this helper.
// ---------------------------------------------------------------------------
template <typename Context, typename Functor>
QMetaObject::Connection snowConnectColorSchemeChanged(QStyleHints* hints, Context* context,
                                                     Functor functor)
{
#if SNOW_QT6
    return QObject::connect(hints, &QStyleHints::colorSchemeChanged, context, functor);
#else
    Q_UNUSED(hints)
    // qGuiApp is `static_cast<QGuiApplication*>(QCoreApplication::instance())`
    // (Qt 5.15.2 QtGui/qguiapplication.h:70). QGuiApplication::instance() alone
    // returns QCoreApplication*, which the connect() template rejects: it types the
    // sender as FunctionPointer<Func1>::Object*, i.e. exactly QGuiApplication*.
    return QObject::connect(qGuiApp, &QGuiApplication::paletteChanged, context,
                            [functor](const QPalette&) {
                                functor(snowColorScheme(QGuiApplication::styleHints()));
                            });
#endif
}
#endif // SNOW_COMPAT_HAS_GUI

// ---------------------------------------------------------------------------
// 15. QByteArrayView - introduced in Qt 6.0, no Qt5 peer.
//
//     Verified against this machine: Qt 5.15.2 has no QtCore/QByteArrayView
//     forwarding header and QtCore/qbytearray.h never mentions the name. Two
//     shapes are used in this tree, which is why BOTH a rewrite rule and a type
//     shim are needed:
//
//       (a) transient wrapper, 7 sites
//               hash.addData(QByteArrayView(ptr, len));
//           Qt5's QCryptographicHash offers only addData(const char*, int) and
//           addData(const QByteArray&) - no view overload
//           (QtCore/qcryptographichash.h:97-99). Rewrite rule R29 drops the
//           wrapper and pins the length: addData(ptr, static_cast<int>(len)).
//
//       (b) genuine parameter / member type, 6 sites in
//           snow_image_viewer/src/editing/worker_protocol.cpp
//           (crc32c(QByteArrayView), ValueReader::bytes_, locals). Those cannot
//           be rewritten away, so the class below implements exactly the subset
//           that file needs: construction from (const char*, qsizetype), from a
//           const char[] literal and from QByteArray, plus size() / data() /
//           empty() / operator[] / begin() / end(). Nothing else is claimed.
// ---------------------------------------------------------------------------
#if SNOW_QT5
#  include <QByteArray>

class QByteArrayView
{
public:
    constexpr QByteArrayView() noexcept = default;

    constexpr QByteArrayView(const char* data, qsizetype size) noexcept
        : m_data(data), m_size(size)
    {
    }

    QByteArrayView(const QByteArray& bytes) noexcept
        : m_data(bytes.constData()), m_size(bytes.size())
    {
    }

    // Qt6 stores N - 1 bytes for a literal: the terminating NUL is not part of
    // the view. Only const char[N] literals bind here, which is the form used in
    // this tree (QByteArrayView("\0", 1) has two arguments and therefore picks
    // the (const char*, qsizetype) overload instead).
    template <std::size_t N>
    constexpr QByteArrayView(const char (&literal)[N]) noexcept
        : m_data(literal), m_size(static_cast<qsizetype>(N) - 1)
    {
    }

    [[nodiscard]] constexpr const char* data() const noexcept { return m_data; }
    [[nodiscard]] constexpr qsizetype size() const noexcept { return m_size; }
    [[nodiscard]] constexpr bool empty() const noexcept { return m_size == 0; }
    [[nodiscard]] constexpr char operator[](qsizetype index) const { return m_data[index]; }
    [[nodiscard]] constexpr const char* begin() const noexcept { return m_data; }
    [[nodiscard]] constexpr const char* end() const noexcept { return m_data + m_size; }

private:
    const char* m_data = nullptr;
    qsizetype m_size = 0;
};
#endif // SNOW_QT5

// ---------------------------------------------------------------------------
// 16. Q_UNREACHABLE_RETURN - introduced in Qt 6.5 as a macro.
//
//     Qt 5.15 has Q_UNREACHABLE() (QtCore/qglobal.h) but no *_RETURN variant, so
//     a Qt6-era call site fails as
//       error C3861: "Q_UNREACHABLE_RETURN": identifier not found
//     There is nothing to shim as a function - it has to be a macro, because the
//     whole point is the `return` it injects into the caller's body. The Qt6
//     definition (qtcore/qcompilerdetection.h) is reproduced verbatim below, so
//     call sites keep their exact Qt6 semantics and need no rewrite.
//
//     This is intentionally defined even though the file is "compat helpers":
//     a macro is the only possible mechanism here.
// ---------------------------------------------------------------------------
#if SNOW_QT5 && !defined(Q_UNREACHABLE_RETURN)
#  define Q_UNREACHABLE_RETURN(...)                                                                \
      do {                                                                                         \
          Q_UNREACHABLE();                                                                         \
          return __VA_ARGS__;                                                                       \
      } while (false)
#endif

#endif // !SNOW_COMPAT_INERT
