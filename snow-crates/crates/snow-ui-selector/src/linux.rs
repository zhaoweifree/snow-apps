//! X11 window-level pointer provider.
//!
//! The pointer's target is resolved from the window manager's client list: the
//! topmost window whose frame contains the point. That is the entire traversal
//! this backend performs, and it is deliberate rather than partial. Control
//! level granularity on Linux comes from AT-SPI2, which is a separate protocol
//! over a separate bus and is not implemented yet; pretending otherwise would
//! mean returning a window rectangle as if it were a widget.
//!
//! What that costs, stated plainly: `HitTestMode::UiElement` and
//! `HitTestMode::Window` return the same single-element path here. A caller
//! that needs the control under the pointer must treat this provider as the
//! window-level fallback it is. It is the same shape the MSAA backend has on
//! Windows, where a point hit test also yields one element.
//!
//! Root coordinates are already pixels, so the `displays` argument that the
//! HiDPI platforms need for a logical-to-pixel mapping carries no information
//! here and is ignored.

use std::marker::PhantomData;

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
}

/// The main entry point for hit-testing windows.
///
/// `!Send`/`!Sync` for the same reason the other platforms are: the service is
/// created and used on the worker thread that owns the selection session, and
/// nothing about the window list should tempt a caller to move it.
pub struct ElementRegionService {
    snapshot: WindowSnapshot,
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
            _thread_bound: PhantomData,
        })
    }

    /// The X11 provider has no second strategy to select: there is one window
    /// tree, and AT-SPI2 -- the only alternative -- is not wired up yet.
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

    /// Drops the window list. The X11 provider holds no long-lived server
    /// object, so there is nothing else to release.
    pub fn release_cache(&mut self) {
        self.snapshot = WindowSnapshot::default();
    }

    pub fn window_snapshot(&self) -> Option<WindowSnapshot> {
        Some(self.snapshot.clone())
    }

    pub fn from_snapshot(snapshot: &WindowSnapshot) -> SelectorResult<Self> {
        Ok(Self {
            snapshot: snapshot.clone(),
            _thread_bound: PhantomData,
        })
    }

    /// Resolve the window under `point`.
    ///
    /// `mode` is accepted but does not change the answer: without a control
    /// level provider both modes report the window rectangle, which is what
    /// `HitTestMode::Window` asks for and the best available answer for
    /// `HitTestMode::UiElement`.
    pub fn query(
        &mut self,
        point: Point,
        _mode: HitTestMode,
        control: &QueryControl<'_>,
        _progress: &mut dyn FnMut(&[ElementRect]),
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
        let hit = self.snapshot.windows.iter().rev().find(|window| {
            window.id != 0
                && point.window_id.is_none_or(|id| window.id == id as u64)
                && contains(&window.bounds, point.x, point.y)
        });

        // No window covers the point: report the desktop so that a click on a
        // gap between windows still selects a region, which is what the macOS
        // provider does with its display rectangle.
        let rect = hit.map_or(self.snapshot.desktop, |window| window.bounds);
        Ok(QueryResult {
            path: Some(vec![ElementRect::new(rect)]),
            reason: StopReason::Complete,
        })
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

/// Half-open containment, matching the X11 root coordinate convention.
fn contains(rect: &PixelRect, x: i32, y: i32) -> bool {
    x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom
}
