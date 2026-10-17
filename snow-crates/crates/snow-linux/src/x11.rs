//! Thin, checked wrapper around a single X11 connection.
//!
//! All X11 traffic in the crate funnels through [`X11Session`] so that
//! connection setup, extension probing and error translation stay in one place.

pub mod pointer;
pub mod shm;
pub mod window;

use x11rb::connection::Connection;
use x11rb::protocol::randr;
use x11rb::protocol::shm as xshm;
use x11rb::protocol::xfixes;
use x11rb::protocol::xproto::{
    self, ConnectionExt as _, Format, ImageFormat, ImageOrder, Screen, Visualtype, Window,
};
use x11rb::rust_connection::RustConnection;

use crate::capture::ImageLayout;
use crate::error::{LinuxError, LinuxResult};
use crate::geometry::Rect;

/// Cached presence of the X extensions this backend depends on.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ExtensionSupport {
    pub randr: bool,
    pub xfixes: bool,
    /// MIT-SHM 1.2, which is the version that can hand out a descriptor for a
    /// server-allocated segment. An older server is reported as unsupported
    /// rather than probed again, because `GetImage` is a correct answer.
    pub shm: bool,
}

/// An owned X11 connection plus the screen selected at connect time.
pub struct X11Session {
    conn: RustConnection,
    screen_num: usize,
    extension: ExtensionSupport,
    /// The shared-memory read buffer, created on the first capture that can use
    /// it. It is a field rather than a local because re-creating the segment per
    /// frame would cost more than the copy it saves.
    shm: shm::ShmState,
}

impl std::fmt::Debug for X11Session {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("X11Session")
            .field("screen_num", &self.screen_num)
            .field("extension", &self.extension)
            .finish_non_exhaustive()
    }
}

impl X11Session {
    /// Connect using `$DISPLAY` (x11rb's `None` means "consult the environment").
    ///
    /// A host without `DISPLAY` is reported as [`LinuxError::SessionMissing`] so
    /// callers can degrade instead of treating a headless machine as a fault.
    /// Every other failure keeps the server's own message: a socket that is not
    /// accepting connections yet is transient (a compositor may still be coming
    /// up), so it must not be flattened into "unsupported".
    pub fn connect() -> LinuxResult<Self> {
        if std::env::var_os("DISPLAY").is_none_or(|value| value.is_empty()) {
            return Err(LinuxError::SessionMissing);
        }

        let (conn, screen_num) = x11rb::connect(None)
            .map_err(|err| LinuxError::other(format!("connecting to the X display: {err}")))?;

        // A well-formed display string can still name a screen the server does
        // not expose; that is as unusable as having no display at all.
        if screen_num >= conn.setup().roots.len() {
            return Err(LinuxError::SessionMissing);
        }

        let extension = probe_extensions(&conn);

        Ok(Self {
            conn,
            screen_num,
            extension,
            shm: shm::ShmState::Unused,
        })
    }

    pub fn conn(&self) -> &RustConnection {
        &self.conn
    }

    pub fn screen(&self) -> &Screen {
        &self.conn.setup().roots[self.screen_num]
    }

    pub fn root(&self) -> Window {
        self.screen().root
    }

    pub fn extension_support(&self) -> ExtensionSupport {
        self.extension
    }

    /// RandR 1.5 monitor objects: the single source of truth for logical
    /// monitor geometry, including negative-origin multi-head layouts.
    pub fn randr_monitors(&self) -> LinuxResult<Vec<randr::MonitorInfo>> {
        if !self.extension.randr {
            return Err(LinuxError::other("the RandR extension is unavailable"));
        }
        let reply = randr::get_monitors(&self.conn, self.root(), false)
            .map_err(|err| LinuxError::backend("querying RandR monitors", err))?
            .reply()
            .map_err(|err| LinuxError::reply("querying RandR monitors", err))?;
        Ok(reply.monitors)
    }

