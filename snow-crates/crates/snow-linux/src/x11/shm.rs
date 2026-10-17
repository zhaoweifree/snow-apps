//! MIT-SHM capture: the server writes the frame into memory both sides map.
//!
//! Core `GetImage` sends every pixel through the connection. For a 1920x1080
//! root that is ~8 MB per frame, allocated by the server, copied into a reply,
//! and copied again out of it -- three copies of the desktop, per frame, at
//! whatever rate the recorder asks for. `ShmGetImage` replaces all three with a
//! single write into a segment the client can already read, and the reply
//! shrinks to a depth and a byte count.
//!
//! The whole path is optional. MIT-SHM needs a server that will map a file
//! descriptor it did not create, which is true of a local display and routinely
//! false for anything else: a remote display over TCP, a server built without
//! the extension, a hardened sandbox. Every one of those answers falls back to
//! `GetImage` rather than failing, because a slower capture is still a capture.
//!
//! Sizing is the one place this can go wrong *quietly*. `ShmGetImage` has no
//! reply data, so the client must allocate before it knows the size, using the
//! server's own padding rule ([`crate::capture::shm_segment_size`]). The reply's
//! `size` then says how much was actually written, and the stride is derived
//! from that number rather than from the guess -- a disagreement between the two
//! is treated as "this path cannot be trusted right now" and the frame goes
//! through `GetImage` instead of being decoded with the wrong row pitch.
//!
//! Preparation and reading are separate steps for a borrow reason worth
//! remembering: preparing may allocate, so it needs `&mut self`, while reading
//! hands out a slice of the mapping, so it must borrow. A single method doing
//! both cannot return a borrow of itself along one path and keep using itself on
//! the other -- the mutable borrow would have to live as long as the returned
//! image.

use std::os::fd::AsRawFd;

use memmap2::{MmapMut, MmapOptions};
use x11rb::connection::Connection;
use x11rb::protocol::shm as xshm;
use x11rb::protocol::xproto::{ImageFormat, ImageOrder};

use super::{ImageData, RawImage, X11Session};
use crate::capture::{ImageLayout, reply_stride, shm_segment_size};
use crate::error::{LinuxError, LinuxResult};
use crate::geometry::Rect;

/// Whether this connection can use shared memory, and the segment it holds.
pub(crate) enum ShmState {
    /// Not attempted yet: the first capture decides.
    Unused,
    Ready(Segment),
    /// Attempted and refused.
    ///
    /// Not retried. The reasons a server declines -- no MIT-SHM extension, a
    /// display that is not this machine, a descriptor it will not map -- are
    /// properties of the connection, not of the moment, so retrying would spend
    /// a round trip per frame to learn the same thing.
    Unavailable,
}

/// A shared segment plus the X resource that refers to it.
pub(crate) struct Segment {
    /// The XID the server knows this segment by.
    id: u32,
    /// The client's mapping of the same memory.
    map: MmapMut,
}

impl Segment {
    /// Ask the server for a segment of `bytes` and map it.
    ///
    /// `CreateSegment` (MIT-SHM 1.2) is used rather than SysV `shmget` +
    /// `Attach`: the server allocates and returns a descriptor, so the client
    /// needs no System V IPC at all and the segment dies with the connection.
    fn create(session: &X11Session, bytes: u32) -> LinuxResult<Self> {
        let conn = session.conn();
        let id = conn.generate_id().map_err(|error| {
            LinuxError::other(format!("allocating a shared memory segment id: {error}"))
        })?;
        let reply = xshm::create_segment(conn, id, bytes, false)
            .map_err(|error| LinuxError::backend("creating a shared memory segment", error))?
            .reply()
            .map_err(|error| LinuxError::reply("creating a shared memory segment", error))?;

        // SAFETY: the descriptor belongs to the segment the server just created
        // at our request, it is still open here (the container closes it when
        // `reply` is dropped at the end of this function), and the length is the
        // one we asked for. The mapping outlives the descriptor: it stays valid
        // after `close`, which is what makes this a mapping rather than a read.
        let map = unsafe {
            MmapOptions::new()
                .len(bytes as usize)
                .map_mut(reply.shm_fd.as_raw_fd())
        }
        .map_err(|error| LinuxError::other(format!("mapping the shared segment: {error}")))?;

        Ok(Self { id, map })
    }

    fn id(&self) -> u32 {
        self.id
    }

    fn len(&self) -> usize {
        self.map.len()
    }

