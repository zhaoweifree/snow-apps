//! X11 window tree plus the AT-SPI2 widget tree on top of it.
//!
//! Two sources, because neither alone answers "what is under the pointer":
//!
//! 1. **X11** knows which window the point is in. `_NET_CLIENT_LIST` is the
//!    window manager's own stacking order, so the topmost window whose frame
//!    contains the point is the one the user sees. That is a rectangle and
//!    nothing more.
//! 2. **AT-SPI2** knows the widgets inside that window, on a second D-Bus
//!    connection. It is matched to the window by process id -- `_NET_WM_PID`
//!    on the frame against `GetConnectionUnixProcessID` per application --
//!    because a bus name says nothing about which window it draws.
//!
//! `HitTestMode::Window` stops after the first step. `HitTestMode::UiElement`
//! asks the second, and reports the window rectangle when the accessibility
//! route is unavailable or silent: a desktop with accessibility switched off
//! still selects windows, and a widget that is not in the accessibility tree
//! still sits inside one.
//!
//! Root coordinates are already pixels, so the `displays` argument that the
//! HiDPI platforms need for a logical-to-pixel mapping carries no information
//! here and is ignored.

use std::marker::PhantomData;
use std::time::Instant;

use snow_atspi::{AccessibilityBus, Bounds};
use snow_linux::geometry::Rect;
use snow_linux::x11::X11Session;

use crate::{
    AccessibilityBackend, DisplayGeometry, ElementRect, HitTestMode, PixelRect, Point,
    QueryControl, QueryResult, SelectorResult, StopReason, WindowSnapshot,
};

/// One selectable window, in root coordinates.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct WindowInfo {
    pub(crate) id: u64,
    pub(crate) bounds: PixelRect,
    /// The process behind the window, when it published `_NET_WM_PID`.
    ///
    /// This is what ties the X11 window to the application on the accessibility
    /// bus. `None` means the query can only report the window: there is nothing
    /// to match, and matching on a zero would pair the window with an unrelated
    /// application that also failed to publish one.
    pub(crate) pid: Option<u32>,
}

/// The main entry point for hit-testing windows and, above them, widgets.
///
/// `!Send`/`!Sync` for the same reason the other platforms are: the service is
/// created and used on the worker thread that owns the selection session, and
/// nothing about the window list or the bus connection should tempt a caller to
/// move it.
pub struct ElementRegionService {
    snapshot: WindowSnapshot,
    /// The accessibility bus, connected on first use.
    ///
    /// Attempted once and the outcome kept: a desktop with accessibility
    /// switched off answers the same way for the life of the process, and a
    /// failed handshake is a D-Bus round trip that is not free. `None` therefore
    /// means "tried and unavailable" as much as "not tried yet";
    /// `bus_attempted` tells them apart.
    bus: Option<AccessibilityBus>,
    bus_attempted: bool,
    _thread_bound: PhantomData<*mut ()>,
}

impl ElementRegionService {
    pub fn new() -> SelectorResult<Self> {
        Self::with_backend(AccessibilityBackend::Accessibility)
    }

    pub fn with_backend(backend: AccessibilityBackend) -> SelectorResult<Self> {
        Self::with_backend_excluding_ids(backend, &[])
    }

    pub fn with_backend_excluding_ids(
        backend: AccessibilityBackend,
        excluded: &[usize],
    ) -> SelectorResult<Self> {
        Self::with_backend_and_displays(backend, excluded, None)
    }

    pub fn with_backend_and_displays(
        _: AccessibilityBackend,
        excluded: &[usize],
        displays: Option<&[DisplayGeometry]>,
    ) -> SelectorResult<Self> {
        Ok(Self {
            snapshot: snapshot(excluded, displays)?,
            bus: None,
            bus_attempted: false,
            _thread_bound: PhantomData,
        })
    }

    /// One provider, whatever was requested.
    ///
    /// `Msaa` and `Uia` name Windows' two accessibility technologies; on a
    /// request for either, the honest answer is the provider that exists here
    /// rather than an error the caller cannot act on.
    pub fn backend(&self) -> AccessibilityBackend {
        AccessibilityBackend::Accessibility
    }

