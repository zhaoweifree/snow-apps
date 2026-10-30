#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "snow_canvas_render_geometry.h"

#include <QApplication>
#include <QFocusEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLineF>
#include <QMouseEvent>
#include <QPainterPathStroker>

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void mouse(SnowCanvasWidget& canvas, QEvent::Type type, QPointF point, Qt::MouseButton button,
           Qt::MouseButtons buttons) {
    QMouseEvent event(type, point, point, point, button, buttons, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &event);
}

void key(SnowCanvasWidget& canvas, int code, Qt::KeyboardModifiers modifiers = Qt::NoModifier,
         const QString& text = {}) {
    QKeyEvent event(QEvent::KeyPress, code, modifiers, text);
    QApplication::sendEvent(&canvas, &event);
}

QJsonArray records(const SnowCanvasRuntime& runtime, const QString& kind) {
    const auto elements = QJsonDocument::fromJson(runtime.serializeDocumentSession())
                              .object()
                              .value(QStringLiteral("document"))
                              .toObject()
                              .value(QStringLiteral("slots"))
                              .toArray();
    QJsonArray result;
    for (const auto& slot : elements) {
        if (slot.toObject().value(QStringLiteral("data")).toObject().contains(kind)) {
            result.append(slot);
        }
    }
    return result;
}

QJsonObject payload(const SnowCanvasRuntime& runtime, const QString& kind) {
    const auto found = records(runtime, kind);
    require(found.size() == 1, "expected one element of requested kind");
    return found[0].toObject().value(QStringLiteral("data")).toObject().value(kind).toObject();
}

QPointF center(const QJsonObject& element) {
    const auto point = element.value(QStringLiteral("center")).toObject();
    return {point.value(QStringLiteral("x")).toDouble(),
            point.value(QStringLiteral("y")).toDouble()};
}

QPointF handle(const SnowCanvasWidget& canvas, const SnowCanvasRuntime& runtime) {
    const auto shape = payload(runtime, QStringLiteral("Rectangle"));
    const auto transform = canvas.canvasToViewTransform();
    const double zoom = std::hypot(transform.m11(), transform.m12());
    const double angle = shape.value(QStringLiteral("rotation")).toDouble();
    const double offset = shape.value(QStringLiteral("height")).toDouble() * zoom / 2.0 + 24.0;
    return transform.map(center(shape)) + QPointF(-std::sin(angle), std::cos(angle)) * offset;
}

void selectShape(SnowCanvasRuntime& runtime) {
    const auto id = records(runtime, QStringLiteral("Rectangle"))[0]
                        .toObject()
                        .value(QStringLiteral("id"))
                        .toObject();
    SnowCanvasRuntimeEditor editor(runtime);
    require(editor.select(id.value(QStringLiteral("index")).toInt(),
                          id.value(QStringLiteral("generation")).toInt()),
            "select shape");
}

void prepare(SnowCanvasWidget& canvas, SnowCanvasRectangleShape kind) {
    canvas.resize(800, 700);
    canvas.show();
    QApplication::processEvents();
    require(canvas.setViewportCamera(0, 0, 1), "set camera");
    require(canvas.setCanvasTool(SnowCanvasTool::Shape), "activate shape tool");
    SnowCanvasShapeStyle style;
    style.shape = kind;
    require(canvas.setCanvasShapeStylePatch(style, SnowCanvasShapeStylePropertyShape,
                                            SnowCanvasShapeKind::Rectangle),
            "set shape kind");
    mouse(canvas, QEvent::MouseButtonPress, {180, 150}, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseMove, {360, 250}, Qt::NoButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, {360, 250}, Qt::LeftButton, Qt::NoButton);
    require(!canvas.hasActiveTextEditing(), "ordinary shape creation does not edit text");
}

void assertConnectorExport(SnowCanvasRuntime& runtime) {
    auto session = QJsonDocument::fromJson(runtime.serializeDocumentSession()).object();
    auto document = session.value(QStringLiteral("document")).toObject();
    auto elements = document.value(QStringLiteral("slots")).toArray();
    for (qsizetype index = 0; index < elements.size(); ++index) {
        auto slot = elements[index].toObject();
        auto data = slot.value(QStringLiteral("data")).toObject();
        if (!data.contains(QStringLiteral("Rectangle"))) {
            continue;
        }
        auto shape = data.value(QStringLiteral("Rectangle")).toObject();
        shape.remove(QStringLiteral("text_element_id"));
        data.insert(QStringLiteral("Rectangle"), shape);
        slot.insert(QStringLiteral("data"), data);
        elements[index] = slot;
    }
    document.insert(QStringLiteral("slots"), elements);
    session.insert(QStringLiteral("document"), document);
    SnowCanvasRuntime unbound;
    session.insert(QStringLiteral("history"),
                   QJsonDocument::fromJson(unbound.serializeDocumentSession())
                       .object()
                       .value(QStringLiteral("history")));
    require(unbound.restoreDocumentSession(QJsonDocument(session).toJson()),
            "restore identical elements without binding");
    const QRectF region(-400, -350, 800, 700);
    const QSize size(800, 700);
    const auto image = runtime.renderToImage(region, size, {});
    require(!image.isNull() && image != unbound.renderToImage(region, size, {}),
            "export includes a persistent connector");
    SnowCanvasRuntime restored;
    require(restored.restoreDocumentSession(runtime.serializeDocumentSession()),
            "restore bound shape session");
    require(restored.renderToImage(region, size, {}) == image,
            "session roundtrip preserves text and connector export");
}

