//! Monitor enumeration.
//!
//! RandR 1.5 is authoritative when available; otherwise the root window's
//! geometry is reported as a single monitor so callers always get a usable
//! layout instead of an error.

use crate::error::LinuxResult;
use crate::geometry::{Rect, union_all};
use crate::x11::X11Session;

/// One logical monitor, in root-window (desktop) coordinates.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct MonitorDesc {
    pub name: String,
    pub is_primary: bool,
    pub rect: Rect,
    /// Physical size in millimetres; `0` when the connector reports none, which
    /// is how DPI-dependent features detect "unknown" instead of dividing by it.
    pub mm_width: u32,
    pub mm_height: u32,
}

impl MonitorDesc {
    /// Identity that survives compositor restarts and reconnect renumbering.
    pub fn stable_id(&self) -> String {
        format!("x11-{}", self.name)
    }
}

/// Enumerate every logical monitor on the default screen.
///
/// Guarantees a non-empty result whenever a session could be reached: RandR
/// output is used as-is, and a session without RandR reports the whole root
/// window.
pub fn enumerate_monitors(session: &X11Session) -> LinuxResult<Vec<MonitorDesc>> {
    if session.extension_support().randr {
        let raw = session.randr_monitors()?;
        if !raw.is_empty() {
            let mut monitors = Vec::with_capacity(raw.len());
            for monitor in raw {
                monitors.push(MonitorDesc {
                    name: session.atom_name(monitor.name).unwrap_or_default(),
                    is_primary: monitor.primary,
                    rect: Rect::new(
                        i32::from(monitor.x),
                        i32::from(monitor.y),
                        u32::from(monitor.width),
                        u32::from(monitor.height),
                    ),
                    mm_width: monitor.width_in_millimeters,
                    mm_height: monitor.height_in_millimeters,
                });
            }
            return Ok(monitors);
        }
    }

    Ok(vec![root_monitor(session)])
}

/// Pick the primary monitor, synthesising one from the root window if RandR
/// reports no monitor flagged primary (common on bare Xvfb / wlroots setups).
pub fn primary_monitor(session: &X11Session) -> LinuxResult<MonitorDesc> {
    let monitors = enumerate_monitors(session)?;
    if let Some(primary) = monitors.iter().find(|monitor| monitor.is_primary) {
        return Ok(primary.clone());
    }

    // Failing an explicit flag, the head that owns the desktop origin is the
    // best stand-in: X11 lays out every other head relative to it.
    if let Some(origin) = monitors
        .iter()
        .find(|monitor| monitor.rect.x <= 0 && monitor.rect.y <= 0)
    {
        return Ok(origin.clone());
    }

    Ok(root_monitor(session))
}

/// A synthetic monitor describing the entire default screen.
pub fn root_monitor(session: &X11Session) -> MonitorDesc {
    let screen = session.screen();
    MonitorDesc {
        name: "root".to_string(),
        is_primary: true,
        rect: Rect::new(
            0,
            0,
            u32::from(screen.width_in_pixels),
            u32::from(screen.height_in_pixels),
        ),
        mm_width: u32::from(screen.width_in_millimeters),
        mm_height: u32::from(screen.height_in_millimeters),
    }
}

/// Union of every monitor rectangle, in desktop coordinates.
///
/// Used to size full-desktop captures; `None` only when `monitors` is empty,
/// which [`enumerate_monitors`] never produces.
pub fn desktop_rect(monitors: &[MonitorDesc]) -> Option<Rect> {
    let rects: Vec<Rect> = monitors.iter().map(|monitor| monitor.rect).collect();
    union_all(&rects)
}

/// The monitor containing `x`/`y`, if any.
///
/// Overlapping heads are impossible under RandR 1.5, so the first match is
/// unambiguous.
pub fn monitor_at(monitors: &[MonitorDesc], x: i32, y: i32) -> Option<&MonitorDesc> {
    monitors.iter().find(|monitor| monitor.rect.contains(x, y))
}
