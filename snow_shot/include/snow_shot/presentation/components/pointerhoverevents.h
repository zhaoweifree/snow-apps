#pragma once

#include "widgets/detail/pointer_region.h"

#include <QApplication>
#include <QEvent>
#include <QWidget>

#include <optional>

namespace snow_shot::presentation {

// Popup transitions can deliver hover movement without another Enter. Accept
// that entire stream, but only over the enabled receiver's visible input subtree.
inline std::optional<QPoint> pointerHoverPositionWithin(const QWidget* receiver,
                                                        const QEvent* event) {
    if (!receiver || !event || !receiver->isEnabled())
        return std::nullopt;
    switch (event->type()) {
    case QEvent::Enter:
    case QEvent::HoverEnter:
    case QEvent::HoverMove:
    case QEvent::MouseMove:
        break;
    default:
        return std::nullopt;
    }
    const auto position = adqt::widgets::detail::pointerEventGlobalPosition(receiver, event);
    if (!position ||
        !adqt::widgets::detail::pointerRegionContains(receiver, receiver->rect(), *position))
        return std::nullopt;
    // A grab or an intervening overlay can leave the event addressed to a
    // covered widget. Hit-test the delivered position, never the live cursor.
    if (!adqt::widgets::detail::pointerInWidgetTree(QApplication::widgetAt(*position), receiver))
        return std::nullopt;
    return position;
}

} // namespace snow_shot::presentation
