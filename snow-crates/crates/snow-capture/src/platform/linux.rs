//! X11 screen capture on the root window.
//!
//! One backend, one method: core `GetImage` against the root window, decoded
//! into packed 8-bit pixels. There is no GPU path and no event stream behind it,
//! so the capturer is a thin translation between `snow-capture`'s target
//! geometry and `snow-linux`'s X11 session -- all of the protocol work, and
//! everything that can be wrong about pixel layout, lives in
//! `snow-linux::capture` where it is unit-tested off-Linux.
//!
//! "One method" does not mean "one rectangle": a region capture asks the server
//! for just the sub-rectangle it needs and decodes it straight into the
//! caller's frame, whose row stride is wider than the rectangle. Reading the
//! whole monitor and cropping afterwards moves the same bytes twice for nothing.
//!
//! Change tracking is the exception, and it is opt-in per frame: when the caller
//! says the buffer it passed in already holds this target's previous output,
//! `snow-linux::damage` decides whether the frame can be skipped entirely or
//! re-encoded partially. Both shortcuts are bounded in time, because an
//! undetected change would otherwise freeze the recording rather than slow it
//! down.
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
//! * A Wayland session is refused outright, and that refusal is the point of
//!   the [`capture_plan`] check below. XWayland exports `DISPLAY` there, so an
//!   X11 connection succeeds and `GetImage` returns real pixels -- of the
//!   XWayland root window, which holds only X clients. A capture would "succeed"
//!   with a picture of a desktop that has nothing on it, and nothing downstream
//!   could tell that from a genuinely empty screen.

use std::time::{Duration, Instant};

use snow_core::timestamp::TickFormat;
use snow_linux::capture::{OutputOrder, OutputWindow, RegionBlit, region_read_rect};
use snow_linux::damage::{Changes, DamagePolicy, Decision, decide, duplicate_hold};
use snow_linux::display::MonitorDesc;
use snow_linux::geometry::Rect as X11Rect;
use snow_linux::portal::{CapturePlan, capture_plan};
use snow_linux::session::SessionKind;
use snow_linux::x11::X11Session;
use snow_linux::x11::damage::DamageTracker;
use snow_media::time::ClockDomain;

use crate::backend::{
    CaptureBackend, CaptureBackendKind, CaptureBlitRegion, CaptureSampleMetadata, MonitorCapturer,
};
use crate::error::{CaptureError, CaptureResult};
use crate::frame::{CapturePixelFormat, DirtyRect, Frame};
use crate::region::{MonitorGeometry, MonitorLayout};
#[cfg(feature = "stage-timing")]
use crate::timing::{StageScope, attach_stage_timings};
use crate::timing::{stage_checkpoint, stage_record_since};
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

/// How long the non-interactive portal probe may take.
///
/// The probe is one property read on the session bus, so a portal that is
/// merely slow still answers well within this. A longer bound would turn
/// "the portal is hung" into a capture that hangs with it, and a shorter one
/// would misreport a busy machine as "no portal".
const PORTAL_PROBE_TIMEOUT: Duration = Duration::from_secs(3);

