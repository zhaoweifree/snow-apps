#include "../src/platform/windows/captureexclusioncomposition.h"

#include <QApplication>
#include <QBackingStore>
#include <QElapsedTimer>
#include <QPainter>
#include <QPointer>
#include <QRhiWidget>
#include <QThread>
#include <QWindow>
#include <qpa/qplatformbackingstore.h>
#include <rhi/qrhi.h>

#include <cstdlib>
#include <iostream>
#include <thread>

namespace composition = snow_shot::platform::windows::detail;

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void alphaCoverage() {
    require(composition::captureExclusionAlphaRegion({}).isEmpty(), "null image has no coverage");
    for (const auto format : {QImage::Format_ARGB32_Premultiplied, QImage::Format_RGBA8888,
                              QImage::Format_ARGB32, QImage::Format_Alpha8}) {
        QImage image(17, 9, format);
        image.fill(Qt::transparent);
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                // Include disjoint spans, holes, both row edges, faint shadows,
                // and padded scan lines. RGB must never determine hit coverage.
                const int alpha = (x + y) % 3 == 0 ? 0 : ((x + y) % 2 == 0 ? 1 : 255);
                image.setPixelColor(x, y, QColor(120, 80, 40, alpha));
            }
        }
        image.setDevicePixelRatio(1.5);
        const QRegion region = composition::captureExclusionAlphaRegion(image);
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x)
                require(region.contains(QPoint(x, y)) == (image.pixelColor(x, y).alpha() != 0),
                        "region must match nonzero alpha in physical pixels");
        }
        image.fill(Qt::transparent);
        require(composition::captureExclusionAlphaRegion(image).isEmpty(),
                "fully transparent frame must remove all native coverage");
    }
    QImage opaque(17, 9, QImage::Format_RGB32);
    opaque.fill(Qt::black);
    require(composition::captureExclusionAlphaRegion(opaque) == QRegion(opaque.rect()),
            "non-alpha images cover their entire rect");
}

void regionCacheLifecycle() {
    composition::CaptureExclusionRegionCache cache;
    require(cache.isEmpty(), "unused surfaces must not retain coverage");
    int nativeIdentity = 0;
    const auto hwnd = reinterpret_cast<HWND>(&nativeIdentity);
    QRegion coverage;
    for (int y = 0; y < 128; y += 2)
        coverage += QRect(y % 7, y, 64, 1);
    for (int session = 0; session < 20; ++session) {
        require(!cache.matches(hwnd, coverage), "reused HWND must rebuild native coverage");
        cache.remember(hwnd, coverage);
        require(cache.matches(hwnd, coverage), "unchanged coverage should avoid native updates");
        require(!cache.matches(hwnd, coverage.translated(1, 0)),
                "changed coverage must update the native region");
        cache.invalidate();
        require(cache.isEmpty(), "surface invalidation must release cached region storage");
        cache.invalidate();
        require(cache.isEmpty(), "repeated native invalidation must remain empty");
    }
}

class Surface final : public QWidget {
  public:
    Surface() : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint) {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setGeometry(200, 200, 160, 100);
        composition::enableCaptureExclusionCompositionFallback(this);
    }

    void releaseSurface() {
        hide();
        destroy(true, true);
    }

    QRect painted = QRect(30, 30, 80, 40);
    int alpha = 255;

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(rect(), Qt::transparent);
        painter.fillRect(painted, QColor(160, 80, 40, alpha));
    }
};

bool hasCoverage(Surface& surface, QPoint logicalPoint) {
    const auto hwnd = reinterpret_cast<HWND>(surface.winId());
    const qreal dpr = surface.devicePixelRatioF();
    POINT point{qRound(logicalPoint.x() * dpr), qRound(logicalPoint.y() * dpr)};
    require(ClientToScreen(hwnd, &point), "resolve native point");
    // The caller is deliberately not the GUI thread. A same-thread hit test
    // would allow HTTRANSPARENT to hide broken cross-application hit coverage.
    HWND hit = nullptr;
    std::thread query([&] { hit = WindowFromPoint(point); });
    query.join();
    return hit == hwnd || IsChild(hwnd, hit);
}

template <typename Predicate> void eventually(Predicate predicate, const char* message) {
    QElapsedTimer deadline;
    deadline.start();
    while (deadline.elapsed() < 3000) {
        QApplication::processEvents();
        if (predicate())
            return;
        QThread::msleep(1);
    }
    require(false, message);
}

