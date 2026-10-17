//! Reading and moving the pointer over a core X11 connection.
//!
//! Both requests are core protocol, so this works on a server that lacks
//! XInput2 or XFixes entirely. That is the reason the Qt application uses it
//! instead of a raw-device read: a screenshot annotator only needs the position
//! the server would report, not per-device state.

use x11rb::NONE;
use x11rb::connection::Connection;
use x11rb::protocol::xproto;

use crate::error::{LinuxError, LinuxResult};
use crate::pointer::{PointerSample, RootPosition};
use crate::x11::X11Session;

impl X11Session {
    /// Where the pointer is, in root-window pixels.
    pub fn pointer_sample(&self) -> LinuxResult<PointerSample> {
        let reply = xproto::query_pointer(self.conn(), self.root())
            .map_err(|err| LinuxError::backend("querying the pointer", err))?
            .reply()
            .map_err(|err| LinuxError::reply("querying the pointer", err))?;
        Ok(PointerSample::new(
            reply.root_x,
            reply.root_y,
            reply.same_screen,
        ))
    }

    /// Move the pointer to an absolute root position.
    ///
    /// No source window and a zero source rectangle make this an absolute jump
    /// rather than a relative move: `WarpPointer` interpolates only when it is
    /// given a rectangle to travel from.
    pub fn warp_pointer_to(&self, target: RootPosition) -> LinuxResult<()> {
        xproto::warp_pointer(
            self.conn(),
            NONE,
            self.root(),
            0,
            0,
            0,
            0,
            target.x,
            target.y,
        )
        .map_err(|err| LinuxError::backend("moving the pointer", err))?;
        // `WarpPointer` has no reply, so without this the request would sit in
        // the outgoing buffer until some later round trip flushed it and the
        // cursor would move at whatever moment that happened to be. Callers
        // treat a successful return as "the pointer has moved".
        self.conn()
            .flush()
            .map_err(|err| LinuxError::backend("flushing the pointer move", err))?;
        Ok(())
    }
}
