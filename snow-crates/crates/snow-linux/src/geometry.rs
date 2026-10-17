//! Root-window rectangles, in integer desktop coordinates.

/// An axis-aligned rectangle in desktop coordinates.
///
/// `x`/`y` may be negative: X11 positions secondary heads relative to the
/// primary one, so a head placed above or left of primary has a negative origin.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct Rect {
    pub x: i32,
    pub y: i32,
    pub width: u32,
    pub height: u32,
}

impl Rect {
    pub const fn new(x: i32, y: i32, width: u32, height: u32) -> Self {
        Self {
            x,
            y,
            width,
            height,
        }
    }

    /// Exclusive right edge, saturating instead of wrapping on absurd inputs.
    pub const fn right(&self) -> i32 {
        self.x.saturating_add_unsigned(self.width)
    }

    /// Exclusive bottom edge, saturating instead of wrapping on absurd inputs.
    pub const fn bottom(&self) -> i32 {
        self.y.saturating_add_unsigned(self.height)
    }

    /// Half-open containment, so adjacent monitors never both claim a pixel.
    pub const fn contains(&self, x: i32, y: i32) -> bool {
        x >= self.x && x < self.right() && y >= self.y && y < self.bottom()
    }

    /// Smallest rectangle covering both inputs.
    pub fn union(&self, other: &Self) -> Self {
        let left = self.x.min(other.x);
        let top = self.y.min(other.y);
        let right = self.right().max(other.right());
        let bottom = self.bottom().max(other.bottom());

        Self {
            x: left,
            y: top,
            width: u32::try_from(right.saturating_sub(left)).unwrap_or(0),
            height: u32::try_from(bottom.saturating_sub(top)).unwrap_or(0),
        }
    }

    pub const fn is_empty(&self) -> bool {
        self.width == 0 || self.height == 0
    }

    /// Intersection with `other`, or `None` when they do not overlap.
    ///
    /// Used to bring a requested capture rectangle inside the root window: the
    /// X protocol rejects a `GetImage` whose rectangle is not fully contained
    /// in the drawable, and clamping is what a screenshot caller wants anyway.
    pub fn intersect(&self, other: &Self) -> Option<Self> {
        let left = self.x.max(other.x);
        let top = self.y.max(other.y);
        let right = self.right().min(other.right());
        let bottom = self.bottom().min(other.bottom());
        if right <= left || bottom <= top {
            return None;
        }
        Some(Self {
            x: left,
            y: top,
            width: u32::try_from(right - left).ok()?,
            height: u32::try_from(bottom - top).ok()?,
        })
    }
}

/// Union of every rectangle in `rects`; `None` for an empty slice.
///
/// Empty rectangles are skipped, so a degenerate monitor cannot widen the
/// resulting desktop capture.
pub fn union_all(rects: &[Rect]) -> Option<Rect> {
    rects
        .iter()
        .filter(|rect| !rect.is_empty())
        .copied()
        .reduce(|acc, rect| acc.union(&rect))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn containment_is_half_open() {
        let rect = Rect::new(0, 0, 1920, 1080);
        assert!(rect.contains(0, 0));
        assert!(rect.contains(1919, 1079));
        assert!(!rect.contains(1920, 1079));
        assert!(!rect.contains(0, 1080));
        assert!(!rect.contains(-1, 0));
    }

    #[test]
    fn negative_origin_monitor_is_contained() {
        let left_head = Rect::new(-1920, 0, 1920, 1080);
        assert!(left_head.contains(-1920, 0));
        assert!(left_head.contains(-1, 1079));
        assert!(!left_head.contains(0, 0));
    }

    #[test]
    fn union_of_side_by_side_heads_spans_both() {
        let primary = Rect::new(0, 0, 1920, 1080);
        let right_head = Rect::new(1920, 0, 2560, 1440);
        assert_eq!(primary.union(&right_head), Rect::new(0, 0, 4480, 1440));
    }

    #[test]
    fn union_of_negative_origin_head_keeps_desktop_origin() {
        let left_head = Rect::new(-1920, -200, 1920, 1080);
        let primary = Rect::new(0, 0, 1920, 1080);
        assert_eq!(
            left_head.union(&primary),
            Rect::new(-1920, -200, 3840, 1280)
        );
    }

    #[test]
    fn union_all_skips_empty_rectangles() {
        let rects = [Rect::new(0, 0, 1920, 1080), Rect::new(5000, 5000, 0, 0)];
        assert_eq!(union_all(&rects), Some(Rect::new(0, 0, 1920, 1080)));
        assert_eq!(union_all(&[]), None);
    }

    #[test]
    fn extreme_coordinates_saturate_instead_of_wrapping() {
        let huge = Rect::new(i32::MAX - 10, 0, 4096, 4096);
        assert_eq!(huge.right(), i32::MAX);
        assert!(huge.contains(i32::MAX - 1, 0));
        assert!(!huge.contains(i32::MAX, 0));
    }

    #[test]
    fn intersect_clamps_a_region_to_the_screen() {
        let screen = Rect::new(0, 0, 1920, 1080);
        assert_eq!(
            screen.intersect(&Rect::new(-100, -100, 200, 200)),
            Some(Rect::new(0, 0, 100, 100))
        );
        assert_eq!(
            screen.intersect(&Rect::new(1900, 1000, 200, 200)),
            Some(Rect::new(1900, 1000, 20, 80))
        );
        assert_eq!(screen.intersect(&screen), Some(screen));
    }

    #[test]
    fn intersect_reports_no_overlap_instead_of_an_empty_rect() {
        let screen = Rect::new(0, 0, 1920, 1080);
        assert_eq!(screen.intersect(&Rect::new(5000, 5000, 10, 10)), None);
        // Touching edges share no pixel, so half-open arithmetic yields None.
        assert_eq!(screen.intersect(&Rect::new(1920, 0, 100, 100)), None);
        assert_eq!(screen.intersect(&Rect::new(0, 0, 0, 100)), None);
    }
}