/// Decide whether this session can be captured by reading the root window.
///
/// The portal is only contacted on a Wayland session: on X11 the answer does
/// not depend on it, and a round trip nobody needs is a round trip that can
/// fail.
fn session_plan() -> CapturePlan {
    let session = SessionKind::detect();
    let portal = if session.is_wayland() {
        snow_linux::portal::dbus::screencast_version(PORTAL_PROBE_TIMEOUT)
    } else {
        None
    };
    capture_plan(session, portal)
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
    /// `None` when damage cannot be trusted here: no extension, no XFIXES, or
    /// no compositor to make root damage cover the whole desktop. Every frame is
    /// then read from the server, which is what this backend did before.
    damage: Option<DamageTracker>,
    policy: DamagePolicy,
    /// When pixels were last read from the server. Drives the heartbeat that
    /// bounds how long a skipped frame may last.
    last_capture: Option<Instant>,
    /// When a frame without dirty rectangles was last handed out. Drives the
    /// full-frame refresh that bounds how long a bad partial frame can persist.
    last_full: Option<Instant>,
    /// Requested stream rate, used to pace skipped frames instead of polling the
    /// server as fast as it answers.
    target_fps: u32,
    /// The root-window rectangle of the last region read and its extent, so a
    /// region capture can tell "this window is unchanged" from "a different
    /// window is unchanged".
    ///
    /// Separate from `last_extent` on purpose: the same capturer serves both
    /// region reads and whole-monitor fallbacks (the fallback is what runs when
    /// the region path declines), and a shared extent would make every such
    /// transition look like a resolution change.
    last_window: Option<(X11Rect, (u32, u32))>,
    /// Mirrors `CaptureOptions::record_stage_timings`; set by the session after
    /// construction, so the constructor's value is only the starting point.
    ///
    /// Without this the option was silently ignored on Linux: nothing here
    /// opened a `StageScope`, so every marker compiled to a no-op and frames
    /// came back with an empty breakdown even though `capture_duration` was
    /// filled in by the session.
    #[cfg(feature = "stage-timing")]
    record_stage_timings: bool,
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
        // Before connecting, not after: on Wayland the connection succeeds and
        // the failure only shows up as wrong pixels.
        if let CapturePlan::Unsupported(reason) = session_plan() {
            return Err(CaptureError::BackendUnavailable(reason.message()));
        }
        let session = open_session()?;
        // Resolve the target once so a monitor that has already disappeared
        // fails at construction rather than on the first frame.
        target_rect(&target, &session)?;
        // A tracker that cannot be created only costs frames -- the capturer
        // still reads the server every time. Turning that into a construction
        // failure would report "screen capture unavailable" for a session whose
        // only problem is a missing compositor or an old protocol version.
        let damage = DamageTracker::create(&session).ok().flatten();
        Ok(Self {
            session,
            target,
            output_format: options.output_pixel_format,
            last_extent: None,
            damage,
            policy: DamagePolicy::default(),
            last_capture: None,
            last_full: None,
            target_fps: 0,
            last_window: None,
            #[cfg(feature = "stage-timing")]
            record_stage_timings: options.record_stage_timings,
        })
    }

    /// Byte order for a frame this capturer is asked to fill.
    ///
    /// The destination decides, not `self.output_format`: a caller that
    /// allocated its own buffer may have asked for either order, and decoding
    /// into the wrong one produces a frame with red and blue swapped.
    fn order_for(format: CapturePixelFormat) -> OutputOrder {
        match format {
            CapturePixelFormat::Bgra8 => OutputOrder::Bgra,
            CapturePixelFormat::Rgba8 => OutputOrder::Rgba,
        }
    }

    fn output_order(&self) -> OutputOrder {
        Self::order_for(self.output_format)
    }

    /// The part of the root window a target request actually covers.
    ///
    /// Damage rectangles are compared against this, so it has to be the clamped
    /// rectangle and not the requested one.
    fn effective_rect(&self, requested: X11Rect) -> CaptureResult<X11Rect> {
        requested
            .intersect(&self.session.root_rect())
            .ok_or_else(|| {
                CaptureError::InvalidConfig(
                    "the target does not overlap the X11 root window".into(),
                )
            })
    }

    /// Read the accumulated change set, or `None` when tracking is off.
    ///
    /// A failing protocol call drops the tracker: the reasons this can fail --
    /// the compositor restarted, the damage object was destroyed -- would
    /// otherwise repeat a round trip that cannot succeed on every single frame.
    fn take_changes(&mut self, area: X11Rect) -> Option<Changes> {
        let drain_begin = stage_checkpoint();
        let result = self
            .damage
            .as_mut()
            .map(|tracker| tracker.take(&self.session, area));
        stage_record_since("x11.damage_drain", drain_begin);
        match result {
            Some(Ok(changes)) => Some(changes),
            Some(Err(_)) => {
                self.damage = None;
                None
            }
            None => None,
        }
    }

    /// Discard accumulated damage without looking at it.
    ///
    /// Used by the plain `capture` path, which never skips: leaving the region
    /// to accumulate would make the next `capture_with_history_hint` answer with
    /// an unbounded backlog instead of this frame's change.
    fn discard_changes(&mut self, area: X11Rect) {
        let drain_begin = stage_checkpoint();
        let result = self
            .damage
            .as_mut()
            .map(|tracker| tracker.take(&self.session, area));
        stage_record_since("x11.damage_drain", drain_begin);
        if matches!(result, Some(Err(_))) {
            self.damage = None;
        }
    }

    /// Read the target and build a frame, optionally tagged with dirty
    /// rectangles.
    fn read_frame(&mut self, reuse: Option<Frame>, dirty: Vec<DirtyRect>) -> CaptureResult<Frame> {
        let requested = target_rect(&self.target, &self.session)?;
        // Read the output order first: `capture_image` borrows the session until
        // the returned image is dropped, and that image borrows the capture
        // buffer inside it.
        let order = self.output_order();
        let read_begin = stage_checkpoint();
        let image = self.session.capture_image(requested).map_err(map_error)?;
        stage_record_since("x11.sys.get_image", read_begin);
        let width = image.rect.width;
        let height = image.rect.height;

        if let Some(previous) = self.last_extent
            && previous != (width, height)
        {
            // Forget the previous extent as well: the caller is about to rebuild
            // its buffers, and the next frame legitimately differs from this one.
            self.last_extent = None;
            self.last_full = None;
            return Err(CaptureError::ResolutionChanged(width, height));
        }
        self.last_extent = Some((width, height));

        let mut frame = reuse.unwrap_or_else(Frame::empty);
        frame.prepare_for_overwrite(width, height, self.output_format)?;
        let decode_begin = stage_checkpoint();
        image
            .decode_into(frame.as_mut_bytes(), order)
            .map_err(map_error)?;
        stage_record_since("x11.decode", decode_begin);

        let now = Instant::now();
        frame.metadata = Default::default();
        frame.metadata.backend_kind = CaptureBackendKind::X11;
        // X11 hands out no capture timestamp -- `GetImage` returns pixels and
        // nothing else -- so the observation time is this call's own monotonic
        // instant, reported without OS ticks. Consumers then take the `Instant`
        // path in `TimestampAnchor::stream_relative`, which is the only timing
        // information that actually exists here.
        frame.metadata.set_timing_with_format(
            Some(now),
            None,
            TickFormat::Rational {
                timescale: 1_000_000_000,
                domain: ClockDomain::Session,
                epoch: 0,
            },
        );
        // Set after `prepare_for_overwrite`, which clears them.
        frame.metadata.dirty_rects = dirty;
        self.last_capture = Some(now);
        if frame.metadata.dirty_rects.is_empty() {
            self.last_full = Some(now);
        }
        Ok(frame)
    }

    /// Metadata for a region capture, which has no frame of its own to carry it.
    ///
    /// Same clock story as [`Self::read_frame`]: X11 reports no capture
    /// timestamp, so the observation instant is this call's own.
    fn region_sample(
        now: Instant,
        is_duplicate: bool,
        dirty: Vec<DirtyRect>,
    ) -> CaptureSampleMetadata {
        CaptureSampleMetadata {
            capture_time: Some(now),
            raw_os_ticks: None,
            tick_format: TickFormat::Rational {
                timescale: 1_000_000_000,
                domain: ClockDomain::Session,
                epoch: 0,
            },
            is_duplicate,
            dirty_rects: dirty,
        }
    }

    /// Read only the part of the target a region capture needs.
    ///
    /// The generic path reads the whole monitor and copies the window out of it.
    /// For a 300x200 region on a 2560x1440 head that moves 14 MB from the server
    /// and then 14 MB again on this side, to produce 240 KB. `GetImage` has
    /// always accepted a sub-rectangle; what it could not do was land the reply
    /// in a destination with a different stride, which
    /// [`snow_linux::capture::ImageLayout::decode_window_into`] now handles.
    ///
    /// `Ok(None)` means "use the caller's fallback": that path is slower, not
    /// wrong, so every condition that would make this one delicate is declined
    /// rather than handled differently here.
    fn read_region_into(
        &mut self,
        blit: CaptureBlitRegion,
        destination: &mut Frame,
        destination_has_history: bool,
    ) -> CaptureResult<Option<CaptureSampleMetadata>> {
        let base = target_rect(&self.target, &self.session)?;
        let Some(window) = region_read_rect(
            RegionBlit {
                src_x: blit.src_x,
                src_y: blit.src_y,
                width: blit.width,
                height: blit.height,
                dst_x: blit.dst_x,
                dst_y: blit.dst_y,
            },
            base,
            self.session.root_rect(),
            destination.dimensions(),
        ) else {
            return Ok(None);
        };

        // Ask for the change set before reading: it decides both whether the
        // pixels are needed at all and what the caller may re-encode. The
        // rectangles come back local to `window`, which is exactly the form a
        // sample carries (the caller adds its own destination offset).
        let changes = self.take_changes(window);
        let now = Instant::now();
        let since_capture = self
            .last_capture
            .map_or(Duration::MAX, |at| now.saturating_duration_since(at));
        let since_full = self
            .last_full
            .map_or(Duration::MAX, |at| now.saturating_duration_since(at));
        let decision =
            changes.map(|changes| decide(&changes, since_capture, since_full, &self.policy));

        // Skipping needs the same two guarantees as the full-frame path, plus
        // one more: that the previous read covered *this* window at *this*
        // extent. Without it, "nothing changed" would leave a destination that
        // still holds a different region's pixels.
        if destination_has_history
            && self.last_window == Some((window, (blit.width, blit.height)))
            && matches!(decision, Some(Decision::Skip))
        {
            // Nothing is read, so the server answers instantly; without a pause
            // the caller would spin at the speed of its own loop.
            std::thread::sleep(duplicate_hold(self.target_fps));
            return Ok(Some(Self::region_sample(now, true, Vec::new())));
        }

        let order = Self::order_for(destination.pixel_format());
        let (width, height) = destination.dimensions();
        let image = self.session.capture_image(window).map_err(map_error)?;
        if image.rect != window {
            // `capture_image` clamps to the root window, and `window` was
            // checked to be inside it. If those two ever disagree the reply is
            // still a valid picture of somewhere else, so refuse rather than
            // write it one row off.
            return Ok(None);
        }
        image
            .decode_window_into(
                destination.as_mut_bytes(),
                OutputWindow {
                    width,
                    height,
                    x: blit.dst_x,
                    y: blit.dst_y,
                },
                order,
            )
            .map_err(map_error)?;

        let dirty: Vec<DirtyRect> = match decision {
            Some(Decision::Partial(rects)) => rects.iter().filter_map(dirty_rect).collect(),
            _ => Vec::new(),
        };
        self.last_window = Some((window, (blit.width, blit.height)));
        self.last_capture = Some(now);
        if dirty.is_empty() {
            self.last_full = Some(now);
        }
        Ok(Some(Self::region_sample(now, false, dirty)))
    }

    /// The history-hint path, split out so the public entry point can open one
    /// stage scope and attach the result to whichever branch produced the frame.
    ///
    /// Every return here is a real frame: a skipped one is the previous frame
    /// re-labelled, so it carries the timings of the call that decided not to
    /// read the server. That is the number worth having -- a skip that costs as
    /// much as a read is a policy bug, and without instrumentation it looks like
    /// an ordinary stream.
    fn capture_hinted(
        &mut self,
        reuse: Option<Frame>,
        destination_has_history: bool,
    ) -> CaptureResult<Frame> {
        let requested = target_rect(&self.target, &self.session)?;
        let effective = self.effective_rect(requested)?;

        let Some(changes) = self.take_changes(effective) else {
            return self.read_frame(reuse, Vec::new());
        };

        let now = Instant::now();
        let since_capture = self
            .last_capture
            .map_or(Duration::MAX, |at| now.saturating_duration_since(at));
        let since_full = self
            .last_full
            .map_or(Duration::MAX, |at| now.saturating_duration_since(at));

        match decide(&changes, since_capture, since_full, &self.policy) {
            // Skipping needs both: a buffer the session vouches holds this
            // target's previous output, and at least one frame of our own, since
            // a first frame has nothing to repeat.
            Decision::Skip
                if destination_has_history && reuse.is_some() && self.last_extent.is_some() =>
            {
                let Some(frame) = reuse else {
                    return self.read_frame(None, Vec::new());
                };
                // Nothing is painted, so the server answers instantly; without a
                // pause the loop would spin as fast as it can ask. The pause is
                // the hold a stream at this rate would have anyway.
                let hold_begin = stage_checkpoint();
                std::thread::sleep(duplicate_hold(self.target_fps));
                stage_record_since("x11.skip_hold", hold_begin);
                Ok(duplicate_frame(frame))
            }
            Decision::Skip | Decision::Full => self.read_frame(reuse, Vec::new()),
            Decision::Partial(rects) => {
                let dirty: Vec<DirtyRect> = rects.iter().filter_map(dirty_rect).collect();
                self.read_frame(reuse, dirty)
            }
        }
    }
}