    fn written(&self, bytes: usize) -> &[u8] {
        &self.map[..bytes]
    }
}

impl X11Session {
    /// Bytes a shared-memory capture of `rect` needs, or `None` when this
    /// connection cannot say (unknown depth, absurd extent).
    fn segment_bytes(&self, rect: Rect) -> Option<u32> {
        let depth = self.screen().root_depth;
        let format = self.pixmap_format(depth)?;
        shm_segment_size(
            rect.width,
            rect.height,
            format.bits_per_pixel,
            format.scanline_pad,
        )
    }

    /// Decide whether this connection can capture through shared memory and
    /// allocate or grow the segment if it can.
    ///
    /// Called before every capture. It costs nothing once the segment fits: no
    /// request is sent, only a length comparison. Nothing here is fatal -- a
    /// refusal downgrades the session to `GetImage` for the rest of its life,
    /// and the caller is expected to carry on.
    pub(crate) fn prepare_shm(&mut self, rect: Rect) {
        if !self.shm_available() {
            self.shm = ShmState::Unavailable;
            return;
        }
        if matches!(self.shm, ShmState::Unavailable) {
            return;
        }
        let Some(bytes) = self.segment_bytes(rect) else {
            return;
        };
        let grown = match &self.shm {
            ShmState::Ready(segment) => segment.len() < bytes as usize,
            _ => true,
        };
        if !grown {
            return;
        }
        match Segment::create(self, bytes) {
            Ok(segment) => self.shm = ShmState::Ready(segment),
            Err(_) => self.shm = ShmState::Unavailable,
        }
    }

    /// Read `rect` through shared memory, or `None` when this connection cannot.
    ///
    /// `None` is not an error: it means "use `GetImage`", and the caller treats
    /// it that way. A protocol failure on a segment this connection already
    /// created *is* reported -- that means the display went away mid-capture
    /// rather than that shared memory is unavailable.
    pub(crate) fn read_shm_image(&self, effective: Rect) -> LinuxResult<Option<RawImage<'_>>> {
        let ShmState::Ready(segment) = &self.shm else {
            return Ok(None);
        };
        let Some(bytes) = self.segment_bytes(effective) else {
            return Ok(None);
        };
        if segment.len() < bytes as usize {
            return Ok(None);
        }

        let depth = self.screen().root_depth;
        let Some(format) = self.pixmap_format(depth) else {
            return Ok(None);
        };
        let bits_per_pixel = format.bits_per_pixel;
        let visual = self.root_visual()?;
        let (red_mask, green_mask, blue_mask) =
            (visual.red_mask, visual.green_mask, visual.blue_mask);
        let lsb_first = self.conn().setup().image_byte_order == ImageOrder::LSB_FIRST;

        let x = i16::try_from(effective.x)
            .map_err(|_| LinuxError::other("the capture rectangle's x is out of range"))?;
        let y = i16::try_from(effective.y)
            .map_err(|_| LinuxError::other("the capture rectangle's y is out of range"))?;
        let width = u16::try_from(effective.width)
            .map_err(|_| LinuxError::other("the capture rectangle is wider than 65535 pixels"))?;
        let height = u16::try_from(effective.height)
            .map_err(|_| LinuxError::other("the capture rectangle is taller than 65535 pixels"))?;

        let reply = xshm::get_image(
            self.conn(),
            self.root(),
            x,
            y,
            width,
            height,
            u32::MAX,
            u8::from(ImageFormat::Z_PIXMAP),
            segment.id(),
            0,
        )
        .map_err(|error| LinuxError::backend("requesting a shared memory image", error))?
        .reply()
        .map_err(|error| LinuxError::reply("requesting a shared memory image", error))?;

        // The server wrote into our mapping before this reply arrived, so a
        // count past the end means the padding rule did not hold. Do not read
        // it: hand the frame to the path that reads a length it can verify.
        let written = reply.size as usize;
        if written == 0 || written > segment.len() || reply.depth != depth {
            return Ok(None);
        }
        let Some(stride) = reply_stride(effective.width, effective.height, written, bits_per_pixel)
        else {
            return Ok(None);
        };
        let layout = ImageLayout::new(
            effective.width,
            effective.height,
            bits_per_pixel,
            stride,
            lsb_first,
            red_mask,
            green_mask,
            blue_mask,
        )?;

        Ok(Some(RawImage {
            rect: effective,
            layout,
            data: ImageData::Mapped(segment.written(written)),
        }))
    }
}
