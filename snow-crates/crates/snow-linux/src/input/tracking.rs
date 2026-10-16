//! Pure state machines behind the X11 input observer.
//!
//! The X server hands the observer raw, unfiltered events: every button on every
//! device, in root-window coordinates. The recording overlay, however, is written
//! against the *filtered* view a Windows low-level hook produces — clicks are
//! region-relative, a press outside the recorded region is not a click at all,
//! and a movement that leaves the region breaks the pointer trail even when the
//! surrounding movements are coalesced away.
//!
//! Reproducing that view is logic, not I/O, so it lives here where it compiles
//! and is tested on any host.

/// Mouse button as the overlay names it.
///
/// Deliberately the same five values `ObservedMouseButton` has: `Button4` and
/// `Button5` are the *side* buttons (`XButton1`/`XButton2` on Windows), not the
/// wheel, which is why X11's button 4/5 do not map here.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PointerButton {
    Left,
    Right,
    Middle,
    Button4,
    Button5,
}

impl PointerButton {
    /// Index into the five-slot held table.
    fn slot(self) -> usize {
        match self {
            Self::Left => 0,
            Self::Right => 1,
            Self::Middle => 2,
            Self::Button4 => 3,
            Self::Button5 => 4,
        }
    }
}

/// Translate an X11 button number.
///
/// `1/2/3` are left/middle/right and `8/9` the side buttons. `4/5/6/7` are
/// vertical and horizontal wheel detents: they are not clicks, and treating them
/// as such would paint a keycap every time the user scrolls.
pub fn pointer_button(button: u8) -> Option<PointerButton> {
    Some(match button {
        1 => PointerButton::Left,
        2 => PointerButton::Middle,
        3 => PointerButton::Right,
        8 => PointerButton::Button4,
        9 => PointerButton::Button5,
        _ => return None,
    })
}

/// A recorded region, in root-window coordinates.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Region {
    pub x: i32,
    pub y: i32,
    pub width: u32,
    pub height: u32,
}

impl Region {
    pub fn new(x: i32, y: i32, width: u32, height: u32) -> Self {
        Self {
            x,
            y,
            width,
            height,
        }
    }

    /// Half-open containment, matching the Windows hook's comparison so a pixel
    /// on the right or bottom edge belongs to the next region, never to both.
    pub fn contains(&self, x: i32, y: i32) -> bool {
        i64::from(x) >= i64::from(self.x)
            && i64::from(x) < i64::from(self.x) + i64::from(self.width)
            && i64::from(y) >= i64::from(self.y)
            && i64::from(y) < i64::from(self.y) + i64::from(self.height)
    }

    /// Position relative to the region.
    ///
    /// A point outside the region keeps its signed offset rather than being
    /// clamped to the edge: a release that landed past the border is still drawn
    /// where it happened, which is what the Windows hook reports too. Only the
    /// arithmetic saturates, at the extremes of the coordinate space.
    pub fn relative(&self, x: i32, y: i32) -> (i32, i32) {
        (x.saturating_sub(self.x), y.saturating_sub(self.y))
    }
}

/// What the consumer should know about one pointer movement.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct MotionUpdate {
    /// `None` while the pointer is outside the recorded region.
    pub position: Option<(i32, i32)>,
    /// Increments on every exit from the region, so coalescing cannot hide a
    /// break in the trail.
    pub continuity: u64,
    /// `true` only when this movement left the region.
    pub exited: bool,
}

/// Tracks whether the pointer is inside the recorded region.
#[derive(Clone, Copy, Debug)]
pub struct MotionTracker {
    region: Region,
    was_inside: bool,
    continuity: u64,
}

impl MotionTracker {
    /// Start *outside* the region.
    ///
    /// The Windows hook starts the same way and immediately feeds the current
    /// pointer position through the tracker, so the first real movement reports
    /// an entry rather than assuming the pointer was already inside.
    pub fn new(region: Region) -> Self {
        Self {
            region,
            was_inside: false,
            continuity: 0,
        }
    }

    pub fn continuity(&self) -> u64 {
        self.continuity
    }

    pub fn update(&mut self, x: i32, y: i32) -> MotionUpdate {
        let inside = self.region.contains(x, y);
        let exited = self.was_inside && !inside;
        if exited {
            self.continuity = self.continuity.wrapping_add(1);
        }
        self.was_inside = inside;
        MotionUpdate {
            position: inside.then(|| self.region.relative(x, y)),
            continuity: self.continuity,
            exited,
        }
    }
}