/// Re-label a previous frame as unchanged.
///
/// The pixels are deliberately left as they were: they are the previous output
/// for this exact target, which is what a duplicate frame means.
fn duplicate_frame(mut frame: Frame) -> Frame {
    frame.metadata = Default::default();
    frame.metadata.backend_kind = CaptureBackendKind::X11;
    frame.metadata.is_duplicate = true;
    frame.metadata.set_timing_with_format(
        Some(Instant::now()),
        None,
        TickFormat::Rational {
            timescale: 1_000_000_000,
            domain: ClockDomain::Session,
            epoch: 0,
        },
    );
    frame
}

/// Target-local rectangle to the frame's dirty-rectangle form.
///
/// `localize` clips everything inside the target, so the `u32` conversion cannot
/// fail in practice; `None` drops a rectangle rather than wrapping it into a
/// coordinate at the far end of the buffer.
fn dirty_rect(rect: &X11Rect) -> Option<DirtyRect> {
    Some(DirtyRect {
        x: u32::try_from(rect.x).ok()?,
        y: u32::try_from(rect.y).ok()?,
        width: rect.width,
        height: rect.height,
    })
}

impl MonitorCapturer for X11Capturer {
    fn backend_kind(&self) -> CaptureBackendKind {
        CaptureBackendKind::X11
    }

