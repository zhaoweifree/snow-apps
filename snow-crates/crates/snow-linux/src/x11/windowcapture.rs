//! Reading the focused window as a capture target.
//!
//! The X11 work is the same walk the full-screen check does -- `_NET_ACTIVE_WINDOW`
//! (or the keyboard focus), up to the top-level frame, classified by the nearest
//! `_NET_WM_WINDOW_TYPE` in its ancestry -- because both questions are about the
//! window the user is looking at. Sharing it is what keeps them from answering
//! differently, which would show up as a capture that disagrees with the
//! shortcut suppression about whether a window is on screen.
//!
//! What is added here is the id. A capture request names its target, and the
//! history records what it came from, so the window id has to survive the walk.

use super::X11Session;
use crate::error::LinuxResult;
use crate::windowcapture::{WindowTarget, capture_target};

/// The focused window as a capture target, or `None` when it is not one.
///
/// `None` is also the answer when nothing has focus or the window disappeared
/// mid-read: the caller has nothing different to do about either, and reporting
/// "no window" is what makes the capture path fall back to the user's other
/// option instead of returning a picture of whatever was underneath.
pub fn focused_window_target(session: &X11Session) -> LinuxResult<Option<WindowTarget>> {
    let Some((id, window)) = super::fullscreen::focused_window_with_id(session)? else {
        return Ok(None);
    };
    Ok(capture_target(u64::from(id), &window))
}
