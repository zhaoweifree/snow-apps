#include "snow_canvas_input_adapter.h"
#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"

#include <QApplication>
#include <QFocusEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMouseEvent>
#include <QPointingDevice>
#include <QTabletEvent>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

bool tablet(SnowCanvasWidget& canvas, const QPointingDevice& device, QEvent::Type type,
            QPointF point, double pressure, quint64 timestamp) {
    const auto button = type == QEvent::TabletMove ? Qt::NoButton : Qt::LeftButton;
    const auto buttons = type == QEvent::TabletRelease ? Qt::NoButton : Qt::LeftButton;
    QTabletEvent event(type, &device, point, canvas.mapToGlobal(point), pressure, 0, 0, 0, 0, 0,
                       Qt::NoModifier, button, buttons);
    event.setTimestamp(timestamp);
    event.setAccepted(false);
    QApplication::sendEvent(&canvas, &event);
    return event.isAccepted();
}

void mouse(SnowCanvasWidget& canvas, QEvent::Type type, QPointF point, Qt::MouseEventSource source,
           quint64 timestamp) {
    const auto button = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
    const auto buttons = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton;
    QMouseEvent event(type, point, point, canvas.mapToGlobal(point), button, buttons,
                      Qt::NoModifier, source);
    event.setTimestamp(timestamp);
    QApplication::sendEvent(&canvas, &event);
}

void adapterPreservesAvailablePressureAndTimestamps() {
    QPointingDevice pen(QStringLiteral("pressure tablet"), 42, QInputDevice::DeviceType::Stylus,
                        QPointingDevice::PointerType::Pen,
                        QInputDevice::Capability::Position | QInputDevice::Capability::Pressure, 1,
                        3);
    for (const auto pressure : {0.0, 0.25, 1.0, 1.4, -0.2}) {
        QTabletEvent event(QEvent::TabletMove, &pen, {20.5, 30.25}, {100.5, 130.25}, pressure, 0, 0,
                           0, 0, 0, Qt::ShiftModifier, Qt::NoButton, Qt::LeftButton);
        event.setTimestamp(1234);
        const auto input = snow_canvas_input::makePointerInput(event, SNOW_POINTER_EVENT_MOVE);
        require(input.pointer.device == SNOW_POINTER_DEVICE_PEN && input.pointer.pointer_id == 2 &&
                    input.pointer.has_pressure == 1 && input.pointer.timestamp_ms == 1234 &&
                    input.pointer.position_x == 20.5 && input.pointer.position_y == 30.25 &&
                    input.pointer.pressure == std::max(0.0, std::min(1.0, pressure)),
                "tablet samples preserve subpixel positions, time and normalized actual pressure");
    }
    QTabletEvent invalid(QEvent::TabletMove, &pen, {}, {}, std::numeric_limits<double>::quiet_NaN(),
                         0, 0, 0, 0, 0, Qt::NoModifier, Qt::NoButton, Qt::LeftButton);
    require(snow_canvas_input::makePointerInput(invalid, SNOW_POINTER_EVENT_MOVE)
                    .pointer.has_pressure == 0,
            "non-finite pressure is unavailable instead of entering stroke geometry");
    QPointingDevice plain(QStringLiteral("position tablet"), 43, QInputDevice::DeviceType::Stylus,
                          QPointingDevice::PointerType::Pen, QInputDevice::Capability::Position, 1,
                          3);
    QTabletEvent unavailable(QEvent::TabletPress, &plain, {}, {}, 0, 0, 0, 0, 0, 0, Qt::NoModifier,
                             Qt::LeftButton, Qt::LeftButton);
    require(snow_canvas_input::makePointerInput(unavailable, SNOW_POINTER_EVENT_DOWN)
                    .pointer.has_pressure == 0,
            "tablets without pressure capability use simulated pressure");
    QMouseEvent mouseEvent(QEvent::MouseMove, {12.5, 24.5}, {100, 100}, Qt::NoButton,
                           Qt::LeftButton, Qt::NoModifier);
    mouseEvent.setTimestamp(6789);
    const auto input = snow_canvas_input::makePointerInput(mouseEvent, SNOW_POINTER_EVENT_MOVE);
    require(input.pointer.device == SNOW_POINTER_DEVICE_MOUSE && input.pointer.has_pressure == 0 &&
                input.pointer.timestamp_ms == 6789,
            "mouse events preserve timing without pretending to have tablet pressure");
    require(snow_canvas_input::makePointerInput({}, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier,
                                                SNOW_POINTER_EVENT_DOWN)
                    .pointer.timestamp_ms == 0,
            "position-only synthetic input retains an unavailable timestamp");
}

