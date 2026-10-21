//! Desktop window enumeration for pointer hit testing.
//!
//! Selecting what is under the pointer starts with knowing which window it is.
//! The X11 layer reads the attributes; everything that decides *which* windows
//! count and *which* one wins lives here, free of the server, so the rules can
//! be tested with a table of attributes.
//!
//! The one rule worth stating up front: `_NET_CLIENT_LIST` is ordered
//! **bottom-to-top**, so a hit test searches it backwards. Getting that
//! direction wrong is not a crash -- it silently selects the window at the
//! bottom of the stack, which looks like the selector being wrong about which
//! application the user is pointing at.

use crate::geometry::Rect;

/// How a top-level window presents itself to the window manager.
///
/// Derived from `_NET_WM_WINDOW_TYPE`. Only the kinds that are never a pointer
/// target are distinguished.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum WindowKind {
    /// An application window: the only kind a hit test may select.
    Normal,
    /// The full-screen backdrop (`_NET_WM_WINDOW_TYPE_DESKTOP`).
    Desktop,
    /// A panel, taskbar or dock (`_NET_WM_WINDOW_TYPE_DOCK`).
    Dock,
    /// A tooltip, menu, notification or drag icon: a surface that belongs to
    /// another window's interaction rather than being a target of its own.
    Transient,
}

/// The window-type atoms the X11 layer interned, paired with their meaning.
///
/// The roles are passed in rather than looked up here because atom identifiers
/// are assigned by the server at runtime. This keeps the classification pure
/// and lets a test drive it with arbitrary numbers.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct WindowTypeRoles<'a> {
    pub desktop: u32,
    pub dock: u32,
    pub transient: &'a [u32],
}

/// Fold a window's `_NET_WM_WINDOW_TYPE` list into a kind.
///
/// A window may advertise several types, and the specification says the first
/// one it understands wins. An empty or unrecognized list means a normal
/// window: the type property is optional, and treating its absence as
/// "unselectable" would hide every application that does not set it.
pub fn classify(types: &[u32], roles: &WindowTypeRoles<'_>) -> WindowKind {
    for value in types {
        if *value == roles.desktop {
            return WindowKind::Desktop;
        }
        if *value == roles.dock {
            return WindowKind::Dock;
        }
        if roles.transient.contains(value) {
            return WindowKind::Transient;
        }
    }
    WindowKind::Normal
}

/// The attributes the X11 layer reads for one candidate window.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct WindowAttributes {
    pub id: u64,
    pub kind: WindowKind,
    /// `IsViewable`: mapped, with every ancestor mapped too.
    pub viewable: bool,
    /// `override_redirect`, which is how menus and tooltips escape the window
    /// manager. Such a window is not managed, so it is not a selection target.
    pub override_redirect: bool,
    /// `InputOutput` rather than `InputOnly`: an input-only window has no
    /// pixels of its own and would be a rectangle with nothing in it.
    pub has_pixels: bool,
    /// Frame rectangle in root coordinates.
    pub bounds: Rect,
    /// The process that created the window, from `_NET_WM_PID`.
    ///
    /// `None` covers "the client did not publish one" and "it published zero",
    /// which are the same thing to every caller: there is no application behind
    /// this window to ask anything. Comparing a zero instead would match every
    /// other window that also failed to report one -- which is how a control
    /// picker ends up describing a widget belonging to a different program.
    pub pid: Option<u32>,
}

/// Whether a window is a surface the user can name as a target on its own.
///
/// Two questions share this rule -- "is the focused window the full-screen
/// application?" ([`crate::fullscreen`]) and "can the focused window be captured
/// by itself?" ([`crate::windowcapture`]) -- because they reject exactly the same
/// surfaces: the desktop backdrop, panels and menus cover a monitor without being
/// an application, and a hidden window keeps its geometry while showing nothing.
/// Keeping one function for both is what stops the two answers from drifting
/// apart, which would show up as a capture of the desktop labelled with a window
/// name.
pub fn is_paintable(kind: WindowKind, viewable: bool, frame: Rect) -> bool {
    kind == WindowKind::Normal && viewable && !frame.is_empty()
}

/// Whether a window is a pointer target at all.
pub fn selectable(attributes: &WindowAttributes) -> bool {
    attributes.kind == WindowKind::Normal
        && attributes.viewable
        && !attributes.override_redirect
        && attributes.has_pixels
        && !attributes.bounds.is_empty()
}

/// A selectable window.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct WindowInfo {
    pub id: u64,
    pub bounds: Rect,
    /// The process behind the window; see [`WindowAttributes::pid`].
    pub pid: Option<u32>,
}

/// The windows the pointer may select, over the desktop rectangle they sit on.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DesktopWindows {
    /// In bottom-to-top stacking order -- the order `_NET_CLIENT_LIST` reports,
    /// and the order [`DesktopWindows::hit_test`] searches backwards through.
    pub windows: Vec<WindowInfo>,
    /// The root window rectangle, returned when no window covers the point so
    /// that a click on bare desktop still selects something.
    pub desktop: Rect,
}

