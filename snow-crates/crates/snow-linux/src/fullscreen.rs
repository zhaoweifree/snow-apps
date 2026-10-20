//! Deciding whether the focused window is full screen.
//!
//! The answer is used to pause global shortcuts and to get the floating toolbar
//! out of the way while a video, a game or a presentation owns the display. It
//! is a heuristic on purpose: asking the window manager is cheaper and safer
//! than trusting any single flag, so the decision is "the focused window's frame
//! rectangle equals a monitor rectangle".
//!
//! Two things are worth stating up front, because both are silent failures:
//!
//! * **A desktop or panel window covers a monitor exactly.** An empty desktop
//!   is therefore indistinguishable from a full-screen application by geometry
//!   alone, and treating it as one would leave shortcuts disabled on a bare
//!   desktop with nothing on screen to explain why. Those window kinds are
//!   excluded before geometry is even looked at.
//! * **The comparison is against a single monitor, not the whole desktop.** A
//!   window spanning two heads is a maximised window, not a full-screen one,
//!   and it matches the Windows backend's rule as well.
//!
//! `_NET_WM_STATE_FULLSCREEN` is deliberately not required. Some full-screen
//! applications -- games under Wine, video players that size themselves to the
//! output -- never set it, while a window manager that honours it also resizes
//! the window to the monitor, which is exactly what the geometry check sees.

use crate::geometry::Rect;
use crate::window::WindowKind;

/// How far a frame edge may sit from the monitor edge and still count.
///
/// One pixel: window managers round fractional scaling and border widths, and
/// the Windows backend allows the same slack against its own monitor rectangle.
pub const EDGE_TOLERANCE: i32 = 1;

/// The focused window as far as full-screen detection cares.
///
/// Only the fields the decision needs are carried, so the rule can be tested
/// with a table of windows without an X server.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct FocusedWindow {
    pub kind: WindowKind,
    /// `IsViewable` on the window's top-level: mapped, with every ancestor
    /// mapped too. An iconified window keeps its geometry while being hidden.
    pub viewable: bool,
    /// Frame rectangle in root coordinates -- decorations included, because the
    /// decorations are part of what covers the monitor.
    pub frame: Rect,
}

/// Whether a window could be the full-screen application.
///
/// Rejects the kinds that cover a monitor without being an application: the
/// desktop backdrop, panels and docks, and transient surfaces such as menus or
/// notification popups.
pub fn is_fullscreen_candidate(window: &FocusedWindow) -> bool {
    window.kind == WindowKind::Normal && window.viewable && !window.frame.is_empty()
}

/// Whether `window` covers `display` within [`EDGE_TOLERANCE`] on every edge.
pub fn covers_display(window: Rect, display: Rect) -> bool {
    if window.is_empty() || display.is_empty() {
        return false;
    }
    // `abs_diff` rather than a subtraction: two edges at opposite ends of the
    // coordinate space are further apart than an `i32` can hold, and the
    // wrapping difference would land inside the tolerance.
    let close = |lhs: i32, rhs: i32| lhs.abs_diff(rhs) <= EDGE_TOLERANCE as u32;
    close(window.x, display.x)
        && close(window.y, display.y)
        && close(window.right(), display.right())
        && close(window.bottom(), display.bottom())
}

/// Whether the focused window covers any of `displays`.
///
/// `focused` is `None` when nothing has focus -- keyboard focus on the root
/// window, or an active-window property naming a window that has since gone.
/// An empty `displays` means the geometry is unknown, which must not be read as
/// "covers everything".
pub fn focused_is_fullscreen(focused: Option<&FocusedWindow>, displays: &[Rect]) -> bool {
    let Some(window) = focused else {
        return false;
    };
    if !is_fullscreen_candidate(window) {
        return false;
    }
    displays
        .iter()
        .any(|display| covers_display(window.frame, *display))
}

