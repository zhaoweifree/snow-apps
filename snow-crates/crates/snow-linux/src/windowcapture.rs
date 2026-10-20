//! Turning "the window the user is looking at" into pixels.
//!
//! The X11 backend has exactly one way to read pixels: `GetImage` against the
//! root window. Reading a window's own drawable would need a Composite redirect
//! so that the server keeps the window's contents while it is obscured, and
//! without that a plain window read returns whatever the server happens to have
//! -- on many drivers the frame that was on screen when the window was last
//! painted. So a "window capture" on X11 is a **crop of the root capture** at the
//! window's frame rectangle, which is also what the other X11 screenshot tools
//! do. The visible pixels are the honest answer: an obscured window is captured
//! obscured.
//!
//! That makes the whole feature geometry, and geometry is the part that can be
//! tested without a server:
//!
//! * [`capture_target`] decides whether the focused window is something the user
//!   can name as a target at all, and carries its frame.
//! * [`crop_plan`] turns a frame plus the rectangles of the captured displays
//!   into the slices the caller has to copy, including the case of a window
//!   straddling two heads -- the case where reading a single display quietly
//!   returns half a window.
//!
//! Both inputs are root-window pixel coordinates, the same space
//! [`crate::window::DesktopWindows`] reports and the same space the capture
//! frames are addressed in.

use crate::fullscreen::FocusedWindow;
use crate::geometry::Rect;

/// A window that can be captured on its own.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct WindowTarget {
    /// The X11 window id, kept for identity -- history entries name the window
    /// they came from, and two captures of the same window should compare equal.
    pub id: u64,
    /// Frame rectangle in root coordinates: decorations included, because the
    /// decorations are part of what the user calls "this window".
    pub frame: Rect,
}

/// The capturable window behind a focused one, or `None` when it is not one.
///
/// The rule is the same one that decides whether a window can be the full-screen
/// application, and deliberately so: both questions reject the exact same
/// surfaces. The desktop backdrop, panels and menus cover a monitor without
/// being an application, and a hidden window keeps its geometry while showing
/// nothing -- capturing any of those would hand the user a picture of the
/// desktop labelled as a window.
pub fn capture_target(id: u64, focused: &FocusedWindow) -> Option<WindowTarget> {
    crate::window::is_paintable(focused.kind, focused.viewable, focused.frame).then_some(
        WindowTarget {
            id,
            frame: focused.frame,
        },
    )
}

/// One display's share of a window capture.
///
/// `source` is in the display's own pixel coordinates, which is where the
/// captured image starts at zero. `destination_*` is in the window's frame
/// coordinates, i.e. where the slice belongs in the output image whose size is
/// the frame's.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CropSlice {
    /// Index into the display list the plan was built from.
    pub display_index: usize,
    pub source: Rect,
    pub destination_x: i32,
    pub destination_y: i32,
}