    fn capture(&mut self, reuse: Option<Frame>) -> CaptureResult<Frame> {
        #[cfg(feature = "stage-timing")]
        let stages = StageScope::enter(self.record_stage_timings);
        let requested = target_rect(&self.target, &self.session)?;
        // This path is used when the caller cannot vouch for the buffer's
        // provenance, so nothing may be skipped and nothing may be called
        // unchanged. The change set is still consumed, to keep it describing one
        // frame rather than every frame since the tracker was created.
        let effective = self.effective_rect(requested)?;
        self.discard_changes(effective);
        // `mut` exists only for the feature-gated attach below.
        #[cfg_attr(not(feature = "stage-timing"), allow(unused_mut))]
        let mut frame = self.read_frame(reuse, Vec::new())?;
        #[cfg(feature = "stage-timing")]
        attach_stage_timings(&mut frame, stages);
        Ok(frame)
    }

    fn capture_with_history_hint(
        &mut self,
        reuse: Option<Frame>,
        destination_has_history: bool,
    ) -> CaptureResult<Frame> {
        #[cfg(feature = "stage-timing")]
        let stages = StageScope::enter(self.record_stage_timings);
        // `mut` exists only for the feature-gated attach below.
        #[cfg_attr(not(feature = "stage-timing"), allow(unused_mut))]
        let mut frame = self.capture_hinted(reuse, destination_has_history)?;
        #[cfg(feature = "stage-timing")]
        attach_stage_timings(&mut frame, stages);
        Ok(frame)
    }

