//! Pointer position and cursor-image sampling over X11.
//!
//! Position comes from the core `QueryPointer` request; the bitmap comes from
//! the XFixes extension, which is the only way to read the *composited* cursor
//! image (core X11 can only return a cursor handle, and its pixels are not
//! readable without XRender detours).

use x11rb::protocol::xfixes;
use x11rb::protocol::xproto::ConnectionExt as _;

use crate::error::{LinuxError, LinuxResult};
use crate::x11::X11Session;

/// A cursor bitmap in straight (non-premultiplied) alpha RGBA8.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct CursorImage {
    pub hotspot_x: u32,
    pub hotspot_y: u32,
    pub width: u32,
    pub height: u32,
    pub rgba: Vec<u8>,
}

/// One observation of the global pointer.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct CursorSample {
    /// Position in root-window (desktop) coordinates.
    pub x: i32,
    pub y: i32,
    /// `false` when the pointer has been grabbed onto another screen, which is
    /// the closest X11 gets to "the cursor is not on this desktop".
    pub on_screen: bool,
    /// `None` when XFixes is unavailable or the server reported a failed cursor.
    pub image: Option<CursorImage>,
}

/// Poll-based cursor sampler for X11.
///
/// The image is cached: X changes the pointer position far more often than its
/// shape, and cursors owned by other clients can transiently fail the
/// `GetCursorImage` round-trip. A transient failure must not erase a still-valid
/// shape.
pub struct X11CursorSampler {
    session: X11Session,
    cached_image: Option<CursorImage>,
}

impl std::fmt::Debug for X11CursorSampler {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("X11CursorSampler")
            .field("has_cached_image", &self.cached_image.is_some())
            .finish_non_exhaustive()
    }
}

impl X11CursorSampler {
    pub fn new() -> LinuxResult<Self> {
        Ok(Self {
            session: X11Session::connect()?,
            cached_image: None,
        })
    }

    pub fn session(&self) -> &X11Session {
        &self.session
    }

    /// Read the current pointer position synchronously, refreshing the cached
    /// cursor image when the server cooperates.
    pub fn sample(&mut self) -> LinuxResult<CursorSample> {
        let (x, y, on_screen) = self.pointer_position()?;
        let image = self.refresh_image();
        Ok(CursorSample {
            x,
            y,
            on_screen,
            image,
        })
    }

    fn pointer_position(&mut self) -> LinuxResult<(i32, i32, bool)> {
        let reply = self
            .session
            .conn()
            .query_pointer(self.session.root())
            .map_err(|err| LinuxError::backend("querying the pointer", err))?
            .reply()
            .map_err(|err| LinuxError::reply("querying the pointer", err))?;

        Ok((
            i32::from(reply.root_x),
            i32::from(reply.root_y),
            reply.same_screen,
        ))
    }

    fn refresh_image(&mut self) -> Option<CursorImage> {
        if !self.session.xfixes_available() {
            return self.cached_image.clone();
        }

        let reply = xfixes::get_cursor_image(self.session.conn()).ok()?.reply().ok()?;

        if let Some(image) = convert_cursor_image(&reply) {
            self.cached_image = Some(image);
        }
        self.cached_image.clone()
    }
}

fn convert_cursor_image(
    reply: &x11rb::protocol::xfixes::GetCursorImageReply,
) -> Option<CursorImage> {
    let width = u32::from(reply.width);
    let height = u32::from(reply.height);
    if width == 0 || height == 0 {
        return None;
    }

    let expected = (width as usize).checked_mul(height as usize)?;
    let rgba = crate::pixels::premultiplied_argb32_to_rgba8(&reply.cursor_image, expected)?;

    Some(CursorImage {
        hotspot_x: u32::from(reply.xhot),
        hotspot_y: u32::from(reply.yhot),
        width,
        height,
        rgba,
    })
}