/// Convert a rectangle expressed in Qt's logical coordinates into root pixels.
///
/// Qt's coordinates are a fraction of the server's when the session scales, and
/// the factor belongs to the screen the rectangle sits on -- which is why it is
/// passed in rather than derived here. A non-finite or non-positive scale means
/// the caller had no ratio to offer, and then the input is taken as native.
pub fn logical_to_native(x: i32, y: i32, width: u32, height: u32, scale: f64) -> Rect {
    if !scale.is_finite() || scale <= 0.0 {
        return Rect::new(x, y, width, height);
    }
    let scaled = |value: i32| (f64::from(value) * scale).round();
    let left = scaled(x);
    let top = scaled(y);
    // The edges are scaled rather than the width, so that a fractional origin
    // cannot leave the rectangle one pixel short of the monitor it describes.
    let right = scaled(x.saturating_add_unsigned(width));
    let bottom = scaled(y.saturating_add_unsigned(height));
    // Widened before subtracting: two saturated edges are further apart than an
    // `i32` can express.
    let span = |start: f64, end: f64| u32::try_from((end - start).round() as i64).unwrap_or(0);
    Rect::new(
        clamp_to_i32(left),
        clamp_to_i32(top),
        span(left, right),
        span(top, bottom),
    )
}

/// Round to the nearest `i32`, saturating rather than wrapping.
fn clamp_to_i32(value: f64) -> i32 {
    value.clamp(f64::from(i32::MIN), f64::from(i32::MAX)) as i32
}

#[cfg(test)]
mod tests {
    use super::*;

    fn window(kind: WindowKind, x: i32, y: i32, width: u32, height: u32) -> FocusedWindow {
        FocusedWindow {
            kind,
            viewable: true,
            frame: Rect::new(x, y, width, height),
        }
    }

    const SCREEN: Rect = Rect::new(0, 0, 1920, 1080);

    #[test]
    fn a_window_exactly_on_the_monitor_is_fullscreen() {
        let focused = window(WindowKind::Normal, 0, 0, 1920, 1080);
        assert!(focused_is_fullscreen(Some(&focused), &[SCREEN]));
    }

    #[test]
    fn an_empty_desktop_is_not_fullscreen() {
        // The regression this rule exists for: the desktop backdrop covers the
        // monitor exactly, so geometry alone would disable every shortcut on a
        // bare desktop with nothing on screen to explain why.
        let desktop = window(WindowKind::Desktop, 0, 0, 1920, 1080);
        assert!(!focused_is_fullscreen(Some(&desktop), &[SCREEN]));
        let dock = window(WindowKind::Dock, 0, 0, 1920, 1080);
        assert!(!focused_is_fullscreen(Some(&dock), &[SCREEN]));
        let menu = window(WindowKind::Transient, 0, 0, 1920, 1080);
        assert!(!focused_is_fullscreen(Some(&menu), &[SCREEN]));
    }

    #[test]
    fn an_iconified_or_hidden_window_keeps_its_geometry_but_does_not_count() {
        let mut focused = window(WindowKind::Normal, 0, 0, 1920, 1080);
        focused.viewable = false;
        assert!(!focused_is_fullscreen(Some(&focused), &[SCREEN]));
    }

    #[test]
    fn nothing_focused_is_not_fullscreen() {
        assert!(!focused_is_fullscreen(None, &[SCREEN]));
    }

    #[test]
    fn an_unknown_display_set_is_not_fullscreen() {
        // Reporting "full screen" for an unknown layout would hide the toolbar
        // and pause every shortcut on evidence that does not exist.
        let focused = window(WindowKind::Normal, 0, 0, 1920, 1080);
        assert!(!focused_is_fullscreen(Some(&focused), &[]));
    }

    #[test]
    fn a_maximised_window_leaving_a_panel_visible_is_not_fullscreen() {
        let focused = window(WindowKind::Normal, 0, 0, 1920, 1040);
        assert!(!focused_is_fullscreen(Some(&focused), &[SCREEN]));
    }

    #[test]
    fn a_window_larger_than_the_monitor_is_not_fullscreen() {
        // A decorated window whose client happens to be monitor-sized: the
        // title bar pushes the frame past the bottom edge.
        let focused = window(WindowKind::Normal, 0, 0, 1920, 1082);
        assert!(!focused_is_fullscreen(Some(&focused), &[SCREEN]));
    }

