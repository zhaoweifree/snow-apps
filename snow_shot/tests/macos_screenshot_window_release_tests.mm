#include "snow_shot/platform/screenshotnative.h"

#import <AppKit/AppKit.h>
#import <IOSurface/IOSurface.h>
#import <QuartzCore/QuartzCore.h>

#include <QApplication>
#include <QColor>
#include <QEvent>
#include <QEventLoop>
#include <QPainter>
#include <QPointer>
#include <QProcess>
#include <QTimer>
#include <QWidget>
#include <QWindow>
#include <cstdlib>
#include <cstdint>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

class ScreenshotSurface final : public QWidget {
  public:
    ScreenshotSurface()
        : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                               Qt::NoDropShadowWindowHint) {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_NoSystemBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setGeometry(120, 120, 192, 128);
    }

    void releaseNativeSurface() {
        hide();
        setUpdatesEnabled(false);
        destroy(true, true);
    }

    QColor color = QColor(37, 101, 173);
    int paints = 0;

  protected:
    void paintEvent(QPaintEvent*) override {
        ++paints;
        QPainter painter(this);
        painter.fillRect(rect(), color);
    }
};

template <typename Predicate> void waitFor(Predicate ready, const char* message) {
    if (ready())
        return;
    // An idle dispatcher is not a presentation fence. Let Cocoa deliver expose,
    // paint and compositor events until the requested observable state exists.
    QEventLoop loop;
    QTimer poll;
    QTimer deadline;
    deadline.setSingleShot(true);
    bool completed = false;
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (ready()) {
            completed = true;
            loop.quit();
        }
    });
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    poll.start(1);
    deadline.start(5000);
    loop.exec();
    require(completed, message);
}

void retirementDoesNotMaterializeSurfaces() {
    using snow_shot::platform::retireScreenshotWindowPresentation;
    retireScreenshotWindowPresentation(nullptr);
    ScreenshotSurface surface;
    for (int attempt = 0; attempt != 3; ++attempt) {
        retireScreenshotWindowPresentation(&surface);
        require(surface.internalWinId() == 0 && surface.windowHandle() == nullptr,
                "retiring an unmaterialized screenshot must not create a native surface");
        const int previousPaints = surface.paints;
        surface.setUpdatesEnabled(true);
        surface.show();
        surface.repaint();
        waitFor([&] { return surface.paints > previousPaints; },
                "a reused screenshot must still paint");
        const auto nativeId = surface.internalWinId();
        const QPointer<QWindow> window = surface.windowHandle();
        const QRect geometry = surface.geometry();
        const int paints = surface.paints;
        surface.setUpdatesEnabled(false);
        retireScreenshotWindowPresentation(&surface);
        require(surface.isVisible() && !surface.updatesEnabled() &&
                    surface.internalWinId() == nativeId && surface.windowHandle() == window &&
                    surface.geometry() == geometry,
                "presentation retirement must preserve the QWidget until its owner hides it");
        QCoreApplication::postEvent(&surface, new QEvent(QEvent::UpdateRequest));
        QCoreApplication::processEvents();
        require(surface.paints == paints,
                "queued updates must not repaint a screenshot whose presentation is retired");
        surface.releaseNativeSurface();
        require(surface.internalWinId() == 0 && surface.windowHandle() == nullptr &&
                    window.isNull(),
                "screenshot completion must release its Qt native surface");
        retireScreenshotWindowPresentation(&surface);
        require(surface.internalWinId() == 0 && surface.windowHandle() == nullptr,
                "repeated retirement must not revive a destroyed native surface");
    }
}

CALayer* presentedSurfaceLayer(CALayer* layer) {
    if (!layer)
        return nil;
    id contents = layer.contents;
    if (contents && CFGetTypeID(reinterpret_cast<CFTypeRef>(contents)) == IOSurfaceGetTypeID())
        return layer;
    for (CALayer* child in layer.sublayers) {
        if (auto* surface = presentedSurfaceLayer(child))
            return surface;
    }
    return nil;
}

bool presentationIsEmpty(CALayer* layer) {
    if (!layer)
        return true;
    if (layer.contents)
        return false;
    for (CALayer* child in layer.sublayers) {
        if (!presentationIsEmpty(child))
            return false;
    }
    return true;
}

void clearModelContents(CALayer* layer) {
    if (!layer)
        return;
    layer.contents = nil;
    for (CALayer* child in layer.sublayers)
        clearModelContents(child);
}

QByteArray ownedUnmappedMemory() {
    // A retained NSWindow can own WindowServer pixels after the Qt view and its
    // mapped IOSurfaces are gone. Checking CALayer.contents cannot detect that.
    // Inspect this isolated native test process, not aggregate RSS or allocator
    // caches. vmmap ships with macOS; no screen capture permission is needed.
    QProcess vmmap;
    vmmap.start(QStringLiteral("/usr/bin/vmmap"),
                {QStringLiteral("-summary"), QString::number(QCoreApplication::applicationPid())});
    require(vmmap.waitForFinished(10000) && vmmap.exitStatus() == QProcess::NormalExit &&
                vmmap.exitCode() == 0,
            "vmmap must inspect the native fixture's retained memory");
    const QByteArray report = vmmap.readAllStandardOutput();
    require(report.contains("REGION TYPE") && report.contains("TOTAL"),
            "vmmap must return a complete memory region summary");
    for (const QByteArray& line : report.split('\n')) {
        if (line.trimmed().startsWith("owned unmapped "))
            return line.trimmed();
    }
    return {};
}