void nativeCoverage(bool initiallyVisible) {
    Surface surface;
    if (initiallyVisible) {
        surface.show();
        eventually([&] { return hasCoverage(surface, QPoint(50, 50)); }, "raster body is hittable");
        require(!hasCoverage(surface, QPoint(5, 5)), "raster corner is transparent");
    }
    const QRect geometry = surface.geometry();
    require(composition::applyCaptureExclusionCompositionFallback(&surface), "apply composition");
    require(surface.isVisible() == initiallyVisible && surface.geometry() == geometry,
            "composition must preserve visibility and geometry");
    surface.show();
    const auto verify = [&] {
        eventually(
            [&] {
                return hasCoverage(surface, surface.painted.center()) &&
                       !hasCoverage(surface, QPoint(5, 5));
            },
            "composition must retain body input and pass through transparent corners");
        require(surface.mask().isEmpty(), "native alpha coverage must not clip Qt's future paints");
        require(SetWindowDisplayAffinity(reinterpret_cast<HWND>(surface.winId()),
                                         WDA_EXCLUDEFROMCAPTURE),
                "alpha coverage must remain compatible with capture exclusion");
    };
    verify();
    surface.painted = QRect(100, 10, 40, 30);
    surface.update();
    eventually(
        [&] {
            return hasCoverage(surface, QPoint(120, 20)) && !hasCoverage(surface, QPoint(50, 50));
        },
        "repaint must grow and shrink native coverage");
    surface.alpha = 1;
    surface.repaint();
    verify();
    surface.alpha = 0;
    surface.update();
    eventually([&] { return !hasCoverage(surface, QPoint(120, 20)); },
               "zero alpha removes coverage");
    surface.alpha = 255;
    surface.update();
    verify();
    // A child-only repaint also changes top-level input coverage.
    QWidget child(&surface);
    child.setAutoFillBackground(true);
    QPalette palette = child.palette();
    palette.setColor(QPalette::Window, Qt::blue);
    child.setPalette(palette);
    child.setGeometry(10, 65, 20, 20);
    child.show();
    eventually([&] { return hasCoverage(surface, QPoint(15, 70)); },
               "child-only updates must expand native coverage");
    child.hide();
    eventually([&] { return !hasCoverage(surface, QPoint(15, 70)); },
               "child hide must remove native coverage");
    surface.resize(240, 140);
    surface.painted = QRect(180, 80, 40, 30);
    surface.update();
    verify();
    surface.hide();
    surface.show();
    verify();
    for (int session = 0; session < 20; ++session) {
        auto* store = surface.backingStore();
        require(store && store->handle(), "composition must have a backing store");
        auto* rhi = store->handle()->rhi(surface.windowHandle());
        require(rhi != nullptr, "composition must exercise graphics resource allocation");
        bool graphicsReleased = false;
        rhi->addCleanupCallback([&](QRhi*) { graphicsReleased = true; });
        const QPointer<QWindow> nativeWindow(surface.windowHandle());
        surface.releaseSurface();
        require(graphicsReleased, "native release must destroy the graphics device and resources");
        require(!surface.backingStore() && !surface.internalWinId() && nativeWindow.isNull(),
                "native release must retire the backing store and window");
        require(composition::usesCaptureExclusionComposition(&surface),
                "helper survives native release");
        surface.painted.translate(session % 2 == 0 ? -20 : 20, 0);
        surface.show();
        verify();
        DWORD affinity = WDA_NONE;
        require(GetWindowDisplayAffinity(reinterpret_cast<HWND>(surface.winId()), &affinity) &&
                    affinity == WDA_EXCLUDEFROMCAPTURE,
                "recreated session must restore capture exclusion");
        require(surface.findChildren<QRhiWidget*>().size() == 1,
                "exactly one helper survives repeated recreation");
    }
}

void nativeOwnedSurfaceIsolation(bool initiallyVisible) {
    QWidget owner;
    owner.show();
    Surface surface;
    surface.setParent(&owner, surface.windowFlags());
    if (initiallyVisible)
        surface.show();
    const QPointer<QWindow> ownerWindow(owner.windowHandle());
    const WId ownerId = owner.internalWinId();
    const QRect geometry = surface.geometry();
    require(composition::applyCaptureExclusionCompositionFallback(&surface), "apply composition");
    require(ownerWindow && owner.windowHandle() == ownerWindow && owner.internalWinId() == ownerId,
            "composition must not replace the native owner window");
    require(surface.isVisible() == initiallyVisible && surface.geometry() == geometry,
            "owned composition must preserve visibility and geometry");
    surface.show();
    eventually([&] { return hasCoverage(surface, surface.painted.center()); },
               "owned surface must present after composition");
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    alphaCoverage();
    regionCacheLifecycle();
    if (app.arguments().contains(QStringLiteral("--native"))) {
        nativeCoverage(false);
        nativeCoverage(true);
        nativeOwnedSurfaceIsolation(false);
        nativeOwnedSurfaceIsolation(true);
    }
    std::cout << "capture composition tests passed\n";
}