/// Filters button events down to the clicks a recording should contain.
#[derive(Clone, Copy, Debug, Default)]
pub struct ClickTracker {
    held: [bool; 5],
}

impl ClickTracker {
    /// Decide whether a button transition is worth reporting.
    ///
    /// A press counts only inside the region; its release counts even outside,
    /// because the overlay is already drawing it down. A release for a button
    /// this observer never reported a press for is dropped: reporting it would
    /// draw a key-up with no key-down. The held table therefore only records
    /// transitions that were accepted — a press outside the region leaves no
    /// trace, exactly like the Windows hook it mirrors.
    pub fn accept(&mut self, button: PointerButton, down: bool, inside: bool) -> bool {
        let slot = button.slot();
        let accepted = if down { inside } else { self.held[slot] };
        if accepted {
            self.held[slot] = down;
        }
        accepted
    }

    /// Forget every held button.
    ///
    /// Called when the click channel overflows: the next event carries a
    /// generation change, so the consumer ignores everything up to it, and a
    /// release the consumer never saw the press of must not be reported.
    pub fn reset(&mut self) {
        self.held = [false; 5];
    }
}

/// Modifier state carried by every click, in the overlay's fixed order.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Modifiers {
    pub control: bool,
    pub alt: bool,
    pub shift: bool,
    pub super_key: bool,
}

impl Modifiers {
    pub fn as_array(self) -> [bool; 4] {
        [self.control, self.alt, self.shift, self.super_key]
    }
}

