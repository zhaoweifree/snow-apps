//! Change tracking for X11 damage reports.
//!
//! The X Damage extension answers one question per frame: which parts of a
//! drawable changed since the client last looked. On the root window that is
//! the desktop as a whole, which is what a recorder needs twice over -- to skip
//! a frame entirely when nothing was painted, and to tell an encoder which
//! blocks have to be re-encoded when only a small area moved.
//!
//! Everything here is pure: rectangles in, rectangles out, plus a decision made
//! from three clock readings. The protocol calls live in the Linux-only `x11`
//! module, so this file compiles everywhere and its tests run on any host.
//!
//! Two rules shape the API, both of them about staying *safe* rather than
//! optimal:
//!
//! * A reported region is a **superset** of what changed, never a subset. Merging
//!   two rectangles into their bounding box re-encodes pixels that did not move;
//!   dropping one pixel that did move leaves a permanent artifact on screen.
//!   Every merge below therefore only ever grows.
//! * Both shortcuts expire. Damage that is not reported (a compositor flipping a
//!   hardware plane, a driver bug) would otherwise freeze the picture for as
//!   long as the recording lasts, so [`decide`] forces a real capture every
//!   [`DamagePolicy::heartbeat`] and a complete frame every
//!   [`DamagePolicy::full_refresh`].

use std::time::Duration;

use crate::geometry::Rect;

/// Most rectangles one frame is allowed to report.
///
/// Encoders do better with one bounding box than with a hundred slivers, and a
/// region list that large is usually a full repaint described the hard way.
pub const MAX_RECTS: usize = 16;

/// Merging is quadratic per pass and repeated until stable, so the input is
/// bounded before any of it starts. A region this detailed is answered with a
/// full frame anyway.
const MAX_SOURCE_RECTS: usize = MAX_RECTS * 8;

/// Change rectangles for one frame, in coordinates local to the captured
/// target.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Changes {
    /// Non-overlapping, non-touching rectangles covering at least everything
    /// that changed inside the target. Empty means "nothing changed".
    pub rects: Vec<Rect>,
    /// The answer was too detailed to be used as an exact list, so `rects` must
    /// not be read at all.
    ///
    /// A truncated answer cannot drive partial encoding: the merged bounding box
    /// covers the whole screen often enough that sending the frame is cheaper,
    /// and an exact list short enough to be useful does not exist.
    pub truncated: bool,
}

impl Changes {
    /// Whether the frame carries no change at all.
    pub fn is_empty(&self) -> bool {
        self.rects.is_empty()
    }
}

/// Bring server rectangles into the captured target's own coordinates.
///
/// `source` is in root-window coordinates and `area` is the rectangle being
/// captured. Parts of a change that fall outside `area` belong to another
/// monitor and are dropped -- but only after clipping, so a change that
/// straddles the boundary still reports the half that is visible here.
pub fn localize(source: &[Rect], area: Rect) -> Changes {
    if area.is_empty() {
        return Changes {
            rects: Vec::new(),
            truncated: !source.is_empty(),
        };
    }
    if source.len() > MAX_SOURCE_RECTS {
        // Merging is quadratic per pass, so an unusually detailed region is
        // answered by "do not use this list" rather than by spending the frame
        // budget merging it.
        return Changes {
            rects: Vec::new(),
            truncated: true,
        };
    }

    let mut rects: Vec<Rect> = Vec::with_capacity(source.len());
    for rect in source {
        let Some(clipped) = rect.intersect(&area) else {
            continue;
        };
        let local = Rect::new(
            clipped.x.saturating_sub(area.x),
            clipped.y.saturating_sub(area.y),
            clipped.width,
            clipped.height,
        );
        if !local.is_empty() {
            rects.push(local);
        }
    }

    merge_touching(&mut rects);
    if rects.len() > MAX_RECTS {
        return Changes {
            rects: Vec::new(),
            truncated: true,
        };
    }

    Changes {
        rects,
        truncated: false,
    }
}

/// Collapse rectangles that overlap or share an edge into their union.
///
/// Bounding-box merging can swallow unchanged pixels between two distant
/// rectangles, which is why it is done only for rectangles that already touch:
/// two windows repainting in opposite corners stay two rectangles.
fn merge_touching(rects: &mut Vec<Rect>) {
    let mut merged = true;
    while merged {
        merged = false;
        'restart: for i in 0..rects.len() {
            for j in (i + 1)..rects.len() {
                if touches(&rects[i], &rects[j]) {
                    rects[i] = rects[i].union(&rects[j]);
                    rects.swap_remove(j);
                    merged = true;
                    break 'restart;
                }
            }
        }
    }
}

/// Overlap or shared edge. Half-open rectangles touch when one's right edge
/// equals the other's left edge, so the comparison uses `<=` on both sides.
fn touches(a: &Rect, b: &Rect) -> bool {
    a.x <= b.right() && b.x <= a.right() && a.y <= b.bottom() && b.y <= a.bottom()
}

