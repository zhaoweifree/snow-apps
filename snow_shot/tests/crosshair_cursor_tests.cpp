#include "snow_shot/presentation/crosshaircursor.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"

#include <QApplication>
#include <QImage>
#include <cstdlib>
#include <iostream>

#ifdef Q_OS_WIN
#include "snow_capture.h"
#include <qt_windows.h>
#endif

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void artworkAndScaling() {
#ifdef Q_OS_WIN
    SnowCaptureCursorImageView view{};
    require(snow_capture_crosshair_cursor_image(&view) != 0, "shared artwork must be available");
    require(view.width == 32 && view.height == 32 && view.hotspot_x == 16 && view.hotspot_y == 16 &&
                view.rgba_len == 32 * 32 * 4,
            "shared base image must retain its size and centered hotspot");
    for (qreal dpr : {1.0, 1.08, 1.25, 1.5, 1.52, 2.0}) {
        const auto cursor = snow_shot::presentation::crosshairCursor(dpr);
        require(cursor.shape() == Qt::BitmapCursor && cursor.hotSpot() == QPoint(16, 16),
                "custom crosshair must be a bitmap with the center hotspot");
        const auto image = cursor.pixmap().toImage().convertToFormat(QImage::Format_RGBA8888);
        const int physicalHotspot = qRound(16 * dpr);
        require(image.size() == QSize(physicalHotspot * 2, physicalHotspot * 2) &&
                    image.devicePixelRatio() == dpr,
                "crosshair raster must follow the display DPI");
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const auto sourceX = static_cast<quint64>(x) * view.width / image.width();
                const auto sourceY = static_cast<quint64>(y) * view.height / image.height();
                const auto* pixel = view.rgba + (sourceY * view.width + sourceX) * 4;
                require(image.pixelColor(x, y) == QColor(pixel[0], pixel[1], pixel[2], pixel[3]),
                        "Qt pixels must match the generator's nearest-neighbor mapping");
            }
        }
        require(cursor == snow_shot::presentation::crosshairCursor(dpr),
                "repeated crosshair requests must reuse the cursor");
        const int outline = physicalHotspot - qMax(1, image.width() / 32);
        require(image.pixelColor(physicalHotspot, physicalHotspot) == QColor(Qt::white) &&
                    image.pixelColor(outline, physicalHotspot) == QColor(Qt::white) &&
                    image.pixelColor(outline, outline) == QColor(Qt::black) &&
                    image.pixelColor(0, 0).alpha() == 0,
                "crosshair must have white fill, black outline and transparent corners");
    }
#else
    require(snow_shot::presentation::crosshairCursor().shape() == Qt::CrossCursor,
            "other platforms retain the native crosshair");
#endif
}

void canvasLayers() {
    SnowCanvasWidget canvas;
    snow_shot::presentation::configureCanvasCrosshairCursor(canvas);
    require(canvas.setCanvasTool(SnowCanvasTool::Shape), "activate a crosshair tool");
    const auto expected = snow_shot::presentation::crosshairCursor(canvas.devicePixelRatioF());
    require(canvas.cursor() == expected, "Snow Shot canvas tools must use the shared crosshair");
    canvas.setCursorForLayer(SnowCanvasCursorLayer::Host, QCursor(Qt::WaitCursor));
    require(canvas.cursor().shape() == Qt::WaitCursor,
            "non-crosshair host cursors retain priority");
    canvas.setCursorForLayer(SnowCanvasCursorLayer::CanvasTool, QCursor(Qt::IBeamCursor));
    canvas.clearCursorForLayer(SnowCanvasCursorLayer::Host);
    require(canvas.cursor().shape() == Qt::IBeamCursor, "clearing host reveals the latest tool");
    canvas.setCursorForLayer(SnowCanvasCursorLayer::Host, QCursor(Qt::CrossCursor));
    require(canvas.cursor() == expected, "host crosshairs use the same artwork as tool cursors");
}

#ifdef Q_OS_WIN
void initialNativeCursor() {
    for (qreal dpr : {1.0, 1.08, 1.1, 1.25, 1.5, 1.52, 2.0}) {
        const HCURSOR previous = GetCursor();
        const bool applied = snow_shot::presentation::applyNativeCrosshairCursor(dpr);
        const HCURSOR first = GetCursor();
        ICONINFO info{};
        const bool inspected = applied && GetIconInfo(first, &info);
        BITMAP bitmap{};
        const bool sized = inspected && GetObjectW(info.hbmColor, sizeof(bitmap), &bitmap);
        const bool reused =
            snow_shot::presentation::applyNativeCrosshairCursor(dpr) && GetCursor() == first;
        SetCursor(previous);
        if (info.hbmColor)
            DeleteObject(info.hbmColor);
        if (info.hbmMask)
            DeleteObject(info.hbmMask);
        const int size = 2 * qRound(16 * dpr);
        require(applied && inspected && sized && bitmap.bmWidth == size &&
                    bitmap.bmHeight == size &&
                    info.xHotspot == static_cast<DWORD>((size + 1) / 2) &&
                    info.yHotspot == static_cast<DWORD>((size + 1) / 2) && reused,
                "initial native crosshair must be realized and cached before it is displayed");
    }
}
#endif
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
#ifdef Q_OS_WIN
    if (app.arguments().contains(QStringLiteral("--native-only"))) {
        initialNativeCursor();
        return 0;
    }
#endif
    artworkAndScaling();
    canvasLayers();
    return 0;
}