/// Modifier state implied by a pressed-key table.
///
/// Used for key events, which carry the table and need no server round-trip. A
/// modifier counts when *either* side is held, which is the same rule the
/// overlay applies when it renders the modifier chips.
///
/// AltGr is deliberately not Alt: on X11 it is a key of its own (`0xa5`), not
/// the `Ctrl`+`Alt` pair Windows synthesises for it.
pub fn modifiers_from_pressed(pressed: &[u8; 256]) -> Modifiers {
    Modifiers {
        control: pressed[0xa2] != 0 || pressed[0xa3] != 0,
        alt: pressed[0xa4] != 0,
        shift: pressed[0xa0] != 0 || pressed[0xa1] != 0,
        super_key: pressed[0x5b] != 0 || pressed[0x5c] != 0,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn region() -> Region {
        Region::new(100, 200, 300, 150)
    }

    #[test]
    fn region_bounds_are_half_open() {
        let region = region();
        assert!(region.contains(100, 200));
        assert!(region.contains(399, 349));
        assert!(!region.contains(400, 349));
        assert!(!region.contains(399, 350));
        assert!(!region.contains(99, 200));
    }

    #[test]
    fn region_arithmetic_survives_extreme_coordinates() {
        // Multi-head desktops legitimately place monitors at large negative
        // offsets, so containment must not overflow.
        let head = Region::new(-3000, -1080, 1920, 1080);
        assert!(head.contains(-3000, -1080));
        assert!(!head.contains(-3001, -1080));

        // The widest region the type can express still answers correctly at the
        // far end of the coordinate space: the bounds are widened before they
        // are added, so `i32::MIN + u32::MAX` cannot wrap into a false negative.
        let wide = Region::new(i32::MIN + 10, 0, u32::MAX, 1);
        assert!(wide.contains(i32::MIN + 10, 0));
        assert!(wide.contains(i32::MAX, 0));
        assert!(!wide.contains(i32::MIN + 9, 0));

        // A region at the far positive end is addressable too, and the half-open
        // upper bound still holds there: `i32::MAX` itself is outside.
        let edge = Region::new(i32::MAX - 5, i32::MAX - 5, 5, 5);
        assert!(edge.contains(i32::MAX - 1, i32::MAX - 1));
        assert!(!edge.contains(i32::MAX, i32::MAX));
        assert!(!edge.contains(i32::MAX, i32::MAX - 6));
    }

    #[test]
    fn relative_positions_keep_offsets_outside_the_region() {
        let region = region();
        assert_eq!(region.relative(100, 200), (0, 0));
        assert_eq!(region.relative(140, 260), (40, 60));
        // A release outside the region is still placed where it happened.
        assert_eq!(region.relative(0, 0), (-100, -200));
        // The arithmetic saturates instead of overflowing at the extremes.
        assert_eq!(region.relative(i32::MIN, i32::MIN), (i32::MIN, i32::MIN));
    }

    #[test]
    fn movement_reports_region_relative_positions_only_inside() {
        let mut tracker = MotionTracker::new(region());
        let inside = tracker.update(150, 250);
        assert_eq!(inside.position, Some((50, 50)));
        assert!(!inside.exited);

        let outside = tracker.update(10, 10);
        assert_eq!(outside.position, None);
        assert!(outside.exited);
        assert_eq!(outside.continuity, 1);
    }

    #[test]
    fn continuity_increments_once_per_exit() {
        let mut tracker = MotionTracker::new(region());
        // Starting outside is not an exit: the tracker begins there.
        assert!(!tracker.update(0, 0).exited);
        assert_eq!(tracker.continuity(), 0);
        tracker.update(150, 250);
        tracker.update(0, 0);
        assert_eq!(tracker.continuity(), 1);
        // Moving further outside stays in the same continuous absence.
        tracker.update(0, 1);
        assert_eq!(tracker.continuity(), 1);
        tracker.update(150, 250);
        tracker.update(0, 0);
        assert_eq!(tracker.continuity(), 2);
    }

    #[test]
    fn presses_outside_the_region_are_not_clicks() {
        let mut tracker = ClickTracker::default();
        assert!(!tracker.accept(PointerButton::Left, true, false));
        // Its release is dropped even inside the region: the consumer never saw
        // a press, and a rejected press must not leave a held button behind.
        assert!(!tracker.accept(PointerButton::Left, false, true));
        // A press that does belong to the recording still works.
        assert!(tracker.accept(PointerButton::Left, true, true));
    }

    #[test]
    fn releases_follow_a_press_outside_the_region() {
        let mut tracker = ClickTracker::default();
        assert!(tracker.accept(PointerButton::Left, true, true));
        // Dragging out of the region and releasing still completes the click.
        assert!(tracker.accept(PointerButton::Left, false, false));
    }

    #[test]
    fn each_button_is_tracked_independently() {
        let mut tracker = ClickTracker::default();
        assert!(tracker.accept(PointerButton::Right, true, true));
        // A left release was never pressed, so it is not this right press.
        assert!(!tracker.accept(PointerButton::Left, false, true));
        assert!(tracker.accept(PointerButton::Right, false, false));
        assert!(!tracker.accept(PointerButton::Right, false, false));
    }

    #[test]
    fn reset_drops_the_held_set_after_an_overflow() {
        let mut tracker = ClickTracker::default();
        assert!(tracker.accept(PointerButton::Middle, true, true));
        tracker.reset();
        assert!(!tracker.accept(PointerButton::Middle, false, false));
        // A fresh press is still accepted.
        assert!(tracker.accept(PointerButton::Middle, true, true));
    }

    #[test]
    fn wheel_detents_are_not_clicks() {
        for wheel in 4..=7u8 {
            assert_eq!(pointer_button(wheel), None);
        }
        assert_eq!(pointer_button(1), Some(PointerButton::Left));
        assert_eq!(pointer_button(2), Some(PointerButton::Middle));
        assert_eq!(pointer_button(3), Some(PointerButton::Right));
        // 8/9 are the side buttons, which the overlay names `Button4`/`Button5`.
        assert_eq!(pointer_button(8), Some(PointerButton::Button4));
        assert_eq!(pointer_button(9), Some(PointerButton::Button5));
        assert_eq!(pointer_button(0), None);
    }

    #[test]
    fn modifiers_keep_the_overlay_order() {
        let modifiers = Modifiers {
            control: true,
            alt: false,
            shift: true,
            super_key: false,
        };
        assert_eq!(modifiers.as_array(), [true, false, true, false]);
    }

    #[test]
    fn pressed_table_names_the_same_modifiers_the_overlay_draws() {
        let mut pressed = [0u8; 256];
        assert_eq!(modifiers_from_pressed(&pressed), Modifiers::default());

        pressed[0xa3] = 0x80; // right Control
        pressed[0xa1] = 0x80; // right Shift
        pressed[0x5c] = 0x80; // right Super
        let modifiers = modifiers_from_pressed(&pressed);
        assert_eq!(modifiers.as_array(), [true, false, true, true]);
    }

    #[test]
    fn altgr_is_not_alt() {
        // X11 has a dedicated AltGr key. Reading it as Alt would paint an `Alt`
        // chip that the user never held.
        let mut pressed = [0u8; 256];
        pressed[0xa5] = 0x80;
        let modifiers = modifiers_from_pressed(&pressed);
        assert!(!modifiers.alt);
        assert_eq!(modifiers.as_array(), [false, false, false, false]);
    }
}
