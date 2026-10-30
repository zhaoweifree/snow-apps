#include "captureexclusioncomposition.h"

#include <QBackingStore>
#include <QEvent>
#include <QGuiApplication>
#include <QPointer>
#include <QRhiWidget>
#include <QWindow>
#include <qpa/qplatformbackingstore.h>

namespace snow_shot::platform::windows::detail {
namespace {
// The opted-in windows paint into Qt's raster backing image. QRhi only presents
// that image through D3D11. Windows no longer derives input coverage from alpha
// on this path, so keep its native region in sync with the completed image.
class CaptureExclusionComposition final : public QRhiWidget {
  public:
    explicit CaptureExclusionComposition(QWidget* window) {
        setObjectName(QStringLiteral("snowCaptureExclusionComposition"));
        setApi(Api::Direct3D11);
        setFocusPolicy(Qt::NoFocus);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        hide();
        // Configure before parenting: Qt may replace an existing raster HWND.
        setParent(window);
        window->installEventFilter(this);
        observeNativeWindow();
    }

  private:
    void observeNativeWindow() {
        m_regionCache.invalidate();
        if (m_nativeWindow == parentWidget()->windowHandle())
            return;
        if (m_nativeWindow)
            m_nativeWindow->removeEventFilter(this);
        m_nativeWindow = parentWidget()->windowHandle();
        if (m_nativeWindow)
            m_nativeWindow->installEventFilter(this);
    }

    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == parentWidget() && event->type() == QEvent::WinIdChange)
            observeNativeWindow();
        if (event->type() == QEvent::DevicePixelRatioChange)
            m_regionCache.invalidate();
        // Both ordinary updates and native expose events can flush the backing
        // store. Finish Qt's dispatch before reading it; never render a second
        // copy or read a paint device outside beginPaint/endPaint.
        const bool presents =
            (watched == parentWidget() && event->type() == QEvent::UpdateRequest) ||
            (watched == m_nativeWindow && event->type() == QEvent::Expose);
        if (!presents || m_dispatching)
            return false;
        const QPointer<QRhiWidget> lifetime(this);
        m_dispatching = true;
        QCoreApplication::sendEvent(watched, event);
        if (lifetime) {
            updateNativeRegion();
            if (lifetime)
                m_dispatching = false;
        }
        return true;
    }

    void updateNativeRegion() {
        QWidget* window = parentWidget();
        auto* store = window->backingStore();
        if (!window->internalWinId() || !store || store->size().isEmpty() || !store->handle())
            return;
        // toImage is a shared CPU image on the Windows backing store, not a GPU
        // readback. Keep this Qt platform dependency confined to this file.
        const QImage image = store->handle()->toImage();
        if (image.isNull())
            return;
        const auto hwnd = reinterpret_cast<HWND>(window->internalWinId());
        POINT clientOrigin{};
        RECT frame{};
        if (!ClientToScreen(hwnd, &clientOrigin) || !GetWindowRect(hwnd, &frame))
            return;
        const QRegion region = captureExclusionAlphaRegion(image).translated(
            clientOrigin.x - frame.left, clientOrigin.y - frame.top);
        if (m_regionCache.matches(hwnd, region))
            return;
        // Qt exports an empty region as nullptr; Windows interprets nullptr as
        // removing the mask entirely. An explicit empty HRGN means no coverage.
        const HRGN native = region.isEmpty() ? CreateRectRgn(0, 0, 0, 0) : region.toHRGN();
        if (!native)
            return;
        // Do not use QWidget::setMask: that would clip future backing-store
        // paints to yesterday's shape and prevent transparent pixels filling in.
        if (!SetWindowRgn(hwnd, native, FALSE)) {
            DeleteObject(native);
            return;
        }
        // Windows owns native after success. The region is already in physical
        // pixels, preserving fractional-DPI edges and even faint shadow alpha.
        m_regionCache.remember(hwnd, region);
    }

    QPointer<QWindow> m_nativeWindow;
    CaptureExclusionRegionCache m_regionCache;
    bool m_dispatching = false;
};
} // namespace

QRegion captureExclusionAlphaRegion(const QImage& image) {
    QRegion region;
    if (image.isNull())
        return region;
    if (!image.hasAlphaChannel())
        return QRegion(image.rect());
    const QImage pixels = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < pixels.height(); ++y) {
        const auto* row = reinterpret_cast<const QRgb*>(pixels.constScanLine(y));
        for (int x = 0; x < pixels.width();) {
            while (x < pixels.width() && qAlpha(row[x]) == 0)
                ++x;
            const int start = x;
            while (x < pixels.width() && qAlpha(row[x]) != 0)
                ++x;
            if (x > start)
                region += QRect(start, y, x - start, 1);
        }
    }
    return region;
}

bool usesCaptureExclusionComposition(const QWidget* window) {
    return window &&
           window->findChild<QRhiWidget*>(QStringLiteral("snowCaptureExclusionComposition"),
                                          Qt::FindDirectChildrenOnly);
}

bool applyCaptureExclusionCompositionFallback(QWidget* window) {
    if (!window || !window->isWindow() || !window->testAttribute(Qt::WA_TranslucentBackground) ||
        !window->property(captureExclusionCompositionFallbackProperty).toBool() ||
        QGuiApplication::platformName() != QStringLiteral("windows") ||
        usesCaptureExclusionComposition(window)) {
        return false;
    }
    // Qt attaches a reparented QRhiWidget to the closest ancestor with a QWindow.
    // Hidden owned tools (such as the scrolling thumbnail) may not have one yet.
    // Materialize the target first, or Qt can recreate its owner's native window
    // and retire sibling toolbars and the owner's existing capture affinity.
    window->winId();
    new CaptureExclusionComposition(window);
    return true;
}
} // namespace snow_shot::platform::windows::detail
