#include "directcapturenative.h"
#include "captureframeimage.h"
#include "captureframegeometry.h"

#ifdef Q_OS_LINUX
#include "snow_linux.h"

#include <QImage>
#include <QPainter>

#include <vector>
#endif

#include <atomic>
#include <limits>
#include <memory>
#include <QCoreApplication>
#include <utility>

namespace snow_shot::presentation {
namespace {
QString nativeError() {
    return QString::fromUtf8(snow_capture_last_error_message());
}

#ifdef Q_OS_LINUX
// A window has no capture of its own on X11, so it is cut out of the root capture.
//
// The backend reads pixels from the root window and nothing else: reading a
// window's own drawable needs a Composite redirect so that the server keeps its
// contents while it is obscured, and without one the read returns whatever was
// left in the framebuffer. Cropping the root capture is what the other X11
// screenshot tools do, and it has the honest answer for an obscured window -- the
// user sees that the window is behind another one, and the capture shows it.
//
// Which part of which display supplies which part of the window is decided in
// snow-linux, where it is unit-tested with a table of rectangles: a window
// straddling two heads is the case that matters, because copying from a single
// display quietly returns half a window.
bool cropWindowFromDisplays(const QRect& frame, const QVector<DirectCaptureDisplay>& displays,
                            QImage* out) {
    if (out == nullptr || frame.isEmpty() || frame.width() <= 0 || frame.height() <= 0 ||
        displays.empty()) {
        return false;
    }
    std::vector<SnowLinuxRect> rects;
    rects.reserve(displays.size());
    for (const auto& display : displays) {
        const QRect& bounds = display.physicalBounds;
        rects.push_back(SnowLinuxRect{bounds.x(), bounds.y(),
                                      static_cast<uint32_t>(qMax(bounds.width(), 0)),
                                      static_cast<uint32_t>(qMax(bounds.height(), 0))});
    }
    const SnowLinuxRect frameRect{frame.x(), frame.y(), static_cast<uint32_t>(frame.width()),
                                  static_cast<uint32_t>(frame.height())};
    // One slice per display at most, so the display count is a safe capacity: the
    // plan never needs more room than the list it was built from.
    std::vector<SnowLinuxCropSlice> slices(rects.size());
    const qint32 count = snow_linux_window_crop_plan(&frameRect, rects.data(), rects.size(),
                                                     slices.data(), slices.size());
    if (count <= 0) {
        return false;
    }

    // Same pixel layout as the displays themselves, so a single-head window is a
    // plain copy rather than a conversion.
    QImage composed(frame.size(), displays.front().image.format());
    composed.fill(Qt::transparent);
    QPainter painter(&composed);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    bool painted = false;
    for (const SnowLinuxCropSlice& slice : slices) {
        // The plan's indexes come from the same list, so the bound can only fail
        // if it was built elsewhere; the check is what makes the narrowing cast
        // below safe.
        if (slice.display_index >= static_cast<size_t>(displays.size())) {
            continue;
        }
        const QImage& source = displays[static_cast<int>(slice.display_index)].image;
        const QRect sourceRect(slice.source.x, slice.source.y, static_cast<int>(slice.source.width),
                               static_cast<int>(slice.source.height));
        // A display that was captured smaller than it reported would otherwise
        // read outside the image; the overlap is what can actually be copied.
        const QRect available = sourceRect.intersected(source.rect());
        if (available.isEmpty()) {
            continue;
        }
        painter.drawImage(QPoint(slice.destination_x + available.x() - sourceRect.x(),
                                 slice.destination_y + available.y() - sourceRect.y()),
                          source, available);
        painted = true;
    }
    painter.end();
    if (!painted) {
        return false;
    }
    *out = composed;
    return true;
}
#endif
} // namespace

DirectCaptureFrame captureDirectTarget(const DirectCaptureRequest& request) {
    DirectCaptureFrame result;
    switch (request.target) {
    case DirectCaptureTarget::FocusedWindow:
        if (request.window == 0) {
            return result;
        }
#ifdef Q_OS_LINUX
        // The id alone says nothing about where the pixels are: X11 has no
        // window-level capture, and the frame the platform layer read is the only
        // description of the rectangle to cut out.
        if (request.windowFrame.isEmpty()) {
            return result;
        }
#endif
        break;
    case DirectCaptureTarget::CurrentMonitor:
        if (request.monitorName.isEmpty()) {
            return result;
        }
        break;
    }
    SnowCaptureDesktopSessionConfig config{};
    config.capture_retry_count = 1;
    config.pixel_format = SNOW_CAPTURE_PIXEL_FORMAT_BGRA8;
    const std::unique_ptr<SnowCaptureDesktopSession,
                          decltype(&snow_capture_desktop_session_destroy)>
        session(request.historyEnabled ? snow_capture_desktop_session_create(&config) : nullptr,
                snow_capture_desktop_session_destroy);
    SnowCaptureScreenshotRequest nativeRequest{};
    nativeRequest.version = SNOW_CAPTURE_SCREENSHOT_REQUEST_VERSION;
    nativeRequest.struct_size = sizeof(nativeRequest);
    if (request.captureCursor)
        nativeRequest.flags |= SNOW_CAPTURE_SCREENSHOT_REQUEST_INCLUDE_CURSOR;
    if (request.restoreOriginalScreenColors) {
        nativeRequest.flags |= SNOW_CAPTURE_SCREENSHOT_REQUEST_RESTORE_ORIGINAL_COLORS;
    }
#ifndef Q_OS_LINUX
    nativeRequest.focused_window = request.target == DirectCaptureTarget::FocusedWindow
                                       ? static_cast<intptr_t>(request.window)
                                       : 0;
#else
    // Never handed over on X11: the value is an X11 window id, but the native
    // layer's only use for this field is `WindowId::from_windows_handle`, i.e. an
    // HWND, and the X11 backend refuses window targets anyway. The window is cut
    // out of the root capture below instead, so the request asks for the whole
    // desktop.
    nativeRequest.focused_window = 0;
#endif
    const QByteArray monitorIdentity = request.monitorName.toUtf8();
    SnowCaptureTargetScreenshotRequest targetRequest{};
    targetRequest.version = SNOW_CAPTURE_TARGET_SCREENSHOT_REQUEST_VERSION;
    targetRequest.struct_size = sizeof(targetRequest);
    targetRequest.screenshot = nativeRequest;
    targetRequest.monitor_utf8 = request.target == DirectCaptureTarget::CurrentMonitor
                                     ? monitorIdentity.constData()
                                     : nullptr;
    targetRequest.pixel_format = config.pixel_format;
    struct Admission {
        const DirectCaptureRequest& request;
        std::atomic_bool rejected{false};
    } admission{request};
    if (request.admitRaster) {
        targetRequest.admission_context = &admission;
        targetRequest.admit_raster = [](void* context, uint32_t width,
                                        uint32_t height) noexcept -> uint8_t {
            auto& state = *static_cast<Admission*>(context);
            if (state.rejected.load(std::memory_order_acquire))
                return 0;
            bool accepted = false;
            try {
                if (width > 0 && height > 0 &&
                    width <= static_cast<uint32_t>(std::numeric_limits<int>::max()) &&
                    height <= static_cast<uint32_t>(std::numeric_limits<int>::max())) {
                    accepted = state.request.admitRaster(
                        QSize(static_cast<int>(width), static_cast<int>(height)));
                }
            } catch (...) {
                // Exceptions must never unwind through the native C ABI.
            }
            if (!accepted)
                state.rejected.store(true, std::memory_order_release);
            return accepted ? 1 : 0;
        };
    }
#ifdef Q_OS_LINUX
    // X11 has no window-level capture: a window target is cut out of the desktop
    // capture below, which needs every display. Only the monitor target can use
    // the one-shot API, and only when no history session will be opened anyway.
    const bool oneShot = !request.historyEnabled &&
                         request.target == DirectCaptureTarget::CurrentMonitor;
#else
    const bool oneShot = !request.historyEnabled;
#endif
    const std::unique_ptr<SnowCaptureScreenshotResult,
                          decltype(&snow_capture_screenshot_result_destroy)>
        snapshot(oneShot ? snow_capture_target_screenshot(&targetRequest)
                         : (session
                                ? snow_capture_desktop_session_capture(session.get(),
                                                                      &nativeRequest)
                                : nullptr),
                 snow_capture_screenshot_result_destroy);
    if (!snapshot || admission.rejected.load(std::memory_order_acquire)) {
        result.error = admission.rejected.load(std::memory_order_acquire)
                           ? QStringLiteral("resource_limit")
                           : nativeError();
        return result;
    }
    const size_t count = snow_capture_screenshot_result_display_count(snapshot.get());
    for (size_t index = 0; index < count; ++index) {
        SnowCaptureFrameInfo info{};
        if (!snow_capture_screenshot_result_display_info(snapshot.get(), index, &info) ||
            !capture::validFrameInfo(info)) {
            result.error = nativeError();
            break;
        }
        DirectCaptureDisplay display;
        display.image = capture::imageFromFrameLease(
            snow_capture_screenshot_result_display_retain(snapshot.get(), index), info.rgba_bytes,
            info.rgba_len, info.width, info.height, info.stride_bytes, info.pixel_format,
            capture::FrameAlphaMode::Opaque);
        display.physicalBounds =
            QRect(info.x, info.y, static_cast<int>(info.width), static_cast<int>(info.height));
#ifdef Q_OS_MACOS
        SnowCaptureFrameGeometry geometry{};
        geometry.version = SNOW_CAPTURE_FRAME_GEOMETRY_VERSION;
        geometry.struct_size = sizeof(geometry);
        if (!snow_capture_screenshot_result_display_geometry(snapshot.get(), index, &geometry)) {
            result.error = nativeError();
            break;
        }
        const auto logicalBounds = capture::logicalFrameRect(geometry);
        if (!logicalBounds) {
            result.error = QCoreApplication::translate("DirectCaptureController",
                                                       "The captured display geometry is invalid");
            break;
        }
        display.logicalBounds = *logicalBounds;
        display.nativeDisplayId = geometry.display_id;
#endif
        display.stableId = QString::fromUtf8(info.stable_id);
        display.name = QString::fromUtf8(info.name);
        bool currentMonitor =
            display.name == request.monitorName || display.stableId == request.monitorName;
#ifdef Q_OS_MACOS
        currentMonitor =
            currentMonitor ||
            request.monitorName == QStringLiteral("display:%1").arg(display.nativeDisplayId);
#endif
        if (request.target == DirectCaptureTarget::CurrentMonitor && currentMonitor) {
            result.image = display.image;
            result.logicalBounds = display.logicalBounds;
            result.physicalBounds = display.physicalBounds;
            result.identity = display.stableId;
            result.backend = info.backend_kind;
        }
        if (display.image.isNull())
            break;
        result.displays.push_back(std::move(display));
    }
    if (((request.historyEnabled || request.target == DirectCaptureTarget::CurrentMonitor) &&
         result.displays.isEmpty()) ||
        static_cast<size_t>(result.displays.size()) != count) {
        result.image = {};
        result.displays.clear();
        return result;
    }
#ifdef Q_OS_LINUX
    // The one failure this copy of the code can produce on its own: the caller
    // named a monitor and no captured display answers to that name. Saying so
    // beats returning a null image with nothing to report.
    if (request.target == DirectCaptureTarget::CurrentMonitor && result.image.isNull()) {
        result.error = QCoreApplication::translate(
            "DirectCaptureController", "No captured display matched the current monitor");
        return result;
    }
#endif
    if (request.target == DirectCaptureTarget::FocusedWindow) {
#ifdef Q_OS_LINUX
        if (!cropWindowFromDisplays(request.windowFrame, result.displays, &result.image)) {
            result.error = QCoreApplication::translate(
                "DirectCaptureController", "The window is not covered by any captured display");
            return result;
        }
        result.physicalBounds = request.windowFrame;
        result.logicalBounds = request.windowLogicalBounds;
        result.identity = QStringLiteral("window:%1").arg(request.window);
        result.backend = SNOW_CAPTURE_BACKEND_X11;
        return result;
#else
        SnowCaptureWindowFrameInfo info{};
        info.version = SNOW_CAPTURE_WINDOW_FRAME_INFO_VERSION;
        info.struct_size = sizeof(info);
        if (!snow_capture_screenshot_result_focused_window_info(snapshot.get(), &info)) {
            result.error = nativeError();
            return result;
        }
        result.image = capture::imageFromFrameLease(
            snow_capture_screenshot_result_focused_window_retain(snapshot.get()), info.rgba_bytes,
            info.rgba_len, info.width, info.height, info.stride_bytes, info.pixel_format,
            info.backend_kind == SNOW_CAPTURE_BACKEND_SCREEN_CAPTURE_KIT
                ? capture::FrameAlphaMode::Premultiplied
                : capture::FrameAlphaMode::Preserve);
        result.physicalBounds =
            QRect(info.x, info.y, static_cast<int>(info.width), static_cast<int>(info.height));
#ifdef Q_OS_MACOS
        SnowCaptureFrameGeometry geometry{};
        geometry.version = SNOW_CAPTURE_FRAME_GEOMETRY_VERSION;
        geometry.struct_size = sizeof(geometry);
        if (!snow_capture_screenshot_result_focused_window_geometry(snapshot.get(), &geometry)) {
            result.error = nativeError();
            return result;
        }
        const auto logicalBounds = capture::logicalFrameRect(geometry);
        if (!logicalBounds) {
            result.error = QCoreApplication::translate("DirectCaptureController",
                                                       "The captured window geometry is invalid");
            return result;
        }
        result.logicalBounds = *logicalBounds;
#endif
        result.identity = QStringLiteral("window:%1").arg(request.window);
        result.backend = info.backend_kind;
#endif
    }
    return result;
}
} // namespace snow_shot::presentation