void nativeStrokeCommitsOnceAndKeepsMouseUsable() {
    QPointingDevice pen(QStringLiteral("stroke tablet"), 44, QInputDevice::DeviceType::Stylus,
                        QPointingDevice::PointerType::Pen,
                        QInputDevice::Capability::Position | QInputDevice::Capability::Pressure, 1,
                        3);
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    require(canvas.hasTabletTracking(), "native tablet hover follows the brush endpoint cursor");
    canvas.resize(320, 240);
    canvas.show();
    QApplication::processEvents();
    require(canvas.setCanvasTool(SnowCanvasTool::FreeDraw), "activate the tablet brush fixture");
    auto style = canvas.canvasStyleToolbarState().shapeStyle;
    style.brushMode = SnowCanvasBrushMode::PressureSensitive;
    style.strokeWidth = 16;
    require(canvas.setCanvasShapeStylePatch(style,
                                            SnowCanvasShapeStylePropertyBrushMode |
                                                SnowCanvasShapeStylePropertyStrokeWidth,
                                            SnowCanvasShapeKind::FreeDraw),
            "select pressure-sensitive tablet drawing");
    quint32 pending = 0;
    quint32 dispatched = 0;
    QObject::connect(&canvas, &SnowCanvasWidget::freeDrawMoveBatchProcessed, &canvas,
                     [&](quint32 input, quint32 output) {
                         pending += input;
                         dispatched += output;
                     });
    require(tablet(canvas, pen, QEvent::TabletPress, {40, 70}, 0.2, 100),
            "native tablet brush press is accepted");
    for (int sample = 1; sample <= 5; ++sample) {
        require(tablet(canvas, pen, QEvent::TabletMove, {40.0 + sample * 35.0, 70},
                       0.2 + sample * 0.14, static_cast<quint64>(100 + sample * 5)),
                "native tablet samples enter the existing live stroke batch");
    }
    require(tablet(canvas, pen, QEvent::TabletRelease, {215, 70}, 0, 130),
            "native tablet release flushes and commits the brush stroke");
    require(pending == 5 && dispatched == 5 && runtime.hasDocumentContent() && runtime.canUndo(),
            "tablet release processes pending samples through one shared batch");
    double minimumPressure = 1.0;
    double maximumPressure = 0.0;
    const auto documentSlots = QJsonDocument::fromJson(runtime.serializeDocumentSession())
                                   .object()
                                   .value(QStringLiteral("document"))
                                   .toObject()
                                   .value(QStringLiteral("slots"))
                                   .toArray();
    for (const auto& slot : documentSlots) {
        const auto profiles = slot.toObject()
                                  .value(QStringLiteral("data"))
                                  .toObject()
                                  .value(QStringLiteral("FreeDraw"))
                                  .toObject()
                                  .value(QStringLiteral("pressure_segments"))
                                  .toArray();
        for (const auto& profile : profiles) {
            for (const auto& knot : profile.toArray()) {
                const auto pressure = knot.toObject().value(QStringLiteral("pressure")).toDouble();
                minimumPressure = std::min(minimumPressure, pressure);
                maximumPressure = std::max(maximumPressure, pressure);
            }
        }
    }
    require(minimumPressure <= 0.3 && maximumPressure >= 0.7,
            "batched native pen pressure reaches the persisted stroke profile");
    const auto revision = runtime.documentRevision();
    for (const auto source : {Qt::MouseEventSynthesizedByQt, Qt::MouseEventSynthesizedBySystem}) {
        mouse(canvas, QEvent::MouseButtonPress, {40, 70}, source, 100);
        mouse(canvas, QEvent::MouseMove, {215, 70}, source, 125);
        mouse(canvas, QEvent::MouseButtonRelease, {215, 70}, source, 130);
    }
    require(runtime.documentRevision() == revision,
            "synthesized mouse input after a consumed tablet stream cannot create a duplicate");
    mouse(canvas, QEvent::MouseButtonPress, {40, 200}, Qt::MouseEventSynthesizedByQt, 1000);
    mouse(canvas, QEvent::MouseMove, {215, 200}, Qt::MouseEventSynthesizedByQt, 1010);
    mouse(canvas, QEvent::MouseButtonRelease, {215, 200}, Qt::MouseEventSynthesizedByQt, 1020);
    require(runtime.documentRevision() > revision && canvas.undo() && runtime.hasDocumentContent(),
            "later mouse-only input retains its fallback after the tablet duplicate window");
    require(canvas.undo() && !runtime.hasDocumentContent() && canvas.redo() &&
                runtime.hasDocumentContent(),
            "one undo and redo operate on the complete tablet stroke");
    require(canvas.resetEditingStatePreservingTool(), "clear tablet selection for a mouse stroke");
    mouse(canvas, QEvent::MouseButtonPress, {40, 160}, Qt::MouseEventNotSynthesized, 200);
    mouse(canvas, QEvent::MouseMove, {215, 160}, Qt::MouseEventNotSynthesized, 215);
    mouse(canvas, QEvent::MouseButtonRelease, {215, 160}, Qt::MouseEventNotSynthesized, 220);
    require(canvas.undo() && runtime.hasDocumentContent() && canvas.undo() &&
                !runtime.hasDocumentContent(),
            "a real mouse remains usable after native tablet drawing and produces its own stroke");
}