    fn capture_region_into(
        &mut self,
        blit: CaptureBlitRegion,
        destination: &mut Frame,
        destination_has_history: bool,
    ) -> CaptureResult<Option<CaptureSampleMetadata>> {
        // Region samples carry no stage timings: `CaptureSampleMetadata` has no
        // field for them, so opening a scope here would measure into a breakdown
        // that is then dropped.
        self.read_region_into(blit, destination, destination_has_history)
    }

    fn set_target_fps(&mut self, fps: u32) -> CaptureResult<()> {
        // Only used to pace skipped frames: a real capture is already as fast as
        // the server answers, and throttling those is the session's job.
        self.target_fps = fps;
        Ok(())
    }

    fn set_output_pixel_format(&mut self, format: CapturePixelFormat) -> CaptureResult<()> {
        self.output_format = format;
        Ok(())
    }

    #[cfg(feature = "stage-timing")]
    fn set_record_stage_timings(&mut self, enabled: bool) -> CaptureResult<()> {
        // The session calls this after construction, so it has to land in a
        // field: a marker's scope is opened per capture call, not per capturer.
        self.record_stage_timings = enabled;
        Ok(())
    }

    fn capture_access_active(&self) -> bool {
        // Every capture is a self-contained server read; nothing survives
        // between frames that could be released early.
        false
    }
}