impl Default for DesktopWindows {
    fn default() -> Self {
        Self {
            windows: Vec::new(),
            desktop: Rect::new(0, 0, 0, 0),
        }
    }
}

impl DesktopWindows {
    /// The topmost window containing the point.
    ///
    /// Searches from the end of the list because the list is bottom-to-top:
    /// the last entry is the window the user sees.
    pub fn hit_test(&self, x: i32, y: i32) -> Option<&WindowInfo> {
        self.windows
            .iter()
            .rev()
            .find(|window| window.bounds.contains(x, y))
    }
}

/// Build the selectable set from raw attributes.
///
/// `excluded` carries the windows the caller does not want to select -- the
/// application's own overlay is the usual reason -- and is matched on the X11
/// window id. Order is preserved so the stacking order survives.
pub fn desktop_windows(
    attributes: &[WindowAttributes],
    excluded: &[u64],
    desktop: Rect,
) -> DesktopWindows {
    let windows = attributes
        .iter()
        .filter(|attributes| selectable(attributes))
        .filter(|attributes| !excluded.contains(&attributes.id))
        .map(|attributes| WindowInfo {
            id: attributes.id,
            bounds: attributes.bounds,
            pid: attributes.pid,
        })
        .collect();
    DesktopWindows { windows, desktop }
}

#[cfg(test)]
mod tests {
    use super::*;

    const ROLES: WindowTypeRoles = WindowTypeRoles {
        desktop: 300,
        dock: 301,
        transient: &[302, 303, 304],
    };

    fn normal(id: u64, x: i32, y: i32, width: u32, height: u32) -> WindowAttributes {
        WindowAttributes {
            id,
            kind: WindowKind::Normal,
            viewable: true,
            override_redirect: false,
            has_pixels: true,
            bounds: Rect::new(x, y, width, height),
            pid: Some(4321),
        }
    }

    #[test]
    fn the_process_id_travels_with_the_window() {
        // A caller that needs the application behind a window -- the control
        // picker asks the accessibility bus by pid -- must get it from the same
        // record as the rectangle, or the two can describe different windows.
        let attributes = [
            WindowAttributes {
                pid: None,
                ..normal(1, 0, 0, 100, 100)
            },
            normal(2, 0, 0, 100, 100),
        ];
        let desktop = desktop_windows(&attributes, &[], Rect::new(0, 0, 200, 200));
        assert_eq!(desktop.hit_test(50, 50).unwrap().pid, Some(4321));
        let unknown = desktop_windows(&attributes[..1], &[], Rect::new(0, 0, 200, 200));
        // "No pid" survives as `None` rather than as a zero that would match
        // every other unpublished window.
        assert_eq!(unknown.windows[0].pid, None);
    }

    #[test]
    fn the_topmost_window_wins() {
        // `_NET_CLIENT_LIST` is bottom-to-top, so the last entry is on top even
        // though its rectangle is identical.
        let attributes = [normal(1, 0, 0, 100, 100), normal(2, 0, 0, 100, 100)];
        let desktop = desktop_windows(&attributes, &[], Rect::new(0, 0, 200, 200));
        assert_eq!(desktop.hit_test(50, 50).unwrap().id, 2);
    }

    #[test]
    fn stacking_order_survives_overlapping_rectangles() {
        // Bottom window is the big one; a small window sits over its corner.
        let attributes = [normal(1, 0, 0, 200, 200), normal(2, 10, 10, 20, 20)];
        let desktop = desktop_windows(&attributes, &[], Rect::new(0, 0, 400, 400));
        assert_eq!(desktop.hit_test(15, 15).unwrap().id, 2);
        assert_eq!(desktop.hit_test(150, 150).unwrap().id, 1);
    }

    #[test]
    fn a_point_outside_every_window_selects_nothing() {
        let attributes = [normal(1, 0, 0, 50, 50)];
        let desktop = desktop_windows(&attributes, &[], Rect::new(0, 0, 200, 200));
        assert!(desktop.hit_test(100, 100).is_none());
        // The desktop rectangle is reported separately rather than as a hit.
        assert_eq!(desktop.desktop, Rect::new(0, 0, 200, 200));
    }

    #[test]
    fn containment_is_half_open_so_edges_do_not_overlap() {
        let attributes = [normal(1, 0, 0, 50, 50), normal(2, 50, 0, 50, 50)];
        let desktop = desktop_windows(&attributes, &[], Rect::new(0, 0, 200, 200));
        assert_eq!(desktop.hit_test(0, 0).unwrap().id, 1);
        // The shared edge belongs to the second window only.
        assert_eq!(desktop.hit_test(50, 0).unwrap().id, 2);
        assert_eq!(desktop.hit_test(49, 0).unwrap().id, 1);
    }