    pub fn refresh(&mut self) -> SelectorResult<()> {
        self.refresh_excluding_ids(&[])
    }

    pub fn refresh_excluding_ids(&mut self, excluded: &[usize]) -> SelectorResult<()> {
        self.refresh_with_displays(excluded, None)
    }

    pub fn refresh_with_displays(
        &mut self,
        excluded: &[usize],
        displays: Option<&[DisplayGeometry]>,
    ) -> SelectorResult<()> {
        self.snapshot = snapshot(excluded, displays)?;
        Ok(())
    }

    /// Drops everything this provider holds: the window list, and the
    /// accessibility connection.
    ///
    /// The bus is a cache of nothing, but it costs a handshake to rebuild and a
    /// caller that releases between sessions is saying it will not ask again
    /// soon.
    pub fn release_cache(&mut self) {
        self.snapshot = WindowSnapshot::default();
        self.bus = None;
        self.bus_attempted = false;
    }

    pub fn window_snapshot(&self) -> Option<WindowSnapshot> {
        Some(self.snapshot.clone())
    }

    /// Rebuild a service over a snapshot, with no bus yet.
    ///
    /// The connection is deliberately not carried by the snapshot: snapshots
    /// cross threads, a `zbus::Connection` should not, and a restored service
    /// reconnects on its own first query.
    pub fn from_snapshot(snapshot: &WindowSnapshot) -> SelectorResult<Self> {
        Ok(Self {
            snapshot: snapshot.clone(),
            bus: None,
            bus_attempted: false,
            _thread_bound: PhantomData,
        })
    }

    /// Resolve what is under `point`.
    ///
    /// `HitTestMode::Window` returns the window rectangle. `HitTestMode::UiElement`
    /// returns the widget chain inside that window, innermost first and ending
    /// with the window, and falls back to the window alone whenever the widget
    /// tree cannot be read -- which is the same answer the window mode gives, so
    /// a caller never gets nothing where it used to get a rectangle.
    pub fn query(
        &mut self,
        point: Point,
        mode: HitTestMode,
        control: &QueryControl<'_>,
        progress: &mut dyn FnMut(&[ElementRect]),
    ) -> SelectorResult<QueryResult> {
        if (control.cancelled)() {
            return Ok(QueryResult {
                path: None,
                reason: StopReason::Cancelled,
            });
        }

        // The list is bottom-to-top, so the last hit is the one the user sees.
        // A caller-supplied window id narrows the search to that window, which
        // is how a UI thread that already resolved the pointer avoids
        // disagreeing with this traversal about overlapping frames.
        let hit = self
            .snapshot
            .windows
            .iter()
            .rev()
            .find(|window| {
                window.id != 0
                    && point.window_id.is_none_or(|id| window.id == id as u64)
                    && contains(&window.bounds, point.x, point.y)
            })
            .cloned();

        // No window covers the point: report the desktop so that a click on a
        // gap between windows still selects a region, which is what the macOS
        // provider does with its display rectangle.
        let window = hit
            .as_ref()
            .map_or(self.snapshot.desktop, |window| window.bounds);
        let window_element = ElementRect::new(window);

        if matches!(mode, HitTestMode::Window) {
            return Ok(QueryResult {
                path: Some(vec![window_element]),
                reason: StopReason::Complete,
            });
        }

        // Publish the window level answer before asking the bus: it is what the
        // overlay can draw immediately, and it is the answer if the widget tree
        // stays silent. Without it a slow toolkit shows nothing at all instead
        // of a coarse rectangle.
        progress(std::slice::from_ref(&window_element));

        let element_path = hit
            .as_ref()
            .and_then(|window| window.pid)
            .and_then(|pid| self.widget_path(pid, point, window, control));

        if (control.cancelled)() {
            return Ok(QueryResult {
                path: None,
                reason: StopReason::Cancelled,
            });
        }

        let path = match element_path {
            Some(path) if !path.is_empty() => path,
            _ => vec![window_element],
        };
        Ok(QueryResult {
            path: Some(path),
            reason: StopReason::Complete,
        })
    }