void clickDragEditAndReuse() {
    for (auto kind : {SnowCanvasRectangleShape::Rectangle, SnowCanvasRectangleShape::Ellipse,
                      SnowCanvasRectangleShape::Diamond}) {
        for (bool drag : {false, true}) {
            SnowCanvasRuntime runtime;
            SnowCanvasWidget canvas(runtime);
            SnowCanvasTextStyle textStyle;
            textStyle.fontSize = 19;
            textStyle.bold = true;
            textStyle.fill = QColor(Qt::white);
            require(canvas.setCanvasTextStyle(textStyle), "set label defaults");
            prepare(canvas, kind);
            selectShape(runtime);
            require(canvas.setCanvasTool(SnowCanvasTool::Shape), "keep shape tool active");
            const auto original = payload(runtime, QStringLiteral("Rectangle"));
            const auto start = handle(canvas, runtime);
            const QPointF end = drag ? QPointF(520, 410) : start;
            mouse(canvas, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
            require(records(runtime, QStringLiteral("Text")).isEmpty(),
                    "press previews text without changing the document");
            require(!canvas.hasActiveTextEditing(), "press does not open editor");
            if (drag) {
                mouse(canvas, QEvent::MouseMove, {470, 350}, Qt::NoButton, Qt::LeftButton);
                require(records(runtime, QStringLiteral("Text")).isEmpty(),
                        "drag remains transient");
            }
            mouse(canvas, QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
            require(canvas.hasActiveTextEditing() &&
                        canvas.testAttribute(Qt::WA_InputMethodEnabled),
                    "release opens bound text editor and enables input method");
            const auto label = payload(runtime, QStringLiteral("Text"));
            require(
                QLineF(center(label), canvas.canvasToViewTransform().inverted().map(end)).length() <
                    1e-6,
                "label uses final release position or click handle center");
            require(label.value(QStringLiteral("font_size")).toDouble() == 19 &&
                        label.value(QStringLiteral("bold")).toBool() &&
                        label.value(QStringLiteral("width")).toDouble() > 1,
                    "label preserves defaults and receives host measurement");
            auto shape = payload(runtime, QStringLiteral("Rectangle"));
            const auto textId = shape.take(QStringLiteral("text_element_id"));
            auto expected = original;
            expected.remove(QStringLiteral("text_element_id"));
            require(shape == expected, "binding preserves all shape geometry and styling");
            require(textId == records(runtime, QStringLiteral("Text"))[0].toObject().value(
                                  QStringLiteral("id")),
                    "shape binds the created label");
            key(canvas, Qt::Key_A, Qt::NoModifier, QStringLiteral("Bound label 中文"));
            key(canvas, Qt::Key_Return, Qt::ControlModifier);
            require(!canvas.hasActiveTextEditing(), "commit closes editor");
            require(
                payload(runtime, QStringLiteral("Text")).value(QStringLiteral("text")).toString() ==
                    QStringLiteral("Bound label 中文"),
                "typing edits bound text");
            assertConnectorExport(runtime);
            const auto textPoint = canvas.canvasToViewTransform().map(
                center(payload(runtime, QStringLiteral("Text"))));
            mouse(canvas, QEvent::MouseButtonPress, textPoint, Qt::LeftButton, Qt::LeftButton);
            mouse(canvas, QEvent::MouseButtonRelease, textPoint, Qt::LeftButton, Qt::NoButton);
            require(canvas.hasActiveTextEditing(), "selected label reopens under the shape tool");
            key(canvas, Qt::Key_Return, Qt::ControlModifier);

            selectShape(runtime);
            const auto again = handle(canvas, runtime);
            const QPointF relocated(600, 480);
            mouse(canvas, QEvent::MouseButtonPress, again, Qt::LeftButton, Qt::LeftButton);
            // No move event: the release itself must place the label.
            mouse(canvas, QEvent::MouseButtonRelease, relocated, Qt::LeftButton, Qt::NoButton);
            require(canvas.hasActiveTextEditing(), "reusing handle reopens editor");
            require(records(runtime, QStringLiteral("Text")).size() == 1 &&
                        payload(runtime, QStringLiteral("Rectangle"))
                                .value(QStringLiteral("text_element_id")) == textId,
                    "reusing handle preserves one label and its identity");
            require(QLineF(center(payload(runtime, QStringLiteral("Text"))),
                           canvas.canvasToViewTransform().inverted().map(relocated))
                            .length() < 1e-6,
                    "release without move events repositions existing label");
            key(canvas, Qt::Key_Return, Qt::ControlModifier);
        }
    }
}

void cancellationAndEmptyCommit() {
    for (bool existing : {false, true}) {
        for (bool focusLoss : {false, true}) {
            SnowCanvasRuntime runtime;
            SnowCanvasWidget canvas(runtime);
            prepare(canvas, SnowCanvasRectangleShape::Rectangle);
            selectShape(runtime);
            if (existing) {
                const auto start = handle(canvas, runtime);
                mouse(canvas, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
                mouse(canvas, QEvent::MouseButtonRelease, start, Qt::LeftButton, Qt::NoButton);
                key(canvas, Qt::Key_A, Qt::NoModifier, QStringLiteral("Keep me"));
                key(canvas, Qt::Key_Return, Qt::ControlModifier);
                selectShape(runtime);
            }
            const auto originalShape = payload(runtime, QStringLiteral("Rectangle"));
            const auto originalText = records(runtime, QStringLiteral("Text"));
            const auto revision = runtime.documentRevision();
            const auto start = handle(canvas, runtime);
            mouse(canvas, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
            mouse(canvas, QEvent::MouseMove, {500, 450}, Qt::NoButton, Qt::LeftButton);
            if (focusLoss) {
                QFocusEvent event(QEvent::FocusOut, Qt::OtherFocusReason);
                QApplication::sendEvent(&canvas, &event);
            } else {
                key(canvas, Qt::Key_Escape);
            }
            mouse(canvas, QEvent::MouseButtonRelease, {500, 450}, Qt::LeftButton, Qt::NoButton);
            require(!canvas.hasActiveTextEditing(), "cancelled gesture does not start editing");
            require(runtime.documentRevision() == revision &&
                        payload(runtime, QStringLiteral("Rectangle")) == originalShape &&
                        records(runtime, QStringLiteral("Text")) == originalText,
                    "cancellation leaves new or existing binding unchanged");
        }
    }
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    prepare(canvas, SnowCanvasRectangleShape::Ellipse);
    selectShape(runtime);
    const auto start = handle(canvas, runtime);
    mouse(canvas, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, start, Qt::LeftButton, Qt::NoButton);
    require(canvas.hasActiveTextEditing(), "click starts empty editor");
    key(canvas, Qt::Key_Escape);
    require(records(runtime, QStringLiteral("Text")).isEmpty() &&
                !payload(runtime, QStringLiteral("Rectangle"))
                     .value(QStringLiteral("text_element_id"))
                     .isObject(),
            "empty commit removes label and binding while preserving shape");
}

void connectorContourMatchesQtStroke() {
    for (const auto kind : {SNOW_DISPLAY_RECT_SHAPE_RECTANGLE, SNOW_DISPLAY_RECT_SHAPE_ELLIPSE,
                            SNOW_DISPLAY_RECT_SHAPE_DIAMOND}) {
        for (const QSizeF size : {QSizeF(100, 100), QSizeF(20, 200), QSizeF(200, 20)}) {
            for (const SnowCornerRadii radii :
                 {SnowCornerRadii{}, SnowCornerRadii{4, 4, 4, 4}, SnowCornerRadii{0, 4, 0, 8}}) {
                SnowSceneDisplayItem shape{};
                shape.kind = SNOW_SCENE_DISPLAY_ITEM_DRAW_RECT;
                shape.rect_shape = kind;
                shape.width = size.width();
                shape.height = size.height();
                shape.stroke_width = 72;
                shape.rotation = 0.7;
                shape.corner_radii = radii;
                const QRectF rect(-size.width() / 2, -size.height() / 2, size.width(),
                                  size.height());
                QPainterPath path;
                if (kind == SNOW_DISPLAY_RECT_SHAPE_DIAMOND) {
                    path =
                        snow_canvas_render_geometry::roundedDiamondPath(rect, shape.corner_radii);
                } else if (kind == SNOW_DISPLAY_RECT_SHAPE_ELLIPSE) {
                    path.addEllipse(rect);
                } else {
                    path = snow_canvas_render_geometry::roundedRectPath(
                        rect, snow_canvas_render_geometry::toViewCornerRadii(radii, 1.0, rect));
                }
                QPainterPathStroker stroker;
                stroker.setWidth(shape.stroke_width);
                stroker.setJoinStyle(Qt::MiterJoin);
                stroker.setCurveThreshold(0.001);
                const QPainterPath painted = path.united(stroker.createStroke(path));
                QTransform rotation;
                rotation.rotateRadians(shape.rotation);
                const QTransform inverse = rotation.inverted();
                for (const QPointF position :
                     {QPointF(350, 285), QPointF(-350, -285), QPointF(0, -400), QPointF(400, 0)}) {
                    SnowSceneDisplayItem text{};
                    text.kind = SNOW_SCENE_DISPLAY_ITEM_TEXT;
                    const QPointF center = rotation.map(position);
                    text.center_x = center.x();
                    text.center_y = center.y();
                    text.width = 100;
                    text.height = 30;
                    SnowSerialTextConnection connection{};
                    require(snow_scene_resolve_shape_text_connection(&shape, &text, &connection),
                            "resolve connector around Qt stroke");
                    const QPointF start =
                        inverse.map(QPointF(connection.start_x, connection.start_y));
                    const double length = std::hypot(start.x(), start.y());
                    const QPointF direction = start / length;
                    double low = 0;
                    double high = size.width() + size.height() + shape.stroke_width * 8;
                    for (int step = 0; step < 48; ++step) {
                        const double middle = (low + high) / 2;
                        if (painted.contains(direction * middle)) {
                            low = middle;
                        } else {
                            high = middle;
                        }
                    }
                    const double error = std::abs(length - shape.stroke_width / 2 - 8 - high);
                    if (error >= 0.25) {
                        std::cerr << "contour mismatch: kind=" << kind << " size=" << size.width()
                                  << 'x' << size.height() << " radii=" << radii.top_left << ','
                                  << radii.top_right << ',' << radii.bottom_right << ','
                                  << radii.bottom_left << " position=" << position.x() << ','
                                  << position.y()
                                  << " resolved edge=" << length - shape.stroke_width / 2 - 8
                                  << " Qt edge=" << high << '\n';
                    }
                    require(error < 0.25,
                            "connector round cap clears the Qt mitered or rounded outline");
                }
            }
        }
    }
}

void croppedExportPreservesConnectorToOffscreenLabel() {
    SnowCanvasRuntime source;
    SnowCanvasWidget canvas(source);
    prepare(canvas, SnowCanvasRectangleShape::Rectangle);
    selectShape(source);
    const auto start = handle(canvas, source);
    mouse(canvas, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, {520, 410}, Qt::LeftButton, Qt::NoButton);
    key(canvas, Qt::Key_A, Qt::NoModifier, QStringLiteral("Outside crop"));
    key(canvas, Qt::Key_Return, Qt::ControlModifier);
    for (const double ownerX : {-200.0, -900.0}) {
        auto session = QJsonDocument::fromJson(source.serializeDocumentSession()).object();
        auto document = session.value(QStringLiteral("document")).toObject();
        auto elements = document.value(QStringLiteral("slots")).toArray();
        for (qsizetype index = 0; index < elements.size(); ++index) {
            auto slot = elements[index].toObject();
            auto data = slot.value(QStringLiteral("data")).toObject();
            const QString kind = data.contains(QStringLiteral("Rectangle"))
                                     ? QStringLiteral("Rectangle")
                                     : QStringLiteral("Text");
            auto element = data.value(kind).toObject();
            element.insert(QStringLiteral("center"),
                           QJsonObject{{QStringLiteral("x"),
                                        kind == QStringLiteral("Rectangle") ? ownerX : 900.0},
                                       {QStringLiteral("y"), 0.0}});
            data.insert(kind, element);
            slot.insert(QStringLiteral("data"), data);
            elements[index] = slot;
        }
        document.insert(QStringLiteral("slots"), elements);
        session.insert(QStringLiteral("document"), document);
        SnowCanvasRuntime runtime;
        session.insert(QStringLiteral("history"),
                       QJsonDocument::fromJson(runtime.serializeDocumentSession())
                           .object()
                           .value(QStringLiteral("history")));
        require(runtime.restoreDocumentSession(QJsonDocument(session).toJson()),
                "restore crop fixture");
        const auto full =
            runtime.renderToImage(QRectF(-1200, -300, 2400, 600), QSize(2400, 600), {});
        const auto cropped =
            runtime.renderToImage(QRectF(-400, -300, 800, 600), QSize(800, 600), {});
        require(!full.isNull() && !cropped.isNull() && full.copy(800, 0, 800, 600) == cropped,
                "cropped export matches full export when label or both endpoints are offscreen");
    }
}
} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    clickDragEditAndReuse();
    cancellationAndEmptyCommit();
    connectorContourMatchesQtStroke();
    croppedExportPreservesConnectorToOffscreenLabel();
    return 0;
}