/// Which part of which display holds which part of the window.
///
/// Returns one slice per display that overlaps the frame, in the order the
/// displays were given. An empty result means the window is entirely outside
/// every captured display -- minimised to a corner, or on a head the capture
/// layer does not cover -- and the caller has no pixels to assemble.
///
/// Displays that mirror each other produce two slices writing the same pixels to
/// the same place, which is harmless: the second copy is identical.
pub fn crop_plan(frame: Rect, displays: &[Rect]) -> Vec<CropSlice> {
    let mut plan = Vec::new();
    if frame.is_empty() {
        return plan;
    }
    for (display_index, display) in displays.iter().enumerate() {
        if display.is_empty() {
            continue;
        }
        let Some(overlap) = frame.intersect(display) else {
            continue;
        };
        plan.push(CropSlice {
            display_index,
            // The overlap is contained in the display, so the subtraction
            // cannot leave the rectangle; `saturating_sub` only keeps absurd
            // inputs from wrapping.
            source: Rect::new(
                overlap.x.saturating_sub(display.x),
                overlap.y.saturating_sub(display.y),
                overlap.width,
                overlap.height,
            ),
            destination_x: overlap.x.saturating_sub(frame.x),
            destination_y: overlap.y.saturating_sub(frame.y),
        });
    }
    plan
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::window::WindowKind;

    const SCREEN: Rect = Rect::new(0, 0, 1920, 1080);

    fn focused(kind: WindowKind, x: i32, y: i32, width: u32, height: u32) -> FocusedWindow {
        FocusedWindow {
            kind,
            viewable: true,
            frame: Rect::new(x, y, width, height),
        }
    }

    #[test]
    fn a_normal_visible_window_is_a_target() {
        let target = capture_target(7, &focused(WindowKind::Normal, 10, 20, 300, 200)).unwrap();
        assert_eq!(target.id, 7);
        assert_eq!(target.frame, Rect::new(10, 20, 300, 200));
    }

    #[test]
    fn surfaces_that_are_not_an_application_are_refused() {
        for kind in [WindowKind::Desktop, WindowKind::Dock, WindowKind::Transient] {
            assert!(
                capture_target(1, &focused(kind, 0, 0, 1920, 1080)).is_none(),
                "{kind:?}"
            );
        }
    }

    #[test]
    fn a_hidden_window_is_not_a_target() {
        let mut hidden = focused(WindowKind::Normal, 0, 0, 800, 600);
        hidden.viewable = false;
        assert!(capture_target(1, &hidden).is_none());
    }

    #[test]
    fn an_empty_focused_window_is_not_a_target() {
        assert!(capture_target(1, &focused(WindowKind::Normal, 0, 0, 0, 0)).is_none());
    }

    #[test]
    fn a_window_inside_one_display_copies_the_whole_picture() {
        let plan = crop_plan(Rect::new(100, 50, 300, 200), &[SCREEN]);
        assert_eq!(
            plan,
            [CropSlice {
                display_index: 0,
                source: Rect::new(100, 50, 300, 200),
                destination_x: 0,
                destination_y: 0,
            }]
        );
    }

    #[test]
    fn a_window_on_a_secondary_head_is_located_inside_that_display() {
        let left = Rect::new(-1920, 0, 1920, 1080);
        let plan = crop_plan(Rect::new(-1800, 100, 400, 300), &[SCREEN, left]);
        assert_eq!(plan.len(), 1);
        assert_eq!(plan[0].display_index, 1);
        // 120 pixels in from the left head's own origin.
        assert_eq!(plan[0].source, Rect::new(120, 100, 400, 300));
        assert_eq!((plan[0].destination_x, plan[0].destination_y), (0, 0));
    }

    #[test]
    fn a_window_straddling_two_heads_is_split_in_order() {
        let right = Rect::new(1920, 0, 2560, 1440);
        let plan = crop_plan(Rect::new(1800, 100, 400, 300), &[SCREEN, right]);
        assert_eq!(plan.len(), 2);
        // The left head owns the first 120 columns of the window.
        assert_eq!(plan[0].source, Rect::new(1800, 100, 120, 300));
        assert_eq!((plan[0].destination_x, plan[0].destination_y), (0, 0));
        // The right head owns the remaining 280, which land after them.
        assert_eq!(plan[1].display_index, 1);
        assert_eq!(plan[1].source, Rect::new(0, 100, 280, 300));
        assert_eq!((plan[1].destination_x, plan[1].destination_y), (120, 0));
    }

    #[test]
    fn a_window_above_the_origin_is_split_vertically() {
        let upper = Rect::new(0, -1080, 1920, 1080);
        let plan = crop_plan(Rect::new(100, -100, 300, 300), &[SCREEN, upper]);
        assert_eq!(plan.len(), 2);
        // Listed in display order, not in the order they stack.
        assert_eq!(plan[0].display_index, 0);
        assert_eq!(plan[0].source, Rect::new(100, 0, 300, 200));
        assert_eq!(plan[0].destination_y, 100);
        assert_eq!(plan[1].display_index, 1);
        assert_eq!(plan[1].source, Rect::new(100, 980, 300, 100));
        assert_eq!(plan[1].destination_y, 0);
    }

    #[test]
    fn a_window_outside_every_display_has_no_slices() {
        let plan = crop_plan(Rect::new(5000, 5000, 100, 100), &[SCREEN]);
        assert!(plan.is_empty());
    }

    #[test]
    fn touching_an_edge_without_overlapping_is_not_a_slice() {
        // Half-open rectangles: the window starting exactly at the right edge
        // of the only display shares no pixel with it.
        let plan = crop_plan(Rect::new(1920, 0, 100, 100), &[SCREEN]);
        assert!(plan.is_empty());
    }

    #[test]
    fn an_empty_frame_has_no_slices() {
        assert!(crop_plan(Rect::new(0, 0, 0, 0), &[SCREEN]).is_empty());
        assert!(crop_plan(Rect::new(0, 0, 100, 0), &[SCREEN]).is_empty());
    }

    #[test]
    fn degenerate_displays_are_skipped_without_shifting_the_indexes() {
        let plan = crop_plan(
            Rect::new(10, 10, 50, 50),
            &[Rect::new(0, 0, 0, 0), SCREEN, Rect::new(0, 0, 0, 1080)],
        );
        assert_eq!(plan.len(), 1);
        assert_eq!(plan[0].display_index, 1);
    }

    #[test]
    fn an_empty_display_list_has_no_slices() {
        assert!(crop_plan(Rect::new(0, 0, 100, 100), &[]).is_empty());
    }

    #[test]
    fn extreme_edges_do_not_wrap_into_a_slice() {
        // A frame at the far end of the coordinate space must not alias onto a
        // display near the origin through a wrapping subtraction.
        let frame = Rect::new(i32::MAX - 10, i32::MAX - 10, 100, 100);
        assert!(crop_plan(frame, &[SCREEN]).is_empty());
        let plan = crop_plan(frame, &[Rect::new(0, 0, u32::MAX, u32::MAX)]);
        assert_eq!(plan.len(), 1);
        assert_eq!(plan[0].destination_x, 0);
        assert_eq!(plan[0].destination_y, 0);
    }
}
