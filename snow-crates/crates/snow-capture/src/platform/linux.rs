//! X11 screen capture on the root window.
//!
//! One backend, one method: core `GetImage` against the root window, decoded
//! into packed 8-bit pixels. There is no GPU path, no damage tracking and no
//! event stream behind it, so the capturer is a thin translation between
//! `snow-capture`'s target geometry and `snow-linux`'s X11 session -- all of the
//! protocol work, and everything that can be wrong about pixel layout, lives in
//! `snow-linux::capture` where it is unit-tested off-Linux.
//!
//! Two consequences of "root window only" are worth stating up front, because
//! both differ from the Windows and macOS backends:
//!
//! * The pointer is never part of the pixels. X composites it over the root
//!   window rather than into it, so `create_target_capturer` stays at its
//!   default of "no native capturer" and the session's generic cursor path
//!   draws the pointer from a `snow-cursor` sample instead.
//! * Window targets are refused. Reading another window's pixels needs a
//!   Composite redirect, so advertising window capture here would mean
//!   promising something the backend cannot deliver.

use std::time::Instant;

use snow_core::timestamp::TickFormat;
use snow_linux::capture::OutputOrder;
use snow_linux::display::MonitorDesc;
use snow_linux::geometry::Rect as X11Rect;
use snow_linux::x11::X11Session;
use snow_media::time::ClockDomain;

use crate::backend::{CaptureBackend, CaptureBackendKind, MonitorCapturer};
use crate::error::{CaptureError, CaptureResult};
use crate::frame::{CapturePixelFormat, Frame};
use crate::region::{MonitorGeometry, MonitorLayout};
use crate::{CaptureOptions, CaptureTarget, CaptureTargetInfo, MonitorId};

/// Map a `snow-linux` failure onto this crate's error taxonomy.
///
/// `SessionMissing` becomes [`CaptureError::BackendUnavailable`] rather than a
/// platform fault: an unattended machine is a configuration fact, so callers
/// should degrade instead of retrying or reporting a crash.
pub(crate) fn map_error(error: snow_linux::LinuxError) -> CaptureError {
    use snow_linux::LinuxError;
    match error {
        LinuxError::SessionMissing => {
            CaptureError::BackendUnavailable("no X display is reachable".into())
        }
        LinuxError::UnsupportedPixelLayout { reason } => {
            CaptureError::BackendUnavailable(format!("unsupported X11 pixel layout: {reason}"))
        }
        other => CaptureError::platform(anyhow::anyhow!(other)),
    }
}

/// Build a capturable identity from one enumerated X11 monitor.
///
/// The RandR output name is what makes the identity stable, so it is hashed by
/// `MonitorId::from_name`; the raw handle is unused because no X11 request here
/// takes an output handle.
fn monitor_id(desc: &MonitorDesc) -> MonitorId {
    MonitorId::from_name(0, desc.stable_id(), desc.is_primary).with_desktop_geometry(
        crate::monitor::MonitorDesktopGeometry {
            x: f64::from(desc.rect.x),
            y: f64::from(desc.rect.y),
            width: f64::from(desc.rect.width),
            height: f64::from(desc.rect.height),
            // X11 root coordinates are already device pixels, so the logical and
            // pixel extents are the same rectangle.
            pixel_width: desc.rect.width,
            pixel_height: desc.rect.height,
        },
    )
}

fn open_session() -> CaptureResult<X11Session> {
    X11Session::connect().map_err(map_error)
}

fn monitors(session: &X11Session) -> CaptureResult<Vec<MonitorId>> {
    let descs = snow_linux::display::enumerate_monitors(session).map_err(map_error)?;
    Ok(descs.iter().map(monitor_id).collect())
}