void unhandledToolsAndCancelledTabletStreamsKeepTheirExistingBehavior() {
    QPointingDevice pen(QStringLiteral("cancel tablet"), 45, QInputDevice::DeviceType::Stylus,
                        QPointingDevice::PointerType::Pen,
                        QInputDevice::Capability::Position | QInputDevice::Capability::Pressure, 1,
                        3);
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    canvas.resize(320, 240);
    canvas.show();
    QApplication::processEvents();
    require(canvas.setCanvasTool(SnowCanvasTool::Shape), "activate a nonbrush tablet fallback");
    require(!tablet(canvas, pen, QEvent::TabletPress, {40, 60}, 0.5, 100) &&
                !runtime.hasDocumentContent(),
            "other tools leave tablet events unaccepted for Qt's existing mouse fallback");
    require(canvas.setCanvasTool(SnowCanvasTool::FreeDraw), "activate the cancellation brush");
    tablet(canvas, pen, QEvent::TabletPress, {40, 60}, 0.5, 200);
    tablet(canvas, pen, QEvent::TabletMove, {180, 60}, 0.7, 210);
    QFocusEvent focusLost(QEvent::FocusOut);
    QApplication::sendEvent(&canvas, &focusLost);
    tablet(canvas, pen, QEvent::TabletRelease, {180, 60}, 0, 220);
    require(!runtime.hasDocumentContent(), "losing focus cancels an unfinished tablet brush");
    tablet(canvas, pen, QEvent::TabletPress, {40, 60}, 0.5, 300);
    tablet(canvas, pen, QEvent::TabletMove, {180, 60}, 0.7, 310);
    QEvent ungrab(QEvent::UngrabMouse);
    QApplication::sendEvent(&canvas, &ungrab);
    tablet(canvas, pen, QEvent::TabletRelease, {180, 60}, 0, 320);
    require(!runtime.hasDocumentContent(), "losing native pointer capture cancels tablet drawing");
    tablet(canvas, pen, QEvent::TabletPress, {40, 60}, 0.5, 400);
    tablet(canvas, pen, QEvent::TabletMove, {180, 60}, 0.7, 410);
    canvas.hide();
    tablet(canvas, pen, QEvent::TabletRelease, {180, 60}, 0, 420);
    require(!runtime.hasDocumentContent(), "hiding the canvas cancels an unfinished tablet brush");
    canvas.show();
    tablet(canvas, pen, QEvent::TabletPress, {40, 60}, 0.5, 500);
    tablet(canvas, pen, QEvent::TabletMove, {180, 60}, 0.7, 510);
    qGuiApp->applicationStateChanged(Qt::ApplicationInactive);
    tablet(canvas, pen, QEvent::TabletRelease, {180, 60}, 0, 520);
    require(!runtime.hasDocumentContent(), "application deactivation cancels tablet drawing");
    qGuiApp->applicationStateChanged(Qt::ApplicationActive);
    tablet(canvas, pen, QEvent::TabletPress, {40, 60}, 0.5, 600);
    tablet(canvas, pen, QEvent::TabletMove, {180, 60}, 0.7, 610);
    canvas.setInteractionEnabled(false);
    canvas.setInteractionEnabled(true);
    tablet(canvas, pen, QEvent::TabletRelease, {180, 60}, 0, 620);
    require(!runtime.hasDocumentContent(),
            "disabling interaction cancels the engine draft before releasing native capture");
}

