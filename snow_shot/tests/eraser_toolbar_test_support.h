#pragma once

#include "physical_key_test_support.h"
#include "snow_shot/presentation/screenshotcanvastoolstyles.h"
#include "snow_shot/presentation/screenshottoolpalette.h"
#include "snow_shot/storage/settingsadapters.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "widgets/radio_button_group.h"
#include <QAbstractButton>
#include <QApplication>
#include <QLineEdit>
#include <QMouseEvent>
#include <QWheelEvent>

inline void verifyEraserToolbarHost(ScreenshotToolPalette& palette, SnowCanvasWidget& canvas,
                                    void (*require)(bool, const char*)) {
    using Tool = ScreenshotToolPalette::Tool;
    const snow_shot::storage::ScreenshotToolbarSettings settings;
    require(palette.activateDrawingShortcut(QStringLiteral("eraser")) &&
                canvas.canvasTool() == SnowCanvasTool::Eraser,
            "first host eraser activation preserves the whole-element default");
    const auto choose = [&](Tool tool) {
        auto* selector =
            palette.findChild<QWidget*>(QStringLiteral("screenshotEraserModeSelector"));
        auto* group =
            selector ? selector->findChild<adqt::widgets::AdRadioButtonGroup*>() : nullptr;
        require(group && group->button(static_cast<int>(tool)),
                "host eraser modes expose the secondary selector");
        group->button(static_cast<int>(tool))->click();
    };
    const auto mouse = [&](QEvent::Type type, QPointF point, Qt::MouseButton button,
                           Qt::MouseButtons buttons) {
        QMouseEvent event(type, point, canvas.mapToGlobal(snowWidgetPosition(point)), button, buttons, Qt::NoModifier);
        QApplication::sendEvent(&canvas, &event);
    };
    const auto gesture = [&](QPointF first, QPointF last) {
        mouse(QEvent::MouseButtonPress, first, Qt::LeftButton, Qt::LeftButton);
        mouse(QEvent::MouseMove, (first + last) / 2, Qt::NoButton, Qt::LeftButton);
        mouse(QEvent::MouseMove, last, Qt::NoButton, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, last, Qt::LeftButton, Qt::NoButton);
    };
    const auto pixel = [&](const QImage& image, QPointF point) {
        return image.pixelColor((point * canvas.devicePixelRatioF()).toPoint());
    };
    for (const auto tool : {Tool::RectangleEraser, Tool::BrushEraser}) {
        canvas.clearDocument();
        require(palette.activateDrawingShortcut(QStringLiteral("brush")),
                "host drawing shortcut activates a real annotation tool");
        gesture({40, 80}, {180, 80});
        const QImage before = canvas.grab().toImage();
        require(pixel(before, {100, 80}).alpha() > 200, "eraser fixture paints an opaque stroke");
        require(palette.activateDrawingShortcut(QStringLiteral("eraser")),
                "host generic eraser entry activates its remembered mode");
        choose(tool);
        const auto expected = tool == Tool::RectangleEraser ? SnowCanvasTool::RectangleEraser
                                                            : SnowCanvasTool::BrushEraser;
        require(canvas.canvasTool() == expected && palette.activeTool() == tool,
                "secondary eraser activation reaches the host canvas");
        gesture(tool == Tool::RectangleEraser ? QPointF(90, 60) : QPointF(90, 80),
                tool == Tool::RectangleEraser ? QPointF(120, 100) : QPointF(120, 80));
        const QImage erased = canvas.grab().toImage();
        require(pixel(erased, {100, 80}).alpha() <= 2 && pixel(erased, {50, 80}).alpha() > 200,
                "host erasers restore the transparent original only inside their geometry");
        require(canvas.undo() &&
                    pixel(canvas.grab().toImage(), {100, 80}) == pixel(before, {100, 80}),
                "host eraser undo restores the original annotation pixels");
        require(canvas.redo() && pixel(canvas.grab().toImage(), {100, 80}).alpha() <= 2,
                "host eraser redo restores the transparent patch");
    }
    canvas.clearDocument();
    require(palette.activateDrawingShortcut(QStringLiteral("brush")),
            "canvas reset shortcut works while a drawing tool is active");
    gesture({40, 80}, {180, 80});
    gesture({40, 120}, {180, 120});
    const auto activeTool = palette.activeTool();
    const auto canvasTool = canvas.canvasTool();
    int resets = 0;
    const auto connection = QObject::connect(&palette, &ScreenshotToolPalette::resetCanvasRequested,
                                             &canvas, [&]() { ++resets; });
    const auto key = [&](QWidget& receiver, Qt::Key code, Qt::KeyboardModifiers modifiers,
                         bool repeat = false) {
        PhysicalKeyEvent press(QEvent::KeyPress, code, modifiers, QString(), repeat);
        QApplication::sendEvent(&receiver, &press);
        PhysicalKeyEvent release(QEvent::KeyRelease, code, modifiers);
        QApplication::sendEvent(&receiver, &release);
    };
    canvas.window()->activateWindow();
    canvas.setFocus();
    QApplication::processEvents();
    key(canvas, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    require(resets == 1 && pixel(canvas.grab().toImage(), {100, 80}).alpha() <= 2 &&
                pixel(canvas.grab().toImage(), {100, 120}).alpha() <= 2 &&
                palette.activeTool() == activeTool && canvas.canvasTool() == canvasTool,
            "default reset shortcut clears every annotation without changing the active tool");
    require(canvas.undo() && pixel(canvas.grab().toImage(), {100, 80}).alpha() > 200 &&
                pixel(canvas.grab().toImage(), {100, 120}).alpha() > 200,
            "one undo restores every annotation removed by the reset shortcut");
    require(canvas.redo() && pixel(canvas.grab().toImage(), {100, 80}).alpha() <= 2 &&
                canvas.undo(),
            "reset shortcut deletion can be redone and undone");
    const snow_shot::storage::DrawingShortcutSettings shortcuts;
    const auto original = shortcuts.shortcuts(QStringLiteral("reset_canvas"));
    require(
        shortcuts.setShortcuts(QStringLiteral("reset_canvas"), {QStringLiteral("Ctrl+Alt+F12")}),
        "rebind the reset shortcut while the canvas is open");
    // A fresh edit retires redo of the reset before testing the old binding;
    // the canvas may still handle Ctrl+Shift+Z as its standard redo command.
    gesture({40, 160}, {180, 160});
    key(canvas, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    require(resets == 1 && pixel(canvas.grab().toImage(), {100, 80}).alpha() > 200,
            "rebinding reset removes its default shortcut immediately");
    {
        QLineEdit editor(&canvas);
        editor.show();
        canvas.window()->activateWindow();
        editor.setFocus();
        QApplication::processEvents();
        require(editor.hasFocus(), "reset shortcut fixture must focus the text input");
        key(editor, Qt::Key_F12, Qt::ControlModifier | Qt::AltModifier);
        require(resets == 1 && pixel(canvas.grab().toImage(), {100, 80}).alpha() > 200,
                "text inputs retain reset shortcut key events without clearing annotations");
    }
    canvas.setFocus();
    QApplication::processEvents();
    key(canvas, Qt::Key_F12, Qt::ControlModifier | Qt::AltModifier, true);
    require(resets == 1, "held reset shortcuts must not repeat the deletion");
    key(canvas, Qt::Key_F12, Qt::ControlModifier | Qt::AltModifier);
    require(resets == 2 && pixel(canvas.grab().toImage(), {100, 80}).alpha() <= 2 && canvas.undo(),
            "custom reset shortcut takes effect immediately and remains undoable");
    require(shortcuts.setShortcuts(QStringLiteral("reset_canvas"), {}),
            "disable the reset shortcut while the canvas is open");
    key(canvas, Qt::Key_F12, Qt::ControlModifier | Qt::AltModifier);
    require(resets == 2 && pixel(canvas.grab().toImage(), {100, 80}).alpha() > 200,
            "disabled reset shortcuts preserve the annotations");
    require(shortcuts.setShortcuts(QStringLiteral("reset_canvas"), original),
            "restore the reset shortcut after checking the host");
    QObject::disconnect(connection);
    require(palette.activateDrawingShortcut(QStringLiteral("eraser")),
            "restore the active brush eraser after testing reset");
    const double width = canvas.canvasStyleToolbarState().brushEraserStyle.strokeWidth;
    const QPointF point(100, 100);
    QWheelEvent wheel(point, canvas.mapToGlobal(point.toPoint()), QPoint(), QPoint(0, 120),
                      Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&canvas, &wheel);
    require(
        canvas.canvasStyleToolbarState().brushEraserStyle.strokeWidth == width + 1 &&
            snow_shot::presentation::screenshotCanvasToolStyleDefaults().brushEraser.strokeWidth ==
                width + 1,
        "host canvas wheel applies and persists the independent brush eraser width");
    require(palette.activateDrawingShortcut(QStringLiteral("eraser")) &&
                canvas.canvasTool() == SnowCanvasTool::Select &&
                settings.lastEraserTool() == QStringLiteral("brush-eraser"),
            "repeated eraser activation selects without losing the host mode");
    require(palette.activateDrawingShortcut(QStringLiteral("eraser")) &&
                canvas.canvasTool() == SnowCanvasTool::BrushEraser,
            "host eraser entry restores its last selected variant");
}