/// Virtual desktop bounds for a known monitor set.
///
/// Uses the geometry the monitors were enumerated with rather than re-querying
/// the server, so a layout always agrees with the identities in it. A monitor
/// built by hand without geometry is rejected: silently laying it out at the
/// origin would skew every region that spans it.
pub(crate) fn snapshot_layout_from_monitors(
    monitors: Vec<MonitorId>,
) -> CaptureResult<MonitorLayout> {
    if monitors.is_empty() {
        return Err(CaptureError::NoPrimaryMonitor);
    }

    let mut geometries = Vec::with_capacity(monitors.len());
    for monitor in monitors {
        let geometry = monitor.desktop_geometry().ok_or_else(|| {
            CaptureError::InvalidConfig(format!(
                "monitor {} was not enumerated with desktop geometry",
                monitor.name()
            ))
        })?;
        if geometry.pixel_width == 0 || geometry.pixel_height == 0 {
            continue;
        }
        geometries.push(MonitorGeometry {
            x: geometry.x.round() as i32,
            y: geometry.y.round() as i32,
            width: geometry.pixel_width,
            height: geometry.pixel_height,
            monitor,
        });
    }

    if geometries.is_empty() {
        return Err(CaptureError::MonitorLost);
    }

    let virtual_left = geometries.iter().map(|g| g.x).min().unwrap_or(0);
    let virtual_top = geometries.iter().map(|g| g.y).min().unwrap_or(0);
    let virtual_right = geometries
        .iter()
        .map(|g| g.x.saturating_add(g.width as i32))
        .max()
        .unwrap_or(0);
    let virtual_bottom = geometries
        .iter()
        .map(|g| g.y.saturating_add(g.height as i32))
        .max()
        .unwrap_or(0);

    Ok(MonitorLayout {
        monitors: geometries,
        virtual_left,
        virtual_top,
        virtual_width: (virtual_right - virtual_left) as u32,
        virtual_height: (virtual_bottom - virtual_top) as u32,
    })
}

fn window_capture_unsupported() -> CaptureError {
    CaptureError::BackendUnavailable("window capture is not supported by the X11 backend".into())
}

/// The desktop-space rectangle a target covers.
///
/// Window targets are rejected here rather than at the first capture, so a
/// caller learns at session setup that the target cannot be served.
fn target_rect(target: &CaptureTarget, session: &X11Session) -> CaptureResult<X11Rect> {
    match target {
        CaptureTarget::PrimaryMonitor => {
            let primary = snow_linux::display::primary_monitor(session).map_err(map_error)?;
            Ok(primary.rect)
        }
        CaptureTarget::Monitor(id) => {
            let geometry = id.desktop_geometry().ok_or_else(|| {
                CaptureError::InvalidConfig(format!("monitor {} has no known geometry", id.name()))
            })?;
            Ok(X11Rect::new(
                geometry.x.round() as i32,
                geometry.y.round() as i32,
                geometry.pixel_width,
                geometry.pixel_height,
            ))
        }
        CaptureTarget::Region(region) => Ok(X11Rect::new(
            region.x,
            region.y,
            region.width,
            region.height,
        )),
        CaptureTarget::Window(_) => Err(window_capture_unsupported()),
    }
}

pub(crate) struct LinuxBackend;

impl CaptureBackend for LinuxBackend {
    fn enumerate_monitors(&self) -> CaptureResult<Vec<MonitorId>> {
        monitors(&open_session()?)
    }

    fn primary_monitor(&self) -> CaptureResult<MonitorId> {
        let session = open_session()?;
        let primary = snow_linux::display::primary_monitor(&session).map_err(map_error)?;
        Ok(monitor_id(&primary))
    }

    fn monitor_layout(&self) -> CaptureResult<MonitorLayout> {
        snapshot_layout_from_monitors(monitors(&open_session()?)?)
    }

    fn inspect_target(&self, target: &CaptureTarget) -> CaptureResult<Option<CaptureTargetInfo>> {
        Ok(Some(self.target_info(target)?))
    }

    fn inspect_window(&self, _window: &crate::WindowId) -> CaptureResult<CaptureTargetInfo> {
        Err(window_capture_unsupported())
    }

    fn create_monitor_capturer(
        &self,
        monitor: &MonitorId,
    ) -> CaptureResult<Box<dyn MonitorCapturer>> {
        Ok(Box::new(X11Capturer::new(
            CaptureTarget::Monitor(monitor.clone()),
            CaptureOptions::default(),
        )?))
    }

    fn create_window_capturer(
        &self,
        _window: &crate::WindowId,
    ) -> CaptureResult<Box<dyn MonitorCapturer>> {
        Err(window_capture_unsupported())
    }
}