    /// Resolve an atom to its name; unnamed atoms yield an empty string.
    pub fn atom_name(&self, atom: x11rb::protocol::xproto::Atom) -> LinuxResult<String> {
        let reply = self
            .conn
            .get_atom_name(atom)
            .map_err(|err| LinuxError::backend("resolving an atom name", err))?
            .reply()
            .map_err(|err| LinuxError::reply("resolving an atom name", err))?;
        Ok(String::from_utf8_lossy(&reply.name).into_owned())
    }

    pub fn xfixes_available(&self) -> bool {
        self.extension.xfixes
    }

    /// The root window's geometry: everything a core `GetImage` can read.
    pub fn root_rect(&self) -> Rect {
        let screen = self.screen();
        Rect::new(
            0,
            0,
            u32::from(screen.width_in_pixels),
            u32::from(screen.height_in_pixels),
        )
    }

    /// Packed pixel layout of the screen's root visual at `depth`.
    ///
    /// Built from the server's own declarations rather than from a hard-coded
    /// 32-bit `BGRX`: the colour masks come from the visual, and the bits per
    /// pixel and scanline padding come from the setup's pixmap format for that
    /// depth -- the visual type carries no bits-per-pixel field, because one
    /// depth can be served at more than one pixel width. The server is free to
    /// run a 16-bit or big-endian root, and guessing produces a picture with the
    /// channels in the wrong places.
    ///
    /// `depth` is the depth the reply reported, which is what the returned
    /// bytes actually are.
    pub fn root_visual_layout(
        &self,
        width: u32,
        height: u32,
        depth: u8,
        data_len: usize,
    ) -> LinuxResult<ImageLayout> {
        let setup = self.conn.setup();
        let format = self.pixmap_format(depth).ok_or_else(|| {
            LinuxError::other("the server declares no pixmap format for the reply's depth")
        })?;
        let visual = self.root_visual()?;

        let bits_per_pixel = format.bits_per_pixel;
        // The reply itself carries no stride, so the server's declared padding
        // is the fallback for a reply too short to infer a stride from.
        let stride = crate::capture::reply_stride(width, height, data_len, bits_per_pixel)
            .unwrap_or_else(|| {
                crate::capture::scanline_bytes(width, bits_per_pixel, format.scanline_pad)
            });

        ImageLayout::new(
            width,
            height,
            bits_per_pixel,
            stride,
            setup.image_byte_order == ImageOrder::LSB_FIRST,
            visual.red_mask,
            visual.green_mask,
            visual.blue_mask,
        )
    }

    /// The server's pixmap format for a depth: bits per pixel and scanline pad.
    ///
    /// Not on the visual: X11 keeps one visual per depth here, but a depth can
    /// be served at more than one pixel width, and the pixel width is what the
    /// stride is computed from.
    fn pixmap_format(&self, depth: u8) -> Option<&Format> {
        self.conn
            .setup()
            .pixmap_formats
            .iter()
            .find(|format| format.depth == depth)
    }

    /// Whether this connection negotiated MIT-SHM 1.2.
    pub(crate) fn shm_available(&self) -> bool {
        self.extension.shm
    }

    fn root_visual(&self) -> LinuxResult<&Visualtype> {
        let screen = self.screen();
        screen
            .allowed_depths
            .iter()
            .flat_map(|depth| depth.visuals.iter())
            .find(|visual| visual.visual_id == screen.root_visual)
            .ok_or_else(|| {
                LinuxError::other("the root visual is missing from the screen's depth list")
            })
    }