    #[test]
    fn excluded_windows_are_dropped_without_reordering_the_rest() {
        let attributes = [
            normal(1, 0, 0, 100, 100),
            normal(2, 0, 0, 100, 100),
            normal(3, 0, 0, 100, 100),
        ];
        let desktop = desktop_windows(&attributes, &[3], Rect::new(0, 0, 200, 200));
        assert_eq!(desktop.windows.len(), 2);
        assert_eq!(desktop.hit_test(50, 50).unwrap().id, 2);
    }

    #[test]
    fn only_plain_application_windows_are_selectable() {
        let hidden = WindowAttributes {
            viewable: false,
            ..normal(1, 0, 0, 100, 100)
        };
        let unmanaged = WindowAttributes {
            override_redirect: true,
            ..normal(2, 0, 0, 100, 100)
        };
        let pixel_less = WindowAttributes {
            has_pixels: false,
            ..normal(3, 0, 0, 100, 100)
        };
        let empty = normal(4, 0, 0, 0, 0);
        let backdrop = WindowAttributes {
            kind: WindowKind::Desktop,
            ..normal(5, 0, 0, 100, 100)
        };
        let panel = WindowAttributes {
            kind: WindowKind::Dock,
            ..normal(6, 0, 0, 100, 100)
        };
        let menu = WindowAttributes {
            kind: WindowKind::Transient,
            ..normal(7, 0, 0, 100, 100)
        };
        let attributes = [hidden, unmanaged, pixel_less, empty, backdrop, panel, menu];
        for attributes in &attributes {
            assert!(!selectable(attributes), "{attributes:?}");
        }
        assert!(
            desktop_windows(&attributes, &[], Rect::new(0, 0, 200, 200))
                .hit_test(50, 50)
                .is_none()
        );
    }

    #[test]
    fn only_a_visible_application_window_is_paintable() {
        assert!(is_paintable(
            WindowKind::Normal,
            true,
            Rect::new(0, 0, 10, 10)
        ));
        assert!(!is_paintable(
            WindowKind::Desktop,
            true,
            Rect::new(0, 0, 10, 10)
        ));
        assert!(!is_paintable(
            WindowKind::Dock,
            true,
            Rect::new(0, 0, 10, 10)
        ));
        assert!(!is_paintable(
            WindowKind::Transient,
            true,
            Rect::new(0, 0, 10, 10)
        ));
        assert!(!is_paintable(
            WindowKind::Normal,
            false,
            Rect::new(0, 0, 10, 10)
        ));
        assert!(!is_paintable(
            WindowKind::Normal,
            true,
            Rect::new(0, 0, 0, 10)
        ));
    }

    #[test]
    fn classification_prefers_desktop_then_dock_then_transient() {
        assert_eq!(classify(&[], &ROLES), WindowKind::Normal);
        assert_eq!(classify(&[999], &ROLES), WindowKind::Normal);
        assert_eq!(classify(&[300], &ROLES), WindowKind::Desktop);
        assert_eq!(classify(&[301], &ROLES), WindowKind::Dock);
        assert_eq!(classify(&[302], &ROLES), WindowKind::Transient);
        // A window advertising several types takes the first recognized one.
        assert_eq!(classify(&[999, 301, 300], &ROLES), WindowKind::Dock);
        assert_eq!(classify(&[300, 301], &ROLES), WindowKind::Desktop);
    }

    #[test]
    fn the_roles_decide_which_atom_means_what() {
        // Atom identifiers are per-connection, so the same numbers must be
        // read through whatever roles the connection assigned them.
        let shifted = WindowTypeRoles {
            desktop: 7,
            dock: 8,
            transient: &[9],
        };
        assert_eq!(classify(&[300], &shifted), WindowKind::Normal);
        assert_eq!(classify(&[7], &shifted), WindowKind::Desktop);
    }

    #[test]
    fn hit_testing_does_not_overflow_on_extreme_coordinates() {
        // A rectangle spanning the coordinate space saturates instead of
        // wrapping, and containment stays half-open at the saturated edge.
        let attributes = [WindowAttributes {
            bounds: Rect::new(i32::MIN, i32::MIN, u32::MAX, u32::MAX),
            ..normal(1, 0, 0, 1, 1)
        }];
        let desktop = desktop_windows(&attributes, &[], Rect::new(0, 0, 1, 1));
        assert!(desktop.hit_test(i32::MIN, i32::MIN).is_some());
        assert!(desktop.hit_test(i32::MAX - 1, i32::MAX - 1).is_some());
        // The exclusive right edge is exactly the saturated maximum.
        assert!(desktop.hit_test(i32::MAX, i32::MAX).is_none());
    }

    #[test]
    fn an_empty_desktop_reports_no_hit() {
        assert!(DesktopWindows::default().hit_test(0, 0).is_none());
    }
}
