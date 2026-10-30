#include "window_surface_mac_p.h"

#include <QGuiApplication>
#include <QSet>
#include <QWidget>
#include <QWindow>

#import <AppKit/AppKit.h>
#import <objc/runtime.h>

namespace adqt::widgets::detail {
namespace {
char customHeaderButtonKey;

void hideCustomHeaderButton(NSButton* button) {
    if (!button)
        return;
    // Qt's Cocoa backend recomputes button visibility on blocking and size-hint changes,
    // independently of our surface setup. Enforce the custom header's ownership
    // at the native setter so those updates cannot expose duplicate controls.
    // The marker follows the button's lifetime, including native recreation;
    // unmarked buttons keep AppKit's behavior and all buttons keep Qt's enabled state.
    objc_setAssociatedObject(button, &customHeaderButtonKey, @YES,
                             OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    static QSet<Class> installedClasses;
    Class buttonClass = [button class];
    if (!installedClasses.contains(buttonClass)) {
        const SEL selector = @selector(setHidden:);
        const Method method = class_getInstanceMethod(buttonClass, selector);
        const IMP original = method_getImplementation(method);
        const IMP replacement = imp_implementationWithBlock(^(NSButton* receiver, BOOL hidden) {
          if (objc_getAssociatedObject(receiver, &customHeaderButtonKey))
              hidden = YES;
          reinterpret_cast<void (*)(id, SEL, BOOL)>(original)(receiver, selector, hidden);
        });
        // Preserve the concrete AppKit class and its KVO identity.
        class_replaceMethod(buttonClass, selector, replacement, method_getTypeEncoding(method));
        installedClasses.insert(buttonClass);
    }
    button.hidden = YES;
}
} // namespace

void applyMacWindowSurfaceChrome(QWidget* surface, bool minimizeEnabled, bool maximizeEnabled) {
    auto* view = reinterpret_cast<NSView*>(surface->winId());
    // Keep the native title for window menus and accessibility while the window
    // paints its own header across the expanded content area.
    view.window.titleVisibility = NSWindowTitleHidden;
    // The Ant header paints its own controls. Retain AppKit's window-management
    // capabilities for the Dock and Window menu without duplicate traffic lights.
    NSWindowStyleMask style = view.window.styleMask;
    if (minimizeEnabled || maximizeEnabled) {
        style |= NSWindowStyleMaskClosable;
    }
    if (minimizeEnabled) {
        style |= NSWindowStyleMaskMiniaturizable;
    } else {
        style &= ~NSWindowStyleMaskMiniaturizable;
    }
    if (maximizeEnabled) {
        style |= NSWindowStyleMaskResizable;
    }
    if (view.window.styleMask != style) {
        view.window.styleMask = style;
    }
    hideCustomHeaderButton([view.window standardWindowButton:NSWindowCloseButton]);
    hideCustomHeaderButton([view.window standardWindowButton:NSWindowMiniaturizeButton]);
    hideCustomHeaderButton([view.window standardWindowButton:NSWindowZoomButton]);
    [view.window standardWindowButton:NSWindowMiniaturizeButton].enabled = minimizeEnabled;
    [view.window standardWindowButton:NSWindowZoomButton].enabled = maximizeEnabled;
}

void releaseMacWindowSurfaceCursor(QWindow* surface) {
    if (!surface || !surface->handle() ||
        QGuiApplication::platformName() != QStringLiteral("cocoa")) {
        return;
    }
    auto* view = reinterpret_cast<NSView*>(surface->winId());
    // QNSView retains its cursor but Qt 6.11.1 and 6.12.0 do not release that property
    // in dealloc. SurfaceAboutToBeDestroyed is the last point where it is live.
    // Use the property setter so its retain is balanced without touching Qt's
    // shared cursor cache or the application's override cursor stack.
    if ([view respondsToSelector:@selector(setCursor:)]) {
        [view setValue:nil forKey:@"cursor"];
    }
}

void updateMacWindowSurfaceShadow(QWidget* surface) {
    if (QGuiApplication::platformName() != QStringLiteral("cocoa")) {
        return;
    }
    auto* view = reinterpret_cast<NSView*>(surface->winId());
    NSWindow* window = view.window;
    // Cocoa's shadow follows the painted alpha and never participates in hit testing.
    // Qt window flags own shadow visibility, including after native recreation.
    [window invalidateShadow];
}

} // namespace adqt::widgets::detail
