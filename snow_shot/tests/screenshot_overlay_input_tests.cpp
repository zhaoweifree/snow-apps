#include "snow_shot/presentation/screenshotoverlayeventsink.h"
#include "snow_shot/presentation/screenshotoverlaywindow.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"

#include <QApplication>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QTabletEvent>
#include <QWheelEvent>
#include <QWindow>
#include <private/qhighdpiscaling_p.h>
#include <qpa/qwindowsysteminterface.h>

#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

class InputSink final : public ScreenshotOverlayEventSink {
  public:
    bool selecting = true;
    bool inputAllowed = true;
    bool requireGenuineInput = false;
    QVector<QPointF> presses;
    QVector<QPointF> moves;
    QVector<QPointF> releases;
    QVector<QPointF> rightClicks;
    QVector<QPointF> doubleClicks;
    int wheels = 0;
    Qt::KeyboardModifiers modifiers = Qt::NoModifier;
    int modifierRecomputations = 0;
    int snapClears = 0;
    void updateOverlayModifiers(Qt::KeyboardModifiers value, bool recompute) override {
        modifiers = value;
        modifierRecomputations += recompute;
    }
    void clearOverlaySnapping() override {
        ++snapClears;
    }

    bool acceptOverlayInput(bool genuine) override {
        if (requireGenuineInput && genuine)
            inputAllowed = true;
        return inputAllowed;
    }
    bool shouldHandleOverlayMouseEvent(const ScreenshotOverlayWindow*, const QPointF&,
                                       bool) const override {
        return selecting;
    }
    void handleOverlayMousePress(ScreenshotOverlayWindow*, const QPointF& point) override {
        presses.append(point);
    }
    void handleOverlayMouseMove(ScreenshotOverlayWindow*, const QPointF& point) override {
        moves.append(point);
    }
    void handleOverlayMouseRelease(ScreenshotOverlayWindow*, const QPointF& point) override {
        releases.append(point);
    }
    ScreenshotOverlayRightClickResult handleOverlayRightClick(ScreenshotOverlayWindow*,
                                                              const QPointF& point) override {
        rightClicks.append(point);
        return ScreenshotOverlayRightClickResult::Handled;
    }
    bool handleRegionDoubleClick(ScreenshotOverlayWindow*, const QPointF& point) override {
        doubleClicks.append(point);
        return true;
    }
    bool handleOverlayWheel(ScreenshotOverlayWindow*, const QWheelEvent& event) override {
        ++wheels;
        require(event.pixelDelta() == QPoint(0, 2) && event.angleDelta() == QPoint(0, 4) &&
                    event.phase() == Qt::ScrollMomentum && event.timestamp() == 1234 &&
                    event.source() == Qt::MouseEventSynthesizedBySystem,
                "edge routing must preserve wheel gesture metadata");
        return true;
    }
    bool shouldBlockUnhandledOverlayKeyInput() const override {
        return false;
    }
    void raiseToolbarForCanvasInteraction() override {}
};

class InputCanvas final : public SnowCanvasWidget {
  public:
    int presses = 0;
    QPointF lastPress;

  protected:
    bool event(QEvent* event) override {
        if (event->type() == QEvent::MouseButtonPress) {
            ++presses;
            lastPress = static_cast<QMouseEvent*>(event)->position();
            require(static_cast<QMouseEvent*>(event)->timestamp() == 1234,
                    "edge routing must preserve mouse timestamps");
        }
        return SnowCanvasWidget::event(event);
    }
};
} // namespace

