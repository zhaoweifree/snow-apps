//! Pointer coordinates, and the rules around them that need no server.
//!
//! Reading and moving the pointer are two core X11 requests (`QueryPointer` and
//! `WarpPointer`), so the server dialogue itself has nothing interesting in it.
//! What does have rules is the coordinate space: root coordinates are signed
//! 16-bit, and the answer to "where is the pointer" is relative to whichever
//! root holds it.

/// A pointer position in the root window's coordinate space.
///
/// X11 root coordinates are signed 16-bit. `WarpPointer` cannot express anything
/// outside that range, and a cast that wrapped would put the cursor in the
/// opposite corner instead of refusing the move, so the conversion is checked
/// and lives here where it can be tested without a server.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct RootPosition {
    pub x: i16,
    pub y: i16,
}

impl RootPosition {
    /// `None` when the request lies outside the range the protocol can express.
    pub fn new(x: i32, y: i32) -> Option<Self> {
        Some(Self {
            x: i16::try_from(x).ok()?,
            y: i16::try_from(y).ok()?,
        })
    }
}

/// A `QueryPointer` answer, reduced to what this crate uses.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PointerSample {
    root_x: i16,
    root_y: i16,
    same_screen: bool,
}

impl PointerSample {
    pub fn new(root_x: i16, root_y: i16, same_screen: bool) -> Self {
        Self {
            root_x,
            root_y,
            same_screen,
        }
    }

    /// The position, or `None` when the pointer is not on the root this
    /// connection belongs to.
    ///
    /// `QueryPointer` reports coordinates relative to whichever root holds the
    /// pointer, and flags that with `same_screen == false`. This backend
    /// connects to one screen, so those coordinates would be a position on a
    /// different root read as one on this screen -- a cursor reported in the
    /// wrong place rather than a read that fails.
    pub fn position(&self) -> Option<RootPosition> {
        self.same_screen.then_some(RootPosition {
            x: self.root_x,
            y: self.root_y,
        })
    }

    /// Whether the pointer was on this connection's screen.
    pub fn same_screen(&self) -> bool {
        self.same_screen
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_protocol_range_is_accepted_at_its_edges() {
        for (x, y) in [
            (0, 0),
            (i32::from(i16::MIN), i32::from(i16::MIN)),
            (i32::from(i16::MAX), i32::from(i16::MAX)),
        ] {
            let position = RootPosition::new(x, y).expect("the edge of the range is expressible");
            assert_eq!((i32::from(position.x), i32::from(position.y)), (x, y));
        }
    }

    #[test]
    fn one_pixel_past_the_range_is_refused_rather_than_wrapped() {
        assert_eq!(RootPosition::new(32_768, 0), None);
        assert_eq!(RootPosition::new(0, 32_768), None);
        assert_eq!(RootPosition::new(-32_769, 0), None);
        assert_eq!(RootPosition::new(0, -32_769), None);
    }

    #[test]
    fn absurd_coordinates_are_refused_on_both_axes() {
        for (x, y) in [(i32::MAX, 0), (i32::MIN, 0), (0, i32::MAX), (0, i32::MIN)] {
            assert_eq!(RootPosition::new(x, y), None, "{x},{y}");
        }
    }

    #[test]
    fn a_pointer_on_another_root_has_no_position_here() {
        let sample = PointerSample::new(100, 200, false);
        assert!(!sample.same_screen());
        assert_eq!(sample.position(), None);
    }

    #[test]
    fn a_pointer_on_this_root_keeps_its_negative_origin() {
        // A head placed left of the primary one has a negative x; dropping the
        // sign would report it a screen's width to the right.
        let sample = PointerSample::new(-1920, -37, true);
        assert_eq!(sample.position(), Some(RootPosition { x: -1920, y: -37 }));
    }

    #[test]
    fn a_sample_round_trips_through_the_checked_constructor() {
        let sample = PointerSample::new(-8, 41, true);
        let position = sample.position().unwrap();
        assert_eq!(
            RootPosition::new(i32::from(position.x), i32::from(position.y)),
            Some(position)
        );
    }
}