/// When to stop trusting the damage report.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DamagePolicy {
    /// Longest stretch a skipped frame is allowed to last. Damage that never
    /// arrives (direct scanout, a driver that drops reports) would otherwise
    /// freeze the recording, so a real capture happens at least this often.
    pub heartbeat: Duration,
    /// Longest stretch before a frame is handed out without dirty rectangles.
    /// Partial frames are only safe while the encoder still has a complete
    /// reference; refreshing bounds how long an error can persist.
    pub full_refresh: Duration,
    /// Rectangle count above which a frame is captured in full.
    pub max_rects: usize,
}

impl Default for DamagePolicy {
    fn default() -> Self {
        Self {
            heartbeat: Duration::from_millis(500),
            full_refresh: Duration::from_secs(2),
            max_rects: MAX_RECTS,
        }
    }
}

/// What the capturer should do with this frame.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Decision {
    /// Nothing changed and the heartbeat is not due: hand back the previous
    /// frame marked as a duplicate, without reading the server at all.
    Skip,
    /// Read the frame and report no dirty rectangles.
    Full,
    /// Read the frame and report these rectangles (target-local) as the only
    /// parts worth re-encoding.
    Partial(Vec<Rect>),
}

/// Decide between skipping, a full frame and a partial one.
///
/// `since_capture` is how long ago pixels were last read from the server and
/// `since_full` how long ago a frame without dirty rectangles was produced.
/// Pass [`Duration::MAX`] for "never", which is the first frame.
pub fn decide(
    changes: &Changes,
    since_capture: Duration,
    since_full: Duration,
    policy: &DamagePolicy,
) -> Decision {
    if since_full >= policy.full_refresh {
        return Decision::Full;
    }
    if changes.truncated || changes.rects.len() > policy.max_rects {
        return Decision::Full;
    }
    if changes.rects.is_empty() {
        return if since_capture >= policy.heartbeat {
            Decision::Full
        } else {
            Decision::Skip
        };
    }
    Decision::Partial(changes.rects.clone())
}

/// How long a skipped frame waits before the next attempt.
///
/// The X connection is idle while nothing is painted, so without a pause the
/// capture loop would spin at whatever rate the server answers `DamageSubtract`
/// -- thousands of round trips per second to learn the same "nothing changed".
/// The hold is derived from the stream's own rate so that a 30 fps recording
/// polls 30 times a second, with a floor for very high rates and a ceiling for
/// anything the session has not told us yet.
pub fn duplicate_hold(target_fps: u32) -> Duration {
    const DEFAULT_HOLD: Duration = Duration::from_millis(16);
    const MIN_HOLD: Duration = Duration::from_millis(2);
    const MAX_HOLD: Duration = Duration::from_millis(33);

    if target_fps == 0 {
        return DEFAULT_HOLD;
    }
    Duration::from_millis(1000 / u64::from(target_fps)).clamp(MIN_HOLD, MAX_HOLD)
}

#[cfg(test)]
mod tests {
    use super::*;

    const SCREEN: Rect = Rect::new(0, 0, 1920, 1080);

    #[test]
    fn a_change_inside_the_target_is_translated_to_local_coordinates() {
        // Capturing the right half of the screen: a change at root x=1200 is
        // local x=240, and a caller that indexes its own buffer needs that.
        let area = Rect::new(960, 0, 960, 1080);
        let changes = localize(&[Rect::new(1200, 40, 10, 20)], area);
        assert_eq!(changes.rects, vec![Rect::new(240, 40, 10, 20)]);
        assert!(!changes.truncated);
    }

    #[test]
    fn a_change_on_another_monitor_is_dropped() {
        let area = Rect::new(0, 0, 1920, 1080);
        // A head placed to the left of primary, so the damage is at negative x.
        let changes = localize(&[Rect::new(-400, 100, 50, 50)], area);
        assert!(changes.is_empty());
        assert!(!changes.truncated);
    }

    #[test]
    fn a_change_straddling_the_target_edge_keeps_the_visible_half() {
        let area = Rect::new(0, 0, 1920, 1080);
        let changes = localize(&[Rect::new(1900, 1000, 200, 200)], area);
        assert_eq!(changes.rects, vec![Rect::new(1900, 1000, 20, 80)]);
    }

    #[test]
    fn touching_rectangles_merge_into_one() {
        // Two halves of a window repainting: adjacent edges, one rectangle.
        let area = SCREEN;
        let changes = localize(
            &[Rect::new(10, 10, 100, 100), Rect::new(110, 10, 100, 100)],
            area,
        );
        assert_eq!(changes.rects, vec![Rect::new(10, 10, 200, 100)]);
    }

    #[test]
    fn distant_rectangles_stay_separate() {
        let area = SCREEN;
        let changes = localize(
            &[Rect::new(10, 10, 100, 100), Rect::new(800, 600, 100, 100)],
            area,
        );
        assert_eq!(changes.rects.len(), 2);
        assert!(changes.rects.contains(&Rect::new(10, 10, 100, 100)));
        assert!(changes.rects.contains(&Rect::new(800, 600, 100, 100)));
    }

