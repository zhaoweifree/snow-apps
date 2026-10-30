#include "snow_shot/presentation/crosshaircursor.h"

#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include <QGuiApplication>
#include <QImage>
#include <QMap>
#include <QPixmap>
#include <QPointer>

#include <algorithm>
#include <cmath>
#include <vector>

#ifdef Q_OS_WIN
#include "snow_capture.h"
#include <qt_windows.h>
#endif

namespace snow_shot::presentation {
#ifdef Q_OS_WIN
namespace {
class CrosshairCursorCache final : public QObject {
  public:
    explicit CrosshairCursorCache(QObject* parent) : QObject(parent) {}
    ~CrosshairCursorCache() override {
        for (const auto handle : nativeCursors)
            DestroyCursor(handle);
    }
    QMap<qreal, QCursor> cursors;
    QMap<qreal, HCURSOR> nativeCursors;
};

CrosshairCursorCache& cursorCache() {
    static QPointer<CrosshairCursorCache> cache;
    if (!cache)
        cache = new CrosshairCursorCache(qGuiApp);
    return *cache;
}
} // namespace
#endif

QCursor crosshairCursor(qreal devicePixelRatio) {
#ifdef Q_OS_WIN
    const qreal dpr =
        std::isfinite(devicePixelRatio) && devicePixelRatio > 0.0 && devicePixelRatio <= 64.0
            ? devicePixelRatio
            : 1.0;
    auto& cursors = cursorCache().cursors;
    const auto found = cursors.constFind(dpr);
    if (found != cursors.cend())
        return *found;

    SnowCaptureCursorImageView view{};
    if (!snow_capture_crosshair_cursor_image(&view)) {
        qWarning("Unable to load the crosshair cursor image");
        return QCursor(Qt::ArrowCursor);
    }
    // Qt realizes integer logical hotspots with qRound(hotspot * display DPR).
    // Keep an even raster centered on that physical hotspot; independently rounding
    // 32 * DPR could put the white center one pixel away at custom display scales.
    const int width = 2 * qMax(1, qRound(view.hotspot_x * dpr));
    const int height = 2 * qMax(1, qRound(view.hotspot_y * dpr));
    QImage image(width, height, QImage::Format_RGBA8888);
    // Use the generator's integer nearest-neighbor mapping for exact UI/capture parity.
    for (int y = 0; y < height; ++y) {
        auto* row = image.scanLine(y);
        const auto sourceY = static_cast<quint64>(y) * view.height / height;
        for (int x = 0; x < width; ++x) {
            const auto sourceX = static_cast<quint64>(x) * view.width / width;
            const auto* source = view.rgba + (sourceY * view.width + sourceX) * 4;
            std::copy_n(source, 4, row + x * 4);
        }
    }
    image.setDevicePixelRatio(dpr);
    const QCursor cursor(QPixmap::fromImage(image), static_cast<int>(view.hotspot_x),
                         static_cast<int>(view.hotspot_y));
    cursors.insert(dpr, cursor);
    return cursor;
#else
    Q_UNUSED(devicePixelRatio);
    return QCursor(Qt::CrossCursor);
#endif
}

void configureCanvasCrosshairCursor(SnowCanvasWidget& canvas) {
#ifdef Q_OS_WIN
    canvas.setCrosshairCursorFactory(crosshairCursor);
#else
    Q_UNUSED(canvas);
#endif
}

bool applyNativeCrosshairCursor(qreal devicePixelRatio) {
#ifdef Q_OS_WIN
    if (QGuiApplication::platformName() != QStringLiteral("windows"))
        return false;
    const QCursor cursor = crosshairCursor(devicePixelRatio);
    const QImage image =
        cursor.pixmap().toImage().convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (image.isNull())
        return false;
    auto& handles = cursorCache().nativeCursors;
    const qreal dpr = image.devicePixelRatio();
    HCURSOR handle = handles.value(dpr);
    if (!handle) {
        // Qt's native cursor accessor only finds handles already realized in its
        // primary-screen cache. Initial selection must work before any mouse event
        // realizes the cursor, including when selecting on a secondary display.
        BITMAPINFO bitmap{};
        bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmap.bmiHeader.biWidth = image.width();
        bitmap.bmiHeader.biHeight = -image.height();
        bitmap.bmiHeader.biPlanes = 1;
        bitmap.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        const HBITMAP color = CreateDIBSection(nullptr, &bitmap, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!color)
            return false;
        for (int y = 0; y < image.height(); ++y) {
            const auto* source = image.constScanLine(y);
            auto* destination = static_cast<uchar*>(bits) + y * image.width() * 4;
            // Windows' little-endian ARGB32 memory layout is premultiplied BGRA.
            std::copy_n(source, image.width() * 4, destination);
        }
        const std::vector<uchar> maskPixels(
            static_cast<size_t>((image.width() + 15) / 16 * 2 * image.height()), 0);
        const HBITMAP mask = CreateBitmap(image.width(), image.height(), 1, 1, maskPixels.data());
        if (!mask) {
            DeleteObject(color);
            return false;
        }
        ICONINFO info{};
        info.xHotspot = static_cast<DWORD>(
            qMin(image.width() - 1, (cursor.hotSpot().x() * image.width() + 31) / 32));
        info.yHotspot = static_cast<DWORD>(
            qMin(image.height() - 1, (cursor.hotSpot().y() * image.height() + 31) / 32));
        info.hbmColor = color;
        info.hbmMask = mask;
        handle = static_cast<HCURSOR>(CreateIconIndirect(&info));
        DeleteObject(color);
        DeleteObject(mask);
        if (!handle)
            return false;
        handles.insert(dpr, handle);
    }
    SetCursor(handle);
    return GetCursor() == handle;
#else
    Q_UNUSED(devicePixelRatio);
    return false;
#endif
}
} // namespace snow_shot::presentation