impl LinuxBackend {
    /// Geometry of what a capture of `target` would actually read.
    ///
    /// Reports the clamped rectangle, not the requested one: the request is
    /// brought inside the root window before it is sent, and a caller sizing a
    /// buffer around the answer has to see that.
    fn target_info(&self, target: &CaptureTarget) -> CaptureResult<CaptureTargetInfo> {
        let session = open_session()?;
        let rect = target_rect(target, &session)?;
        let effective = rect.intersect(&session.root_rect()).ok_or_else(|| {
            CaptureError::InvalidConfig("the target does not overlap the X11 root window".into())
        })?;
        Ok(CaptureTargetInfo {
            origin_x: effective.x,
            origin_y: effective.y,
            width: effective.width,
            height: effective.height,
        })
    }
}

struct X11Capturer {
    session: X11Session,
    target: CaptureTarget,
    /// Updated by `set_output_pixel_format`, which the session may call after
    /// construction; the constructor's `CaptureOptions` value is only the
    /// starting point.
    output_format: CapturePixelFormat,
    /// Extent of the previous capture, used to report a reconfiguration as a
    /// resolution change instead of handing out a differently sized frame.
    last_extent: Option<(u32, u32)>,
}

impl X11Capturer {
    fn new(target: CaptureTarget, options: CaptureOptions) -> CaptureResult<Self> {
        if !matches!(
            options.backend_tuning,
            crate::tuning::BackendTuning::Default
        ) {
            return Err(CaptureError::BackendUnavailable(
                "Windows capture tuning is unavailable on Linux".into(),
            ));
        }
        let session = open_session()?;
        // Resolve the target once so a monitor that has already disappeared
        // fails at construction rather than on the first frame.
        target_rect(&target, &session)?;
        Ok(Self {
            session,
            target,
            output_format: options.output_pixel_format,
            last_extent: None,
        })
    }

    fn output_order(&self) -> OutputOrder {
        match self.output_format {
            CapturePixelFormat::Bgra8 => OutputOrder::Bgra,
            CapturePixelFormat::Rgba8 => OutputOrder::Rgba,
        }
    }
}

impl MonitorCapturer for X11Capturer {
    fn backend_kind(&self) -> CaptureBackendKind {
        CaptureBackendKind::X11
    }

    fn capture(&mut self, reuse: Option<Frame>) -> CaptureResult<Frame> {
        let requested = target_rect(&self.target, &self.session)?;
        // Read the output order first: `capture_image` borrows the session until
        // the returned image is dropped, and that image borrows the capture
        // buffer inside it.
        let order = self.output_order();
        let image = self.session.capture_image(requested).map_err(map_error)?;
        let width = image.rect.width;
        let height = image.rect.height;

        if let Some(previous) = self.last_extent
            && previous != (width, height)
        {
            // Forget the previous extent as well: the caller is about to rebuild
            // its buffers, and the next frame legitimately differs from this one.
            self.last_extent = None;
            return Err(CaptureError::ResolutionChanged(width, height));
        }
        self.last_extent = Some((width, height));

        let mut frame = reuse.unwrap_or_else(Frame::empty);
        frame.prepare_for_overwrite(width, height, self.output_format)?;
        image
            .decode_into(frame.as_mut_bytes(), order)
            .map_err(map_error)?;

        frame.metadata = Default::default();
        frame.metadata.backend_kind = CaptureBackendKind::X11;
        // X11 hands out no capture timestamp -- `GetImage` returns pixels and
        // nothing else -- so the observation time is this call's own monotonic
        // instant, reported without OS ticks. Consumers then take the `Instant`
        // path in `TimestampAnchor::stream_relative`, which is the only timing
        // information that actually exists here.
        frame.metadata.set_timing_with_format(
            Some(Instant::now()),
            None,
            TickFormat::Rational {
                timescale: 1_000_000_000,
                domain: ClockDomain::Session,
                epoch: 0,
            },
        );
        Ok(frame)
    }

    fn set_output_pixel_format(&mut self, format: CapturePixelFormat) -> CaptureResult<()> {
        self.output_format = format;
        Ok(())
    }

    fn capture_access_active(&self) -> bool {
        // Every capture is a self-contained server read; nothing survives
        // between frames that could be released early.
        false
    }
}