    /// Read `rect` out of the root window.
    ///
    /// `rect` is clamped to the root window first: the protocol answers a
    /// rectangle that is not fully inside the drawable with `BadMatch`, and a
    /// partially off-screen screenshot is a normal request rather than an
    /// error. The returned [`RawImage::rect`] is the area actually read, so the
    /// caller can tell that clamping happened.
    ///
    /// Takes `&mut self` because the shared-memory buffer is part of the
    /// connection's state: the first capture allocates it, and later ones reuse
    /// or grow it. Re-creating a segment per frame costs more than the copy it
    /// saves, so it outlives the call.
    pub fn capture_image(&mut self, rect: Rect) -> LinuxResult<RawImage<'_>> {
        let effective = rect
            .intersect(&self.root_rect())
            .ok_or_else(|| LinuxError::other("the capture rectangle is outside the root window"))?;
        // Preparation may allocate, so it needs the mutable borrow; reading then
        // borrows the connection for as long as the returned image lives. See
        // the module note in `shm` for why those cannot be one call.
        self.prepare_shm(effective);
        self.read_image(effective)
    }

    /// Read a rectangle that is already known to be inside the root window.
    fn read_image(&self, effective: Rect) -> LinuxResult<RawImage<'_>> {
        if let Some(image) = self.read_shm_image(effective)? {
            return Ok(image);
        }

        let x = i16::try_from(effective.x)
            .map_err(|_| LinuxError::other("the capture rectangle's x is out of range"))?;
        let y = i16::try_from(effective.y)
            .map_err(|_| LinuxError::other("the capture rectangle's y is out of range"))?;
        let width = u16::try_from(effective.width)
            .map_err(|_| LinuxError::other("the capture rectangle is wider than 65535 pixels"))?;
        let height = u16::try_from(effective.height)
            .map_err(|_| LinuxError::other("the capture rectangle is taller than 65535 pixels"))?;

        // `AllPlanes`: copy every colour plane, not a subset.
        let reply = xproto::get_image(
            &self.conn,
            ImageFormat::Z_PIXMAP,
            self.root(),
            x,
            y,
            width,
            height,
            u32::MAX,
        )
        .map_err(|err| LinuxError::backend("requesting a root window image", err))?
        .reply()
        .map_err(|err| LinuxError::reply("requesting a root window image", err))?;

        let layout = self.root_visual_layout(
            effective.width,
            effective.height,
            reply.depth,
            reply.data.len(),
        )?;
        Ok(RawImage {
            rect: effective,
            layout,
            data: ImageData::Owned(reply.data),
        })
    }
}

/// Where a captured image's bytes live.
///
/// Shared-memory captures read straight out of the mapping, so the bytes are
/// borrowed rather than copied: that borrow *is* the zero-copy part. Owned bytes
/// remain for `GetImage`, whose reply is already an allocation.
pub enum ImageData<'a> {
    Owned(Vec<u8>),
    Mapped(&'a [u8]),
}

impl ImageData<'_> {
    pub fn as_slice(&self) -> &[u8] {
        match self {
            Self::Owned(data) => data,
            Self::Mapped(data) => data,
        }
    }
}

/// A captured image together with the layout needed to unpack it.
pub struct RawImage<'a> {
    /// The area actually read, after clamping to the root window.
    pub rect: Rect,
    pub layout: ImageLayout,
    pub data: ImageData<'a>,
}

impl RawImage<'_> {
    /// Decode into packed 8-bit pixels.
    pub fn decode_into(
        &self,
        destination: &mut [u8],
        order: crate::capture::OutputOrder,
    ) -> LinuxResult<()> {
        self.layout
            .decode_into(self.data.as_slice(), destination, order)
    }
}

fn probe_extensions(conn: &RustConnection) -> ExtensionSupport {
    // `GetMonitors` is a RandR 1.5 request: a 1.2-only server answers the
    // version handshake happily and then rejects the request, so the answer to
    // "is randr usable" has to come from the version, not from presence.
    let randr = randr::query_version(conn, 1, 5)
        .ok()
        .and_then(|cookie| cookie.reply().ok())
        .is_some_and(|reply| reply.major_version > 1 || reply.minor_version >= 5);

    let xfixes = xfixes::query_version(conn, 5, 0)
        .ok()
        .and_then(|cookie| cookie.reply().ok())
        .is_some();

    // `CreateSegment` is MIT-SHM 1.2: a 1.1 server accepts the handshake and
    // then answers the request with an error, so the version decides.
    let shm = xshm::query_version(conn)
        .ok()
        .and_then(|cookie| cookie.reply().ok())
        .is_some_and(|reply| reply.major_version > 1 || reply.minor_version >= 2);

    ExtensionSupport { randr, xfixes, shm }
}
