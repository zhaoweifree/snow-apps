//! X Damage tracking on the root window.
//!
//! A `Damage` object reports, for one drawable, what changed since the client
//! last subtracted the accumulated region. Pointed at the root window it is the
//! cheapest possible answer to "did the desktop change at all": one round trip
//! per frame instead of a full `GetImage`.
//!
//! The one precondition that makes the answer complete is a **running
//! compositor**. Without one, application windows draw directly to the screen and
//! the root window's damage only covers its own wallpaper -- a report of "nothing
//! changed" while a video plays would freeze the recording. With one
//! (`_NET_WM_CM_S<n>` selection owned), every window is redirected offscreen and
//! the compositor paints the composite into the root, so root damage is the
//! desktop's damage. That check is why [`DamageTracker::create`] returns
//! `Ok(None)` rather than a tracker that quietly under-reports.
//!
//! The tracker owns two X resources and no client-side state that needs
//! releasing: both are freed by the server when the connection closes, which for
//! this backend happens when the capturer is dropped.

use x11rb::NONE;
use x11rb::connection::Connection;
use x11rb::protocol::damage::{self, ReportLevel};
use x11rb::protocol::xfixes;
use x11rb::protocol::xproto::{ConnectionExt as _, Rectangle};

use super::X11Session;
use crate::damage::{Changes, localize};
use crate::error::{LinuxError, LinuxResult};
use crate::geometry::Rect;

/// A `Damage` object plus the region it is subtracted into.
pub struct DamageTracker {
    damage: u32,
    /// Reused every frame. `Subtract` writes the removed damage into this
    /// region, so it has to be emptied before the next `Subtract` -- otherwise
    /// each frame would report every change since the tracker was created.
    area: u32,
}

/// XFIXES spells the empty region as the nil resource id; `x11rb` has no name
/// for it because `Region` is a plain `u32`.
const REGION_NONE: u32 = 0;

impl DamageTracker {
    /// Create a tracker, or `Ok(None)` when damage cannot be trusted here.
    ///
    /// `Ok(None)` covers three environments that are all normal: a server
    /// without the Damage extension, a server without XFIXES (the region the
    /// answer is delivered in), and a session with no compositor. An `Err` is a
    /// protocol failure worth surfacing once, at construction.
    pub fn create(session: &X11Session) -> LinuxResult<Option<Self>> {
        if !session.extension_support().damage || !session.xfixes_available() {
            return Ok(None);
        }
        if !compositor_running(session)? {
            return Ok(None);
        }

        let conn = session.conn();
        let damage = conn
            .generate_id()
            .map_err(|error| LinuxError::other(format!("allocating a damage id: {error}")))?;
        let area = conn
            .generate_id()
            .map_err(|error| LinuxError::other(format!("allocating a region id: {error}")))?;

        damage::create(conn, damage, session.root(), ReportLevel::NON_EMPTY)
            .map_err(|error| LinuxError::backend("creating a damage object", error))?;
        xfixes::create_region(conn, area, &[])
            .map_err(|error| LinuxError::backend("creating a damage region", error))?;

        let mut tracker = Self { damage, area };
        // Whatever was painted between the server starting and this call is
        // already in the frame the caller is about to read. Reporting it as a
        // change would make the first frame look like a partial one.
        tracker.drain(session)?;
        Ok(Some(tracker))
    }

    /// Take the accumulated change set, as rectangles local to `area`.
    ///
    /// `area` is the rectangle the caller is capturing; damage outside it
    /// belongs to another monitor and is clipped away.
    pub fn take(&mut self, session: &X11Session, area: Rect) -> LinuxResult<Changes> {
        self.subtract(session)?;
        let rects = self.fetch_rectangles(session)?;
        Ok(localize(&rects, area))
    }

    /// Discard the accumulated change set without looking at it.
    fn drain(&mut self, session: &X11Session) -> LinuxResult<()> {
        self.subtract(session)?;
        self.fetch_rectangles(session).map(|_| ())
    }

    /// Move the damage accumulated so far into `self.area`.
    fn subtract(&mut self, session: &X11Session) -> LinuxResult<()> {
        // `REGION_NONE` as the parts argument means "every part", which is the
        // whole accumulated damage. `Subtract` has no reply, but the `FetchRegion`
        // that follows it does, and the server processes requests in order --
        // the round trip is what makes the answer belong to this frame.
        damage::subtract(session.conn(), self.damage, self.area, REGION_NONE)
            .map_err(|error| LinuxError::backend("subtracting accumulated damage", error))?;
        Ok(())
    }

    /// Read `self.area` out and empty it.
    fn fetch_rectangles(&mut self, session: &X11Session) -> LinuxResult<Vec<Rect>> {
        let conn = session.conn();
        let reply = xfixes::fetch_region(conn, self.area)
            .map_err(|error| LinuxError::backend("reading the damage region", error))?
            .reply()
            .map_err(|error| LinuxError::reply("reading the damage region", error))?;

        // Emptying is not an optimisation: `Subtract` adds to the region's
        // existing contents, so leaving the rectangles in place would report the
        // same change again on every following frame.
        xfixes::set_region(conn, self.area, &[])
            .map_err(|error| LinuxError::backend("clearing the damage region", error))?;

        Ok(reply.rectangles.iter().copied().map(rectangle).collect())
    }
}

/// `XFIXES` delivers `i16`/`u16` fields; desktop coordinates are `i32`/`u32`.
fn rectangle(shape: Rectangle) -> Rect {
    Rect::new(
        i32::from(shape.x),
        i32::from(shape.y),
        u32::from(shape.width),
        u32::from(shape.height),
    )
}

/// Whether a compositor owns `_NET_WM_CM_S<n>` for this screen.
///
/// The selection exists so that only one compositor can run per screen; its
/// owner is the cheapest reliable answer. Checked once at creation, because a
/// compositor that exits mid-recording is covered by the capturer's heartbeat
/// rather than by a round trip per frame.
fn compositor_running(session: &X11Session) -> LinuxResult<bool> {
    let conn = session.conn();
    let name = format!("_NET_WM_CM_S{}", session.screen_num());
    let atom = conn
        .intern_atom(false, name.as_bytes())
        .map_err(|error| LinuxError::backend("interning the compositor selection atom", error))?
        .reply()
        .map_err(|error| LinuxError::reply("interning the compositor selection atom", error))?
        .atom;
    if atom == NONE {
        return Ok(false);
    }

    let owner = conn
        .get_selection_owner(atom)
        .map_err(|error| LinuxError::backend("querying the compositor selection owner", error))?
        .reply()
        .map_err(|error| LinuxError::reply("querying the compositor selection owner", error))?
        .owner;
    Ok(owner != NONE)
}