void runScreenshotOverlayInputTests() {
    InputSink sink;
    auto* canvas = new InputCanvas;
    ScreenshotOverlayWindow overlay(sink, canvas);
    const auto mouse = [&](QEvent::Type type, const QPointF& point, Qt::MouseButton button,
                           Qt::MouseButtons buttons, bool throughWindow = true) {
        const QPointF windowPoint = canvas->mapTo(&overlay, point);
        QMouseEvent event(type, windowPoint, windowPoint, canvas->mapToGlobal(point), button,
                          buttons, Qt::ShiftModifier, Qt::MouseEventSynthesizedBySystem);
        event.setTimestamp(1234);
        QApplication::sendEvent(throughWindow ? static_cast<QObject*>(overlay.windowHandle())
                                              : static_cast<QObject*>(&overlay),
                                &event);
    };
    QPointingDevice pen(QStringLiteral("edge tablet"), 95, QInputDevice::DeviceType::Stylus,
                        QPointingDevice::PointerType::Pen,
                        QInputDevice::Capability::Position | QInputDevice::Capability::Pressure, 1,
                        3);
    for (const QRect display : {QRect(80, 60, 320, 240), QRect(-320, -240, 320, 240)}) {
        for (int surface = 0; surface < 2; ++surface) {
            overlay.setCaptureGeometry(display);
            overlay.restoreNativeSurface();
            overlay.show();
            QApplication::processEvents();
            require(canvas->size() == display.size() &&
                        canvas->mapToGlobal(QPoint()) == display.topLeft(),
                    "input routing must preserve the capture viewport and origin");
            // The last physical pixel occupies a fractional logical coordinate at scaled DPRs.
            for (const qreal inset : {1.0, 0.8, 2.0 / 3.0, 4.0 / 7.0, 0.5, 0.4, 0.01}) {
                for (const QPointF point : {QPointF(320 - inset, 120), QPointF(160, 240 - inset),
                                            QPointF(320 - inset, 240 - inset)}) {
                    sink.presses.clear();
                    sink.moves.clear();
                    sink.releases.clear();
                    const QPointF end = point - QPointF(20, 20);
                    mouse(QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton);
                    require(sink.modifiers == Qt::ShiftModifier,
                            "selection input must retain mouse modifiers");
                    mouse(QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
                    mouse(QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
                    require(sink.presses == QVector<QPointF>{point} &&
                                sink.moves == QVector<QPointF>{end} &&
                                sink.releases == QVector<QPointF>{end},
                            "an edge press must own the complete selection drag exactly once");
                    sink.moves.clear();
                    mouse(QEvent::MouseMove, point, Qt::NoButton, Qt::NoButton);
                    require(sink.moves == QVector<QPointF>{point},
                            "edge hover must reach pointer presentation exactly once");
                    sink.rightClicks.clear();
                    sink.doubleClicks.clear();
                    mouse(QEvent::MouseButtonPress, point, Qt::RightButton, Qt::RightButton);
                    mouse(QEvent::MouseButtonRelease, point, Qt::RightButton, Qt::NoButton);
                    mouse(QEvent::MouseButtonDblClick, point, Qt::LeftButton, Qt::LeftButton);
                    mouse(QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton);
                    require(sink.rightClicks == QVector<QPointF>{point} &&
                                sink.doubleClicks == QVector<QPointF>{point},
                            "right clicks and completion clicks must reach the display edges");
                }
            }
            const QPointF edge(319.75, 239.75);
            const QPointF windowEdge = canvas->mapTo(&overlay, edge);
            sink.presses.clear();
            sink.moves.clear();
            sink.releases.clear();
            const QPointF outside(340, 260);
            mouse(QEvent::MouseButtonPress, edge, Qt::LeftButton, Qt::LeftButton);
            mouse(QEvent::MouseMove, outside, Qt::NoButton, Qt::LeftButton);
            mouse(QEvent::MouseButtonRelease, outside, Qt::LeftButton, Qt::NoButton);
            require(sink.presses == QVector<QPointF>{edge} &&
                        sink.moves == QVector<QPointF>{outside} &&
                        sink.releases == QVector<QPointF>{outside},
                    "an edge drag must retain its implicit grab outside the canvas");
            const int wheels = sink.wheels;
            QWheelEvent wheel(windowEdge, canvas->mapToGlobal(edge), QPoint(0, 2), QPoint(0, 4),
                              Qt::NoButton, Qt::NoModifier, Qt::ScrollMomentum, false,
                              Qt::MouseEventSynthesizedBySystem);
            wheel.setTimestamp(1234);
            QApplication::sendEvent(overlay.windowHandle(), &wheel);
            require(sink.wheels == wheels + 1, "the edge must receive one wheel gesture");

            sink.presses.clear();
            sink.releases.clear();
            for (const auto type : {QEvent::TabletPress, QEvent::TabletRelease}) {
                QTabletEvent tablet(type, &pen, windowEdge, canvas->mapToGlobal(edge), 0.6, 4, 5, 0,
                                    0, 0, Qt::NoModifier, Qt::LeftButton,
                                    type == QEvent::TabletPress ? Qt::LeftButton : Qt::NoButton);
                QApplication::sendEvent(overlay.windowHandle(), &tablet);
            }
            require(sink.presses == QVector<QPointF>{edge} &&
                        sink.releases == QVector<QPointF>{edge},
                    "tablet edge clicks must use the same canvas coordinates as mouse clicks");
            require(sink.modifiers == Qt::NoModifier,
                    "tablet modifiers must reach selection input");
            const int recomputations = sink.modifierRecomputations;
            QKeyEvent controlPress(QEvent::KeyPress, Qt::Key_Control, Qt::ControlModifier);
            QApplication::sendEvent(canvas, &controlPress);
            require(sink.modifiers.testFlag(Qt::ControlModifier) &&
                        sink.modifierRecomputations == recomputations + 1,
                    "Ctrl press requests a drag recomputation");
            QKeyEvent controlRelease(QEvent::KeyRelease, Qt::Key_Control, Qt::ControlModifier);
            QApplication::sendEvent(canvas, &controlRelease);
            require(!sink.modifiers.testFlag(Qt::ControlModifier) &&
                        sink.modifierRecomputations == recomputations + 2,
                    "Ctrl release clears the modifier even if Qt includes it in the release mask");
            const int clears = sink.snapClears;
            QFocusEvent focusLost(QEvent::FocusOut);
            QApplication::sendEvent(canvas, &focusLost);
            require(sink.snapClears > clears, "focus loss must clear transient snap feedback");

            sink.selecting = false;
            const int presses = canvas->presses;
            mouse(QEvent::MouseButtonPress, edge, Qt::LeftButton, Qt::LeftButton);
            mouse(QEvent::MouseButtonRelease, edge, Qt::LeftButton, Qt::NoButton);
            require(canvas->presses == presses + 1 && canvas->lastPress == edge,
                    "an unhandled edge click must visit the disabled canvas only once");
            canvas->setInteractionEnabled(true);
            require(canvas->setCanvasTool(SnowCanvasTool::FreeDraw), "activate edge drawing");
            mouse(QEvent::MouseButtonPress, edge, Qt::LeftButton, Qt::LeftButton);
            mouse(QEvent::MouseMove, edge - QPointF(20, 20), Qt::NoButton, Qt::LeftButton);
            mouse(QEvent::MouseButtonRelease, edge - QPointF(20, 20), Qt::LeftButton, Qt::NoButton);
            require(canvas->canvasHistoryState().canUndo && canvas->undo() &&
                        !canvas->canvasHistoryState().canUndo,
                    "drawing from an edge must commit one complete canvas gesture");
            canvas->setInteractionEnabled(false);
            sink.selecting = true;

            sink.inputAllowed = false;
            sink.requireGenuineInput = true;
            sink.presses.clear();
            // Qt's QWindow routing marks widget input spontaneous. Send directly
            // to the overlay to exercise genuinely synthetic fallback input.
            mouse(QEvent::MouseButtonPress, edge, Qt::LeftButton, Qt::LeftButton, false);
            mouse(QEvent::MouseButtonRelease, edge, Qt::LeftButton, Qt::NoButton, false);
            require(sink.presses.isEmpty(),
                    "synthetic edge input must respect startup suppression");
            QWindow* window = overlay.windowHandle();
            for (const auto type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
                QWindowSystemInterface::handleMouseEvent<
                    QWindowSystemInterface::SynchronousDelivery>(
                    window, QHighDpi::toNativeLocalPosition(windowEdge, window),
                    QHighDpi::toNativePixels(canvas->mapToGlobal(edge), window),
                    type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton,
                    Qt::LeftButton, type);
            }
            require(sink.inputAllowed && sink.presses == QVector<QPointF>{edge},
                    "genuine edge input must retain its origin and resume startup input");
            sink.requireGenuineInput = false;
            overlay.releaseNativeSurface();
        }
    }
}