    #[test]
    fn overlapping_rectangles_merge_without_losing_coverage() {
        let area = SCREEN;
        let changes = localize(
            &[Rect::new(100, 100, 100, 100), Rect::new(150, 150, 100, 100)],
            area,
        );
        assert_eq!(changes.rects, vec![Rect::new(100, 100, 150, 150)]);
    }

    #[test]
    fn duplicate_rectangles_collapse() {
        let area = SCREEN;
        let changes = localize(&[Rect::new(5, 5, 10, 10), Rect::new(5, 5, 10, 10)], area);
        assert_eq!(changes.rects, vec![Rect::new(5, 5, 10, 10)]);
    }

    #[test]
    fn an_absurd_number_of_rectangles_is_refused_before_merging() {
        let area = SCREEN;
        // One rectangle per pixel row, all disjoint, far more than the cap.
        let source: Vec<Rect> = (0..(MAX_SOURCE_RECTS as i32 + 1))
            .map(|y| Rect::new(0, y * 2, 4, 1))
            .collect();
        let changes = localize(&source, area);
        assert!(changes.truncated);
        assert!(changes.rects.is_empty());
    }

    #[test]
    fn a_region_over_the_rectangle_cap_is_truncated_not_dropped() {
        // More disjoint rectangles than `MAX_RECTS` but under the source limit:
        // still usable as "something changed", never as an exact list.
        let source: Vec<Rect> = (0..(MAX_RECTS as i32 + 4))
            .map(|i| Rect::new(i * 100, 0, 10, 10))
            .collect();
        let changes = localize(&source, SCREEN);
        assert!(changes.truncated);
        assert!(changes.rects.is_empty());
    }

    #[test]
    fn degenerate_input_produces_no_change() {
        let changes = localize(
            &[Rect::new(10, 10, 0, 10), Rect::new(10, 10, 10, 0)],
            SCREEN,
        );
        assert!(changes.is_empty());
        assert!(!changes.truncated);
        assert!(localize(&[], SCREEN).is_empty());
    }

    #[test]
    fn an_empty_target_reports_nothing() {
        // A zero-extent capture cannot contain damage; reporting some would
        // make the caller re-encode rows it does not have.
        let changes = localize(&[Rect::new(0, 0, 10, 10)], Rect::new(0, 0, 0, 1080));
        assert!(changes.is_empty());
        assert!(changes.truncated);
    }

    #[test]
    fn nothing_changed_skips_the_frame() {
        let decision = decide(
            &Changes::default(),
            Duration::from_millis(10),
            Duration::from_millis(100),
            &DamagePolicy::default(),
        );
        assert_eq!(decision, Decision::Skip);
    }

    #[test]
    fn the_heartbeat_forces_a_capture_when_nothing_is_reported() {
        let policy = DamagePolicy::default();
        let decision = decide(
            &Changes::default(),
            policy.heartbeat,
            Duration::from_millis(100),
            &policy,
        );
        assert_eq!(decision, Decision::Full);
    }

    #[test]
    fn the_refresh_interval_forces_a_full_frame_even_with_a_known_change() {
        let policy = DamagePolicy::default();
        let changes = Changes {
            rects: vec![Rect::new(1, 1, 2, 2)],
            truncated: false,
        };
        assert_eq!(
            decide(&changes, Duration::ZERO, policy.full_refresh, &policy),
            Decision::Full
        );
    }

    #[test]
    fn a_small_change_is_reported_as_a_partial_frame() {
        let policy = DamagePolicy::default();
        let changes = Changes {
            rects: vec![Rect::new(1, 1, 2, 2)],
            truncated: false,
        };
        assert_eq!(
            decide(&changes, Duration::ZERO, Duration::ZERO, &policy),
            Decision::Partial(vec![Rect::new(1, 1, 2, 2)])
        );
    }

    #[test]
    fn a_truncated_region_forces_a_full_frame() {
        let policy = DamagePolicy::default();
        let changes = Changes {
            rects: vec![Rect::new(0, 0, 1920, 1080)],
            truncated: true,
        };
        assert_eq!(
            decide(&changes, Duration::ZERO, Duration::ZERO, &policy),
            Decision::Full
        );
    }

    #[test]
    fn the_first_frame_is_always_full() {
        let policy = DamagePolicy::default();
        let decision = decide(&Changes::default(), Duration::MAX, Duration::MAX, &policy);
        assert_eq!(decision, Decision::Full);
    }

    #[test]
    fn the_hold_follows_the_stream_rate() {
        assert_eq!(duplicate_hold(0), Duration::from_millis(16));
        assert_eq!(duplicate_hold(30), Duration::from_millis(33));
        assert_eq!(duplicate_hold(60), Duration::from_millis(16));
        // 1000/144 truncates to 6 ms, still above the floor.
        assert_eq!(duplicate_hold(144), Duration::from_millis(6));
        // A rate the server cannot keep up with must not turn into a busy loop.
        assert_eq!(duplicate_hold(10_000), Duration::from_millis(2));
        // ...nor into a stall that hides a change for a whole frame period.
        assert_eq!(duplicate_hold(1), Duration::from_millis(33));
    }
}