void barrelButtonsPreserveTheOwnedTipStroke() {
    QPointingDevice pen(QStringLiteral("barrel button tablet"), 46,
                        QInputDevice::DeviceType::Stylus, QPointingDevice::PointerType::Pen,
                        QInputDevice::Capability::Position | QInputDevice::Capability::Pressure, 1,
                        3);
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    canvas.resize(320, 240);
    canvas.show();
    QApplication::processEvents();
    require(canvas.setCanvasTool(SnowCanvasTool::FreeDraw), "activate the barrel button fixture");
    tablet(canvas, pen, QEvent::TabletPress, {40, 100}, 0.5, 100);
    tablet(canvas, pen, QEvent::TabletMove, {140, 100}, 0.7, 110);
    for (const auto type : {QEvent::TabletPress, QEvent::TabletRelease}) {
        const Qt::MouseButtons buttons =
            type == QEvent::TabletPress ? Qt::LeftButton | Qt::RightButton : Qt::LeftButton;
        QTabletEvent event(type, &pen, {140, 100}, canvas.mapToGlobal(QPoint(140, 100)), 0.7, 0, 0,
                           0, 0, 0, Qt::NoModifier, Qt::RightButton, buttons);
        event.setAccepted(false);
        QApplication::sendEvent(&canvas, &event);
        require(event.isAccepted() && !runtime.hasDocumentContent(),
                "barrel button changes cannot finish or duplicate an active pen tip stroke");
    }
    tablet(canvas, pen, QEvent::TabletMove, {215, 100}, 0.9, 120);
    tablet(canvas, pen, QEvent::TabletRelease, {215, 100}, 0, 130);
    require(runtime.hasDocumentContent() && canvas.undo() && !runtime.hasDocumentContent(),
            "lifting the tip commits the complete stroke once after barrel button changes");
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    adapterPreservesAvailablePressureAndTimestamps();
    nativeStrokeCommitsOnceAndKeepsMouseUsable();
    unhandledToolsAndCancelledTabletStreamsKeepTheirExistingBehavior();
    barrelButtonsPreserveTheOwnedTipStroke();
    return 0;
}