    #[test]
    fn edges_may_be_off_by_one_pixel() {
        for (x, y, width, height) in [
            (1, 0, 1920, 1080),
            (0, 1, 1920, 1080),
            (0, 0, 1919, 1080),
            (0, 0, 1920, 1079),
            (-1, -1, 1922, 1082),
        ] {
            let focused = window(WindowKind::Normal, x, y, width, height);
            assert!(
                focused_is_fullscreen(Some(&focused), &[SCREEN]),
                "{x},{y} {width}x{height}"
            );
        }
        let off_by_two = window(WindowKind::Normal, 0, 0, 1918, 1080);
        assert!(!focused_is_fullscreen(Some(&off_by_two), &[SCREEN]));
    }

    #[test]
    fn each_monitor_is_matched_on_its_own() {
        let left = Rect::new(0, 0, 1920, 1080);
        let right = Rect::new(1920, 0, 2560, 1440);
        let fullscreen_right = window(WindowKind::Normal, 1920, 0, 2560, 1440);
        assert!(focused_is_fullscreen(
            Some(&fullscreen_right),
            &[left, right]
        ));

        // Spanning both heads is a maximised window, and it matches neither.
        let spanning = window(WindowKind::Normal, 0, 0, 4480, 1440);
        assert!(!focused_is_fullscreen(Some(&spanning), &[left, right]));
    }

    #[test]
    fn a_monitor_left_of_the_origin_is_matched_in_root_coordinates() {
        let left = Rect::new(-1920, 0, 1920, 1080);
        let focused = window(WindowKind::Normal, -1920, 0, 1920, 1080);
        assert!(focused_is_fullscreen(Some(&focused), &[left]));
        // The same geometry on a monitor shifted right must not match.
        assert!(!focused_is_fullscreen(
            Some(&focused),
            &[Rect::new(0, 0, 1920, 1080)]
        ));
    }

    #[test]
    fn degenerate_rectangles_never_match() {
        let empty = Rect::new(0, 0, 0, 0);
        assert!(!covers_display(empty, empty));
        assert!(!covers_display(Rect::new(0, 0, 1920, 1080), empty));
        let focused = window(WindowKind::Normal, 0, 0, 0, 0);
        assert!(!focused_is_fullscreen(Some(&focused), &[SCREEN]));
    }

    #[test]
    fn extreme_edges_do_not_wrap_into_a_match() {
        // `right()` saturates, so a rectangle at the far end of the coordinate
        // space cannot alias onto a monitor near the origin.
        let huge = window(WindowKind::Normal, i32::MIN, i32::MIN, u32::MAX, u32::MAX);
        assert!(!focused_is_fullscreen(Some(&huge), &[SCREEN]));
    }

    #[test]
    fn logical_coordinates_become_native_pixels() {
        // A 2x session: Qt reports half the pixels the server uses.
        assert_eq!(
            logical_to_native(0, 0, 960, 540, 2.0),
            Rect::new(0, 0, 1920, 1080)
        );
        // Unscaled sessions pass through untouched.
        assert_eq!(
            logical_to_native(0, 0, 1920, 1080, 1.0),
            Rect::new(0, 0, 1920, 1080)
        );
    }

    #[test]
    fn a_fractional_scale_keeps_the_rectangle_on_the_monitor() {
        // 1.25 is the common fractional ratio: 1536 logical pixels are 1920
        // native ones, exactly, and the edges are scaled rather than the width
        // so that a fractional origin cannot shorten the result.
        assert_eq!(
            logical_to_native(0, 0, 1536, 864, 1.25),
            Rect::new(0, 0, 1920, 1080)
        );
        assert_eq!(
            logical_to_native(-1536, 0, 1536, 864, 1.25),
            Rect::new(-1920, 0, 1920, 1080)
        );
    }

    #[test]
    fn a_missing_scale_is_treated_as_native() {
        for scale in [0.0, -1.0, f64::NAN, f64::INFINITY] {
            assert_eq!(
                logical_to_native(10, 20, 30, 40, scale),
                Rect::new(10, 20, 30, 40),
                "{scale}"
            );
        }
    }

    #[test]
    fn a_scaled_rectangle_can_be_compared_with_a_monitor() {
        let focused = window(WindowKind::Normal, 0, 0, 1920, 1080);
        let display = logical_to_native(0, 0, 1536, 864, 1.25);
        assert!(focused_is_fullscreen(Some(&focused), &[display]));
    }
}