void requireNoRetainedCompositorFrame() {
    QByteArray retained;
    waitFor(
        [&] {
            retained = ownedUnmappedMemory();
            return retained.isEmpty();
        },
        "a retired native window must not retain an owned-unmapped compositor frame");
}

void requirePaintedPixels(CALayer* layer, const QColor& expected) {
    require(layer != nil, "a shown screenshot must present a real raster surface");
    IOSurfaceRef surface = reinterpret_cast<IOSurfaceRef>(layer.contents);
    require(IOSurfaceGetBytesPerElement(surface) == 4,
            "the screenshot fixture requires Qt's four-byte raster surface");
    require(IOSurfaceLock(surface, kIOSurfaceLockReadOnly, nullptr) == 0,
            "the presented screenshot surface must be readable");
    const auto* bytes = static_cast<const std::uint8_t*>(IOSurfaceGetBaseAddress(surface));
    const auto offset = (IOSurfaceGetHeight(surface) / 2) * IOSurfaceGetBytesPerRow(surface) +
                        (IOSurfaceGetWidth(surface) / 2) * 4;
    const QColor actual(bytes[offset + 2], bytes[offset + 1], bytes[offset], bytes[offset + 3]);
    require(IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, nullptr) == 0,
            "the presented screenshot surface must unlock");
    require(actual == expected, "surface reuse must present the new screenshot's pixels");
}

void retainedNativeWindowsDoNotRetainPresentation() {
    using namespace snow_shot::platform;
    ScreenshotSurface surface;
    requireNoRetainedCompositorFrame();
    for (int generation = 0; generation != 3; ++generation) {
        @autoreleasepool {
            surface.color = generation % 2 == 0 ? QColor(37, 101, 173) : QColor(181, 73, 29);
            surface.setUpdatesEnabled(true);
            configureScreenshotOverlayWindow(&surface);
            surface.show();
            surface.repaint();
            NSView* view = reinterpret_cast<NSView*>(surface.internalWinId());
            NSWindow* window = view.window;
            [NSApp activate];
            [window makeKeyAndOrderFront:nil];
            waitFor(
                [&] {
                    CALayer* layer = presentedSurfaceLayer(view.layer);
                    return NSApp.keyWindow == window && window.isKeyWindow &&
                           surface.windowHandle()->isExposed() && layer != nil &&
                           layer.presentationLayer.contents == layer.contents;
                },
                "the focused screenshot must present its raster surface before retirement");
            requirePaintedPixels(presentedSurfaceLayer(view.layer), surface.color);

            // Model AppKit retaining a retired previous key window. Retaining
            // only NSWindow deliberately leaves the pixel content's ownership
            // in the native layer tree and Qt backing store.
            NSWindow* retainedWindow = [window retain];
            const NSRect nativeFrame = window.frame;
            CALayer* frameLayer = window.contentView.superview.layer;
            const int paints = surface.paints;
            surface.setUpdatesEnabled(false);
            if (generation == 1) {
                // An earlier producer may have emptied the model tree without
                // committing it yet. Retirement must still flush that clear.
                clearModelContents(view.layer);
                clearModelContents(frameLayer);
            }
            retireScreenshotWindowPresentation(&surface);
            require(presentationIsEmpty(view.layer) && presentationIsEmpty(frameLayer),
                    "retirement must detach screenshot pixels before the first native hide");
            require(surface.isVisible() && !surface.updatesEnabled() && window.visible &&
                        window.isKeyWindow && NSApp.keyWindow == window &&
                        NSEqualRects(window.frame, nativeFrame),
                    "detaching presentation must preserve visibility, focus and geometry");
            QCoreApplication::postEvent(&surface, new QEvent(QEvent::UpdateRequest));
            if (generation == 2) {
                // Export may defer native destruction. Dispatch queued updates
                // while the retired surface still exists, then retire again.
                QCoreApplication::sendPostedEvents(&surface, QEvent::UpdateRequest);
                require(surface.paints == paints && presentationIsEmpty(view.layer) &&
                            presentationIsEmpty(frameLayer),
                        "queued Qt updates must not restore pixels on a retired presentation");
                retireScreenshotWindowPresentation(&surface);
            }
            // No event-loop turn between retirement and hide: an implicit
            // transaction commit here would mask a missing production flush.
            surface.releaseNativeSurface();
            require(retainedWindow.contentView == nil && !retainedWindow.visible,
                    "a retained retired native window must release its Qt content view");
            require(surface.internalWinId() == 0 && surface.windowHandle() == nullptr,
                    "native retirement must release the screenshot's Qt window");
            requireNoRetainedCompositorFrame();
            require(surface.paints == paints,
                    "terminal surface retirement must not paint the screenshot again");
            [retainedWindow release];
        }
    }
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    retirementDoesNotMaterializeSurfaces();
    if (QGuiApplication::platformName() == QStringLiteral("cocoa"))
        retainedNativeWindowsDoNotRetainPresentation();
    std::cout << "macOS screenshot window release tests passed\n";
    return EXIT_SUCCESS;
}