    /// The widget chain under `point`, or `None` when this route said nothing.
    ///
    /// Never an error. A selector that failed because a toolkit's accessibility
    /// tree was empty would be trading a working window-level answer for a
    /// diagnostic nobody asked for; the caller cannot act on "AT-SPI2 broke" any
    /// differently from "AT-SPI2 has nothing".
    ///
    /// The budget comes from the caller's [`QueryControl`] rather than from a
    /// constant here. It is the same number the Windows provider spends on its
    /// traversal, and the point of it is that the *whole* query is bounded, not
    /// each call inside it.
    fn widget_path(
        &mut self,
        pid: u32,
        point: Point,
        window: PixelRect,
        control: &QueryControl<'_>,
    ) -> Option<Vec<ElementRect>> {
        if !self.bus_attempted {
            self.bus_attempted = true;
            self.bus = AccessibilityBus::connect().ok();
        }
        let deadline = Instant::now().checked_add(control.budget);
        let path = self
            .bus
            .as_ref()?
            .point_path(pid, (point.x, point.y), bounds(window), deadline);
        if path.is_empty() {
            return None;
        }
        Some(path.into_iter().map(element).collect())
    }
}

fn snapshot(
    excluded: &[usize],
    displays: Option<&[DisplayGeometry]>,
) -> SelectorResult<WindowSnapshot> {
    // See the module note: root coordinates are pixels, so the display list
    // carries nothing this provider can use.
    let _ = displays;
    let session = X11Session::connect()?;
    let excluded: Vec<u64> = excluded.iter().map(|id| *id as u64).collect();
    let desktop = snow_linux::x11::window::desktop_windows_for(&session, &excluded)?;
    Ok(WindowSnapshot {
        windows: desktop
            .windows
            .iter()
            .map(|window| WindowInfo {
                id: window.id,
                bounds: pixel_rect(window.bounds),
                pid: window.pid,
            })
            .collect(),
        desktop: pixel_rect(desktop.desktop),
    })
}

fn pixel_rect(rect: Rect) -> PixelRect {
    PixelRect {
        left: rect.x,
        top: rect.y,
        right: rect.right(),
        bottom: rect.bottom(),
    }
}

/// An accessibility rectangle in the coordinates this crate reports.
///
/// `Bounds` carries a position plus a `u32` size, so the far edges are sums
/// done in `i64` and clamped: a widget that starts at `i32::MAX - 8` and is wide
/// enough to overflow would otherwise wrap into a rectangle that starts to the
/// *left* of where it ends, which every consumer would read as an empty or
/// inverted region.
fn element(bounds: Bounds) -> ElementRect {
    let right = i64::from(bounds.x) + i64::from(bounds.width);
    let bottom = i64::from(bounds.y) + i64::from(bounds.height);
    ElementRect::new(PixelRect {
        left: bounds.x,
        top: bounds.y,
        right: right.clamp(i64::from(i32::MIN), i64::from(i32::MAX)) as i32,
        bottom: bottom.clamp(i64::from(i32::MIN), i64::from(i32::MAX)) as i32,
    })
}

fn bounds(rect: PixelRect) -> Bounds {
    // A window rectangle is built from a `u32` size upstream, so this cannot go
    // negative; the clamp is here because the type allows it and a negative
    // size would silently become a huge one.
    let width = (i64::from(rect.right) - i64::from(rect.left)).clamp(0, i64::from(u32::MAX)) as u32;
    let height =
        (i64::from(rect.bottom) - i64::from(rect.top)).clamp(0, i64::from(u32::MAX)) as u32;
    Bounds::new(rect.left, rect.top, width, height)
}

/// Half-open containment, matching the X11 root coordinate convention.
fn contains(rect: &PixelRect, x: i32, y: i32) -> bool {
    x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom
}
