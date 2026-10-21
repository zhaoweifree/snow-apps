//! Translation of X11 `GetImage` replies into packed 8-bit pixels.
//!
//! Core `GetImage` returns a drawable's pixels in *the server's* memory layout,
//! described by a visual (bits per pixel, one bit mask per colour channel) plus
//! the connection's image byte order. Decoding is therefore mask-driven rather
//! than assumed: a 16-bit RGB565 root, a big-endian server and a 32-bit BGRX
//! root all take the same path, and none of them can be confused with each
//! other.
//!
//! X11 pixels carry no alpha. The fourth byte of a 32-bit pixel is padding, and
//! whatever the server leaves there is not opacity -- so every decoded pixel is
//! written fully opaque instead of passing the padding through. A screenshot
//! with a transparent hole would composite as a hole.
//!
//! Everything here is pure, so it compiles and its tests run on any host,
//! including the Windows workstation the port is developed on.

use crate::error::{LinuxError, LinuxResult};
use crate::geometry::Rect;

/// Byte order the decoder writes for each pixel.
///
/// The frame pipeline consumes either order, so writing the requested one
/// directly avoids a second full-frame pass over the pixels.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum OutputOrder {
    /// Blue, green, red, alpha. This is the order X11 itself uses on
    /// little-endian servers, so the common case degenerates to a row copy.
    Bgra,
    /// Red, green, blue, alpha.
    Rgba,
}

/// Where a decoded image belongs inside a larger frame.
///
/// The extent and the origin travel together because neither means anything
/// without the other: `x` is only in range relative to `width`, and a stride
/// mistaken for an extent writes rows at the wrong offset rather than failing.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct OutputWindow {
    /// The destination's width in pixels, i.e. its row stride in pixels.
    pub width: u32,
    pub height: u32,
    pub x: u32,
    pub y: u32,
}

impl OutputWindow {
    /// The whole of a `width` x `height` destination.
    pub const fn full(width: u32, height: u32) -> Self {
        Self {
            width,
            height,
            x: 0,
            y: 0,
        }
    }
}

/// One colour channel packed into a pixel, as a shift plus a width.
///
/// Storing the split form rather than the raw mask keeps the hot loop free of
/// per-pixel mask scans.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ChannelMask {
    shift: u32,
    bits: u32,
}

impl ChannelMask {
    /// Decompose a mask into `(shift, width)`.
    ///
    /// `None` unless the mask is a single contiguous run of set bits: a
    /// discontiguous mask (say `0b1011`) has no single shift that extracts it,
    /// and silently picking one would produce a plausible-looking wrong image.
    pub fn split(mask: u32) -> Option<Self> {
        if mask == 0 {
            return None;
        }
        let shift = mask.trailing_zeros();
        let bits = (mask >> shift).trailing_ones();
        // `checked_shr` reports a shift of 32+ as `None`, which is exactly the
        // "nothing above this run" answer we want for a mask that ends at bit 31.
        if mask.checked_shr(shift + bits).unwrap_or(0) != 0 {
            return None;
        }
        Some(Self { shift, bits })
    }

    pub const fn shift(self) -> u32 {
        self.shift
    }

    pub const fn bits(self) -> u32 {
        self.bits
    }

    /// Pull this channel out of `pixel` and scale it to 8 bits.
    ///
    /// Narrow channels are scaled arithmetically with rounding rather than
    /// bit-replicated; both reach full scale at the top code, and the scaled
    /// form keeps a 5-bit midpoint from drifting a code off (see the tests).
    pub fn extract(self, pixel: u32) -> u8 {
        let raw = (pixel >> self.shift) & low_bits(self.bits);
        if self.bits >= 8 {
            (raw >> (self.bits - 8)) as u8
        } else {
            let max = low_bits(self.bits);
            ((raw * 255 + max / 2) / max) as u8
        }
    }
}

/// `bits` low bits set; saturates instead of shifting by 32.
const fn low_bits(bits: u32) -> u32 {
    if bits >= 32 {
        u32::MAX
    } else {
        (1u32 << bits) - 1
    }
}

/// Packed pixel layout of an `GetImage` reply.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ImageLayout {
    pub width: u32,
    pub height: u32,
    pub bits_per_pixel: u8,
    /// Server-side row stride. The last row may be shorter; see
    /// [`ImageLayout::required_len`].
    pub bytes_per_line: u32,
    pub lsb_first: bool,
    pub red: ChannelMask,
    pub green: ChannelMask,
    pub blue: ChannelMask,
}

impl ImageLayout {
    /// Validate a layout described by a visual.
    ///
    /// Rejects anything the decoder cannot represent exactly, because every
    /// alternative is a silently wrong picture: a depth-8 `PseudoColor` root
    /// resolves through a colormap this backend does not read, and a
    /// discontiguous mask has no single extraction shift.
    /// Eight scalars, all of them load-bearing: the geometry, the pixel width,
    /// the stride, the byte order and three colour masks. Bundling them into a
    /// struct would only move the argument list to the call site.
    #[allow(clippy::too_many_arguments)]
    pub fn new(
        width: u32,
        height: u32,
        bits_per_pixel: u8,
        bytes_per_line: u32,
        lsb_first: bool,
        red_mask: u32,
        green_mask: u32,
        blue_mask: u32,
    ) -> LinuxResult<Self> {
        if width == 0 || height == 0 {
            return Err(LinuxError::other("the capture rectangle is empty"));
        }
        let pixel_bytes = match bits_per_pixel {
            8 | 16 | 24 | 32 => usize::from(bits_per_pixel) / 8,
            _ => {
                return Err(LinuxError::UnsupportedPixelLayout {
                    reason: "bits_per_pixel is not 8, 16, 24 or 32",
                });
            }
        };
        let minimum_stride = u32::try_from(pixel_bytes)
            .ok()
            .and_then(|bytes| width.checked_mul(bytes))
            .ok_or_else(|| LinuxError::other("the capture rectangle is too wide"))?;
        if bytes_per_line < minimum_stride {
            return Err(LinuxError::other(
                "the GetImage row stride is narrower than the requested width",
            ));
        }

        let split = |mask: u32| {
            ChannelMask::split(mask).ok_or(LinuxError::UnsupportedPixelLayout {
                reason: "a colour mask is not a contiguous run of bits",
            })
        };
        Ok(Self {
            width,
            height,
            bits_per_pixel,
            bytes_per_line,
            lsb_first,
            red: split(red_mask)?,
            green: split(green_mask)?,
            blue: split(blue_mask)?,
        })
    }

    pub const fn pixel_bytes(&self) -> usize {
        self.bits_per_pixel as usize / 8
    }

    /// Bytes the reply must hold: each row is `bytes_per_line` long except the
    /// last, which the server is permitted to truncate after the final pixel.
    pub fn required_len(&self) -> usize {
        let stride = self.bytes_per_line as usize;
        let rows = self.height as usize - 1;
        let last_row = self.width as usize * self.pixel_bytes();
        stride * rows + last_row
    }

    /// Bytes the decoder writes: `width * height * 4`.
    pub fn output_len(&self) -> usize {
        self.width as usize * self.height as usize * 4
    }

    /// True when the packed bytes are already `B,G,R,_` and only need the
    /// alpha byte replaced.
    ///
    /// Deliberately checks the masks rather than trusting a convention: the
    /// fast path is only sound when the layout really is 32-bit low-byte-blue.
    fn is_native_bgrx(&self) -> bool {
        self.bits_per_pixel == 32
            && self.lsb_first
            && self.red.bits() == 8
            && self.green.bits() == 8
            && self.blue.bits() == 8
            && self.red.shift() == 16
            && self.green.shift() == 8
            && self.blue.shift() == 0
    }

    /// Decode a `GetImage` reply into packed pixels.
    pub fn decode_into(
        &self,
        source: &[u8],
        destination: &mut [u8],
        order: OutputOrder,
    ) -> LinuxResult<()> {
        self.decode_window_into(
            source,
            destination,
            OutputWindow::full(self.width, self.height),
            order,
        )
    }

    /// Decode this image into a sub-rectangle of a larger destination.
    ///
    /// This is what makes a region capture read only the region: the server is
    /// asked for the sub-rectangle, and its rows land at `(x, y)` inside a
    /// destination whose own stride is `destination_width` pixels. The
    /// alternative -- read the whole frame, then copy the window out of it --
    /// moves the same bytes over the connection *and* through a temporary.
    ///
    /// Every bound is checked here rather than left to the caller: a window
    /// that does not fit is an error, because the alternative is either a panic
    /// inside the row slicing or, with saturating arithmetic, rows written at
    /// the wrong offset -- a picture that looks plausible and is shifted.
    pub fn decode_window_into(
        &self,
        source: &[u8],
        destination: &mut [u8],
        window: OutputWindow,
        order: OutputOrder,
    ) -> LinuxResult<()> {
        let OutputWindow {
            width: destination_width,
            height: destination_height,
            x,
            y,
        } = window;
        let required = self.required_len();
        if source.len() < required {
            return Err(LinuxError::other(format!(
                "the GetImage reply holds {} bytes, {required} are needed for {}x{}",
                source.len(),
                self.width,
                self.height
            )));
        }
        let expected = destination_width as usize * destination_height as usize * 4;
        if destination.len() != expected {
            return Err(LinuxError::other(format!(
                "the destination holds {} bytes, {expected} are needed for {}x{}",
                destination.len(),
                destination_width,
                destination_height
            )));
        }

        let right = x
            .checked_add(self.width)
            .filter(|right| *right <= destination_width)
            .ok_or_else(|| {
                LinuxError::other(format!(
                    "a {} pixel wide image does not fit at x = {x} in a {destination_width} pixel wide frame",
                    self.width
                ))
            })?;
        let bottom = y
            .checked_add(self.height)
            .filter(|bottom| *bottom <= destination_height)
            .ok_or_else(|| {
                LinuxError::other(format!(
                    "a {} pixel tall image does not fit at y = {y} in a {destination_height} pixel tall frame",
                    self.height
                ))
            })?;
        debug_assert!(right <= destination_width && bottom <= destination_height);

        let destination_stride = destination_width as usize * 4;
        let row_bytes = self.width as usize * 4;
        let source_stride = self.bytes_per_line as usize;
        let native_bgrx = self.is_native_bgrx();

        for row in 0..self.height as usize {
            let start = (y as usize + row) * destination_stride + x as usize * 4;
            let out_row = &mut destination[start..start + row_bytes];
            let source_row = &source[row * source_stride..];
            if native_bgrx {
                // 32-bit `B,G,R,X` rows: copy the row as a unit and patch the
                // alpha byte, instead of masking every pixel.
                out_row.copy_from_slice(&source_row[..row_bytes]);
                if order == OutputOrder::Rgba {
                    for pixel in out_row.chunks_exact_mut(4) {
                        pixel.swap(0, 2);
                    }
                }
                // X11 leaves the padding byte undefined (commonly zero,
                // sometimes stale), so opacity is asserted rather than
                // forwarded.
                for pixel in out_row.chunks_exact_mut(4) {
                    pixel[3] = 0xff;
                }
            } else {
                self.decode_generic_row(source_row, out_row, order);
            }
        }
        Ok(())
    }

    /// Mask-driven decode of one row, used for every layout the fast path does
    /// not cover. This is the definition of "correct" for the decoder, so the
    /// tests compare the fast path's output against it.
    fn decode_generic_row(&self, source_row: &[u8], out_row: &mut [u8], order: OutputOrder) {
        let pixel_bytes = self.pixel_bytes();
        for (column, pixel) in out_row.chunks_exact_mut(4).enumerate() {
            let offset = column * pixel_bytes;
            let raw = read_pixel(&source_row[offset..offset + pixel_bytes], self.lsb_first);
            let (r, g, b) = (
                self.red.extract(raw),
                self.green.extract(raw),
                self.blue.extract(raw),
            );
            match order {
                OutputOrder::Bgra => pixel.copy_from_slice(&[b, g, r, 0xff]),
                OutputOrder::Rgba => pixel.copy_from_slice(&[r, g, b, 0xff]),
            }
        }
    }
}

/// Assemble one pixel from its packed bytes in the server's byte order.
fn read_pixel(bytes: &[u8], lsb_first: bool) -> u32 {
    if lsb_first {
        bytes.iter().enumerate().fold(0u32, |acc, (index, byte)| {
            acc | (u32::from(*byte) << (index * 8))
        })
    } else {
        bytes
            .iter()
            .fold(0u32, |acc, byte| (acc << 8) | u32::from(*byte))
    }
}

/// Row stride the server pads a `GetImage` reply to.
///
/// The protocol's reply carries no stride field, so this reproduces the
/// server's own rule: pad each row to `scanline_pad` bits. [`reply_stride`]
/// still prefers the length the server actually sent, and this is the fallback
/// for a reply too short to infer one from.
pub fn scanline_bytes(width: u32, bits_per_pixel: u8, scanline_pad: u8) -> u32 {
    let pad = u64::from(scanline_pad.max(8));
    let bits = u64::from(width) * u64::from(bits_per_pixel);
    u32::try_from(bits.div_ceil(pad) * (pad / 8)).unwrap_or(u32::MAX)
}

/// Recover the row stride from a reply whose byte length is known.
///
/// `data_len` is the full reply, whose last row may be short, so the quotient
/// is only trustworthy when it lands on a whole number of rows.
pub fn reply_stride(width: u32, height: u32, data_len: usize, bits_per_pixel: u8) -> Option<u32> {
    if height == 0 {
        return None;
    }
    let per_row = data_len / height as usize;
    let minimum = width as usize * (usize::from(bits_per_pixel) / 8);
    u32::try_from(per_row).ok().filter(|_| per_row >= minimum)
}

/// Bytes the server needs in a shared-memory segment for this rectangle.
///
/// `ShmGetImage` has no reply data: the server writes straight into the
/// segment, and the reply only reports how much it wrote. So the client has to
/// ask for a segment big enough *before* it knows the size, using the same
/// padding rule [`scanline_bytes`] implements. Getting this wrong is not an
/// error the protocol reports -- a segment that is too small either truncates
/// the image or, worse, has the server write past the mapping.
///
/// `None` for a degenerate request and for anything that does not fit in the
/// 32-bit size field the request carries. Callers fall back to `GetImage`, which
/// needs no size up front.
pub fn shm_segment_size(
    width: u32,
    height: u32,
    bits_per_pixel: u8,
    scanline_pad: u8,
) -> Option<u32> {
    if width == 0 || height == 0 || bits_per_pixel == 0 {
        return None;
    }
    let pad = u64::from(scanline_pad.max(8));
    let bits = u64::from(width) * u64::from(bits_per_pixel);
    let stride = bits.div_ceil(pad) * (pad / 8);
    let bytes = stride.checked_mul(u64::from(height))?;
    u32::try_from(bytes).ok()
}

/// A sub-rectangle of a frame, plus where it belongs in someone else's frame.
///
/// The field names mirror `snow_capture`'s blit description: `src_*` is in the
/// captured frame's own coordinates and `dst_*` in the destination's.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RegionBlit {
    pub src_x: u32,
    pub src_y: u32,
    pub width: u32,
    pub height: u32,
    pub dst_x: u32,
    pub dst_y: u32,
}

/// The root-window rectangle a region capture has to read, or `None` when one
/// sub-rectangle read cannot serve it.
///
/// `base` is the frame the blit is relative to (a monitor's rectangle, in root
/// coordinates) and `root` the root window's own rectangle; `destination` is the
/// extent of the frame the pixels are going into.
///
/// Every rejection here has a fallback that already works -- read the whole
/// frame and copy the window out of it -- so this is deliberately strict rather
/// than clever. The two rejections worth naming:
///
/// * A window that is not inside `root`. Monitors can be laid out past the edge
///   of the screen; the whole-frame path clamps the request and reports what it
///   actually read, while a sub-rectangle path would have to shift the
///   destination to match. Declining keeps that translation out of this layer.
/// * A window that does not fit inside `base`. That is a caller bug (the blit
///   was computed from a different layout), and honouring it would silently mix
///   pixels from two adjacent monitors into one frame.
pub fn region_read_rect(
    blit: RegionBlit,
    base: Rect,
    root: Rect,
    destination: (u32, u32),
) -> Option<Rect> {
    if blit.width == 0 || blit.height == 0 {
        return None;
    }
    let right = blit
        .src_x
        .checked_add(blit.width)
        .filter(|right| *right <= base.width)?;
    let bottom = blit
        .src_y
        .checked_add(blit.height)
        .filter(|bottom| *bottom <= base.height)?;
    debug_assert!(right <= base.width && bottom <= base.height);
    if blit
        .dst_x
        .checked_add(blit.width)
        .is_none_or(|right| right > destination.0)
        || blit
            .dst_y
            .checked_add(blit.height)
            .is_none_or(|bottom| bottom > destination.1)
    {
        return None;
    }

    // `GetImage` carries `i16` origins and `u16` extents; anything past that
    // cannot be encoded, and the server would read a negative coordinate as a
    // pixel far to the left.
    let x = base.x.checked_add(i32::try_from(blit.src_x).ok()?)?;
    let y = base.y.checked_add(i32::try_from(blit.src_y).ok()?)?;
    if i16::try_from(x).is_err() || i16::try_from(y).is_err() {
        return None;
    }
    if u16::try_from(blit.width).is_err() || u16::try_from(blit.height).is_err() {
        return None;
    }

    let rect = Rect::new(x, y, blit.width, blit.height);
    // `intersect` returning the same rectangle is the containment test: half the
    // window outside the root is exactly the case this refuses.
    match rect.intersect(&root) {
        Some(clipped) if clipped == rect => Some(rect),
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The overwhelmingly common X11 root: depth 24 visual, 32 bits per pixel,
    /// little-endian, `0x00RRGGBB`.
    fn native_layout(width: u32, height: u32) -> ImageLayout {
        let stride = scanline_bytes(width, 32, 32);
        ImageLayout::new(
            width,
            height,
            32,
            stride,
            true,
            0x00ff_0000,
            0x0000_ff00,
            0x0000_00ff,
        )
        .expect("native X11 layout")
    }

    #[test]
    fn mask_split_accepts_contiguous_runs() {
        let mask = ChannelMask::split(0x0000_ff00).expect("contiguous");
        assert_eq!((mask.shift(), mask.bits()), (8, 8));
        let low = ChannelMask::split(0x0000_007f).expect("contiguous");
        assert_eq!((low.shift(), low.bits()), (0, 7));
        let high = ChannelMask::split(0xff00_0000).expect("contiguous");
        assert_eq!((high.shift(), high.bits()), (24, 8));
    }

    #[test]
    fn mask_split_rejects_discontiguous_and_empty_masks() {
        assert!(ChannelMask::split(0).is_none());
        assert!(ChannelMask::split(0b1011).is_none());
        // A run that reaches bit 31 must not shift by 32 while checking the rest.
        assert!(ChannelMask::split(0xffff_ffff).is_some());
    }

    #[test]
    fn narrow_channels_scale_to_full_range() {
        // RGB565: red and blue are 5 bits, green is 6.
        let visual = ImageLayout::new(1, 1, 16, 2, true, 0xf800, 0x07e0, 0x001f).unwrap();
        let white = 0xffffu16.to_le_bytes();
        let mut out = [0u8; 4];
        visual
            .decode_into(&white, &mut out, OutputOrder::Rgba)
            .unwrap();
        assert_eq!(out, [0xff, 0xff, 0xff, 0xff]);

        let black = 0u16.to_le_bytes();
        visual
            .decode_into(&black, &mut out, OutputOrder::Rgba)
            .unwrap();
        assert_eq!(out, [0, 0, 0, 0xff]);

        // Pure green in RGB565 is 0x07e0; red and blue must stay off.
        let green = 0x07e0u16.to_le_bytes();
        visual
            .decode_into(&green, &mut out, OutputOrder::Rgba)
            .unwrap();
        assert_eq!(out, [0, 0xff, 0, 0xff]);
    }

    #[test]
    fn native_bgrx_decodes_to_both_output_orders() {
        let layout = native_layout(2, 1);
        // Two pixels: opaque red then pure blue, with padding rubbish in byte 3.
        let source = [0x00, 0x00, 0xff, 0x11, 0xff, 0x00, 0x00, 0x22];

        let mut bgra = [0u8; 8];
        layout
            .decode_into(&source, &mut bgra, OutputOrder::Bgra)
            .unwrap();
        assert_eq!(bgra, [0x00, 0x00, 0xff, 0xff, 0xff, 0x00, 0x00, 0xff]);

        let mut rgba = [0u8; 8];
        layout
            .decode_into(&source, &mut rgba, OutputOrder::Rgba)
            .unwrap();
        assert_eq!(rgba, [0xff, 0x00, 0x00, 0xff, 0x00, 0x00, 0xff, 0xff]);
    }

    #[test]
    fn padded_rows_are_read_with_the_server_stride() {
        // 1 pixel wide but a stride of 8: the padding must not leak into row 2.
        let layout =
            ImageLayout::new(1, 2, 32, 8, true, 0x00ff_0000, 0x0000_ff00, 0x0000_00ff).unwrap();
        let source = [
            0x00, 0x00, 0xff, 0x00, 0xaa, 0xbb, 0xcc, 0xdd, // row 0 + padding
            0xff, 0x00, 0x00, 0x00, 0x11, 0x22, 0x33, 0x44, // row 1 + padding
        ];
        let mut out = [0u8; 8];
        layout
            .decode_into(&source, &mut out, OutputOrder::Bgra)
            .unwrap();
        assert_eq!(out, [0x00, 0x00, 0xff, 0xff, 0xff, 0x00, 0x00, 0xff]);
    }

    #[test]
    fn big_endian_servers_are_assembled_in_their_own_order() {
        // Same RGB value as the little-endian test, but the bytes arrive
        // most-significant first and must still decode to the same colour.
        let layout =
            ImageLayout::new(1, 1, 32, 4, false, 0x00ff_0000, 0x0000_ff00, 0x0000_00ff).unwrap();
        let source = [0x00, 0xff, 0x00, 0x00];
        let mut out = [0u8; 4];
        layout
            .decode_into(&source, &mut out, OutputOrder::Rgba)
            .unwrap();
        assert_eq!(out, [0xff, 0x00, 0x00, 0xff]);
    }

    #[test]
    fn truncated_reply_is_rejected_instead_of_read_past_the_end() {
        let layout = native_layout(4, 2);
        let mut out = [0u8; 32];
        assert!(
            layout
                .decode_into(&[0u8; 4], &mut out, OutputOrder::Bgra)
                .is_err()
        );
        assert!(layout.required_len() <= 32);
    }

    #[test]
    fn wrong_destination_size_is_rejected() {
        let layout = native_layout(2, 2);
        let mut out = [0u8; 8];
        assert!(
            layout
                .decode_into(&[0u8; 16], &mut out, OutputOrder::Bgra)
                .is_err()
        );
    }

    #[test]
    fn layout_rejects_shapes_it_cannot_decode_exactly() {
        // Depth-8 PseudoColor resolves through a colormap this backend never reads.
        assert!(matches!(
            ImageLayout::new(1, 1, 8, 1, true, 0, 0, 0),
            Err(LinuxError::UnsupportedPixelLayout { .. })
        ));
        // Not a whole number of bytes per pixel.
        assert!(matches!(
            ImageLayout::new(1, 1, 12, 2, true, 0xf00, 0x0f0, 0x00f),
            Err(LinuxError::UnsupportedPixelLayout { .. })
        ));
        // A discontiguous channel mask has no single extraction shift.
        assert!(matches!(
            ImageLayout::new(1, 1, 16, 2, true, 0x8f00, 0x00f0, 0x000f),
            Err(LinuxError::UnsupportedPixelLayout { .. })
        ));
    }

    #[test]
    fn layout_rejects_empty_and_over_narrow_rectangles() {
        assert!(ImageLayout::new(0, 4, 32, 16, true, 0xff0000, 0xff00, 0xff).is_err());
        assert!(ImageLayout::new(4, 0, 32, 16, true, 0xff0000, 0xff00, 0xff).is_err());
        // Stride smaller than one row of pixels would read neighbouring rows.
        assert!(ImageLayout::new(4, 1, 32, 8, true, 0xff0000, 0xff00, 0xff).is_err());
    }

    #[test]
    fn scanline_stride_follows_the_server_declared_padding() {
        assert_eq!(scanline_bytes(1920, 32, 32), 7680);
        assert_eq!(scanline_bytes(1920, 24, 32), 5760);
        // 3 pixels at 16 bpp is 6 bytes, padded to the declared 32-bit unit.
        assert_eq!(scanline_bytes(3, 16, 32), 8);
        assert_eq!(scanline_bytes(0, 32, 32), 0);
        // A 64-bit scanline pad only changes rows that are not already aligned.
        assert_eq!(scanline_bytes(3, 16, 64), 8);
        assert_eq!(scanline_bytes(9, 16, 64), 24);
        // Nonsense padding must not divide by zero or overflow.
        assert_eq!(scanline_bytes(1, 32, 0), 4);
    }

    #[test]
    fn reply_stride_prefers_what_the_server_actually_sent() {
        // 2 rows of 8 bytes: a server that padded a 1-pixel row to 8 is trusted.
        assert_eq!(reply_stride(1, 2, 16, 32), Some(8));
        // A quotient below one row of pixels is a truncated reply, not a stride.
        assert_eq!(reply_stride(4, 2, 16, 32), None);
        assert_eq!(reply_stride(1, 0, 0, 32), None);
    }

    #[test]
    fn native_fast_path_is_only_taken_for_the_matching_layout() {
        assert!(native_layout(2, 2).is_native_bgrx());
        // Same masks but big-endian bytes: the copy path would mis-order them.
        let mut big_endian = native_layout(2, 2);
        big_endian.lsb_first = false;
        assert!(!big_endian.is_native_bgrx());
        // 24 bits per pixel has no padding byte to overwrite.
        let packed = ImageLayout::new(2, 2, 24, 8, true, 0xff0000, 0xff00, 0xff).unwrap();
        assert!(!packed.is_native_bgrx());
        // Swapped red and blue masks are still contiguous but are not BGRX.
        let bgr =
            ImageLayout::new(1, 1, 32, 4, true, 0x0000_00ff, 0x0000_ff00, 0x00ff_0000).unwrap();
        assert!(!bgr.is_native_bgrx());
    }

    #[test]
    fn twenty_four_bit_pixels_decode_without_padding() {
        let packed = ImageLayout::new(2, 1, 24, 8, true, 0xff0000, 0xff00, 0xff).unwrap();
        // BGR triples, then two bytes of row padding.
        let source = [0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00];
        let mut out = [0u8; 8];
        packed
            .decode_into(&source, &mut out, OutputOrder::Rgba)
            .unwrap();
        assert_eq!(out, [0xff, 0x00, 0x00, 0xff, 0x00, 0x00, 0xff, 0xff]);
    }

    /// The fast path is a second implementation of the same decode, so it can
    /// drift from the reference. Compare them over a spread of pixel values
    /// rather than one hand-picked colour: an off-by-one in a mask shift shows
    /// up on some values and not others.
    #[test]
    fn native_fast_path_agrees_with_the_reference_decode() {
        const WIDTH: usize = 37;
        const HEIGHT: usize = 5;

        let layout = native_layout(WIDTH as u32, HEIGHT as u32);
        assert!(layout.is_native_bgrx());

        let mut source = vec![0u8; layout.bytes_per_line as usize * HEIGHT];
        for row in 0..HEIGHT {
            for column in 0..WIDTH {
                let offset = row * layout.bytes_per_line as usize + column * 4;
                // A deterministic spread over all 24 bits, plus a padding byte
                // that must never reach the output.
                let pixel = (row as u32 * 7919 + column as u32 * 104_729) & 0x00ff_ffff;
                source[offset] = (pixel & 0xff) as u8;
                source[offset + 1] = ((pixel >> 8) & 0xff) as u8;
                source[offset + 2] = ((pixel >> 16) & 0xff) as u8;
                source[offset + 3] = 0x5a;
            }
        }

        for order in [OutputOrder::Bgra, OutputOrder::Rgba] {
            let mut fast = vec![0u8; layout.output_len()];
            layout.decode_into(&source, &mut fast, order).unwrap();

            let mut reference = vec![0u8; layout.output_len()];
            for (row, out_row) in reference.chunks_exact_mut(WIDTH * 4).enumerate() {
                layout.decode_generic_row(
                    &source[row * layout.bytes_per_line as usize..],
                    out_row,
                    order,
                );
            }

            assert_eq!(fast, reference, "fast path diverged for {order:?}");
            // Opacity is asserted, never forwarded from the padding byte.
            assert!(fast.chunks_exact(4).all(|pixel| pixel[3] == 0xff));
        }
    }

    #[test]
    fn a_full_hd_segment_is_the_visible_bytes_plus_padding() {
        // 1920 * 4 is already a multiple of 4 bytes, so the segment is the
        // frame itself -- the case a UOS desktop will actually hit.
        assert_eq!(shm_segment_size(1920, 1080, 32, 32), Some(1920 * 4 * 1080));
        // A 1366-wide row is 5464 bytes, which pads to 5464 (already 4-aligned).
        assert_eq!(shm_segment_size(1366, 768, 32, 32), Some(1366 * 4 * 768));
    }

    #[test]
    fn a_narrow_row_is_padded_up_to_the_scanline_boundary() {
        // 3 pixels at 32bpp is 12 bytes: still a multiple of the 32-bit pad.
        assert_eq!(shm_segment_size(3, 2, 32, 8), Some(12 * 2));
        // At 24bpp a 1-pixel row is 3 bytes and pads to 4 with a 32-bit pad.
        assert_eq!(shm_segment_size(1, 1, 24, 32), Some(4));
        // The same row with an 8-bit pad needs no padding at all.
        assert_eq!(shm_segment_size(1, 1, 24, 8), Some(3));
    }

    #[test]
    fn a_degenerate_request_has_no_segment_size() {
        assert_eq!(shm_segment_size(0, 10, 32, 32), None);
        assert_eq!(shm_segment_size(10, 0, 32, 32), None);
        // A zero bit depth would divide the row down to nothing.
        assert_eq!(shm_segment_size(10, 10, 0, 32), None);
    }

    #[test]
    fn an_absurd_rectangle_reports_no_size_instead_of_wrapping() {
        // The request's size field is 32 bits, so a desktop-sized *desktop*
        // must be refused rather than truncated to something the server would
        // happily write past.
        assert_eq!(shm_segment_size(u32::MAX, u32::MAX, 32, 32), None);
        assert_eq!(shm_segment_size(65_536, 65_536, 32, 32), None);
    }

    /// A 16-bit `RGB565` root: the layout the native fast path does not cover,
    /// so it exercises the mask-driven decoder.
    fn rgb565_layout(width: u32, height: u32) -> ImageLayout {
        let stride = scanline_bytes(width, 16, 32);
        ImageLayout::new(width, height, 16, stride, true, 0xf800, 0x07e0, 0x001f)
            .expect("RGB565 layout")
    }

    /// 32-bit little-endian `B,G,R,X` for a `0x00RRGGBB` pixel, with an
    /// undefined (here: hostile) padding byte.
    fn bgrx(pixel: u32, padding: u8) -> [u8; 4] {
        [
            (pixel & 0xff) as u8,
            ((pixel >> 8) & 0xff) as u8,
            ((pixel >> 16) & 0xff) as u8,
            padding,
        ]
    }

    /// Read one pixel out of a packed `RGBA`-ordered frame.
    fn pixel_at(frame: &[u8], width: u32, x: u32, y: u32) -> [u8; 4] {
        let start = ((y * width) + x) as usize * 4;
        frame[start..start + 4].try_into().unwrap()
    }

    #[test]
    fn a_window_decode_writes_only_its_own_rectangle() {
        let layout = native_layout(2, 2);
        // Distinct per-pixel colours, so a swapped row or column is visible.
        let source: Vec<u8> = [
            0x00_11_22_33u32,
            0x00_44_55_66,
            0x00_77_88_99,
            0x00_aa_bb_cc,
        ]
        .iter()
        .flat_map(|pixel| bgrx(*pixel, 0xee))
        .collect();
        let mut destination = vec![0xccu8; 4 * 4 * 4];

        layout
            .decode_window_into(
                &source,
                &mut destination,
                OutputWindow {
                    width: 4,
                    height: 4,
                    x: 1,
                    y: 2,
                },
                OutputOrder::Bgra,
            )
            .unwrap();

        // The window landed where it was asked to, in the destination's order.
        assert_eq!(pixel_at(&destination, 4, 1, 2), [0x33, 0x22, 0x11, 0xff]);
        assert_eq!(pixel_at(&destination, 4, 2, 3), [0xcc, 0xbb, 0xaa, 0xff]);
        // Everything outside it is untouched, byte for byte.
        for y in 0..4 {
            for x in 0..4 {
                if (1..3).contains(&x) && (2..4).contains(&y) {
                    continue;
                }
                assert_eq!(pixel_at(&destination, 4, x, y), [0xcc; 4], "({x}, {y})");
            }
        }
    }

    #[test]
    fn a_window_decode_keeps_the_destination_stride() {
        // Three pixels of padding to the right of the window: a decoder that
        // assumed a contiguous destination would write the second row into the
        // tail of the first.
        let layout = native_layout(2, 2);
        let source: Vec<u8> = [
            0x00_00_00_0au32,
            0x00_00_00_0b,
            0x00_00_00_0c,
            0x00_00_00_0d,
        ]
        .iter()
        .flat_map(|pixel| bgrx(*pixel, 0x00))
        .collect();
        let mut destination = vec![0xccu8; 5 * 2 * 4];

        layout
            .decode_window_into(
                &source,
                &mut destination,
                OutputWindow {
                    width: 5,
                    height: 2,
                    x: 3,
                    y: 0,
                },
                OutputOrder::Bgra,
            )
            .unwrap();

        assert_eq!(pixel_at(&destination, 5, 3, 0), [0x0a, 0, 0, 0xff]);
        assert_eq!(pixel_at(&destination, 5, 4, 0), [0x0b, 0, 0, 0xff]);
        assert_eq!(pixel_at(&destination, 5, 3, 1), [0x0c, 0, 0, 0xff]);
        // The three leading pixels of the second row belong to nothing.
        assert_eq!(pixel_at(&destination, 5, 2, 1), [0xcc; 4]);
    }

    #[test]
    fn a_window_decode_at_the_origin_equals_a_whole_frame_decode() {
        let layout = native_layout(3, 2);
        let source: Vec<u8> = (0..6u32)
            .flat_map(|index| bgrx(index.wrapping_mul(0x00_01_02_03), 0x5a))
            .collect();
        let mut whole = vec![0u8; 3 * 2 * 4];
        let mut window = vec![0u8; 3 * 2 * 4];

        layout
            .decode_into(&source, &mut whole, OutputOrder::Rgba)
            .unwrap();
        layout
            .decode_window_into(
                &source,
                &mut window,
                OutputWindow {
                    width: 3,
                    height: 2,
                    x: 0,
                    y: 0,
                },
                OutputOrder::Rgba,
            )
            .unwrap();

        assert_eq!(whole, window);
    }

    #[test]
    fn a_window_that_does_not_fit_is_refused_instead_of_shifted() {
        let layout = native_layout(2, 2);
        let source = vec![0u8; 2 * 2 * 4];
        let mut destination = vec![0u8; 4 * 4 * 4];

        // One column past the right edge, one row past the bottom.
        assert!(
            layout
                .decode_window_into(
                    &source,
                    &mut destination,
                    OutputWindow {
                        width: 4,
                        height: 4,
                        x: 3,
                        y: 0
                    },
                    OutputOrder::Bgra
                )
                .is_err()
        );
        assert!(
            layout
                .decode_window_into(
                    &source,
                    &mut destination,
                    OutputWindow {
                        width: 4,
                        height: 4,
                        x: 0,
                        y: 3
                    },
                    OutputOrder::Bgra
                )
                .is_err()
        );
    }

    #[test]
    fn a_window_origin_that_would_wrap_is_refused_instead_of_panicking() {
        let layout = native_layout(2, 2);
        let source = vec![0u8; 2 * 2 * 4];
        let mut destination = vec![0u8; 4 * 4 * 4];
        // `x + width` overflows before it can be compared: the overflow has to
        // surface as an error, not as a wrapped origin that indexes somewhere
        // arbitrary in the destination.
        assert!(
            layout
                .decode_window_into(
                    &source,
                    &mut destination,
                    OutputWindow {
                        width: 4,
                        height: 4,
                        x: u32::MAX,
                        y: 0
                    },
                    OutputOrder::Bgra
                )
                .is_err()
        );
        assert!(
            layout
                .decode_window_into(
                    &source,
                    &mut destination,
                    OutputWindow {
                        width: 4,
                        height: 4,
                        x: 0,
                        y: u32::MAX
                    },
                    OutputOrder::Bgra
                )
                .is_err()
        );
        assert_eq!(destination, vec![0u8; 4 * 4 * 4]);
    }

    #[test]
    fn a_window_decode_of_a_narrow_layout_scales_and_orders_channels() {
        // RGB565 forces the generic path: red at the top of the word.
        let layout = rgb565_layout(2, 1);
        let source: Vec<u8> = [0xf800u16, 0x07e0u16]
            .iter()
            .flat_map(|pixel| pixel.to_le_bytes())
            .collect();
        let mut destination = vec![0u8; 3 * 4];

        layout
            .decode_window_into(
                &source,
                &mut destination,
                OutputWindow {
                    width: 3,
                    height: 1,
                    x: 1,
                    y: 0,
                },
                OutputOrder::Rgba,
            )
            .unwrap();

        assert_eq!(pixel_at(&destination, 3, 1, 0), [255, 0, 0, 0xff]);
        assert_eq!(pixel_at(&destination, 3, 2, 0), [0, 255, 0, 0xff]);
        assert_eq!(pixel_at(&destination, 3, 0, 0), [0; 4]);
    }

    #[test]
    fn a_destination_of_the_wrong_length_is_refused() {
        let layout = native_layout(2, 2);
        let source = vec![0u8; 2 * 2 * 4];
        // Described as 4x4 but only holds 4x3 pixels.
        let mut short = vec![0u8; 4 * 3 * 4];
        assert!(
            layout
                .decode_window_into(
                    &source,
                    &mut short,
                    OutputWindow {
                        width: 4,
                        height: 4,
                        x: 0,
                        y: 0
                    },
                    OutputOrder::Bgra
                )
                .is_err()
        );
        // A reply shorter than the image is refused before anything is written.
        let mut destination = vec![0u8; 2 * 2 * 4];
        assert!(
            layout
                .decode_window_into(
                    &source[..4],
                    &mut destination,
                    OutputWindow {
                        width: 2,
                        height: 2,
                        x: 0,
                        y: 0
                    },
                    OutputOrder::Bgra
                )
                .is_err()
        );
        assert_eq!(destination, vec![0u8; 2 * 2 * 4]);
    }

    #[test]
    fn a_region_read_translates_monitor_local_coordinates() {
        let base = Rect::new(1920, 0, 1920, 1080);
        let root = Rect::new(0, 0, 3840, 1080);
        let blit = RegionBlit {
            src_x: 10,
            src_y: 20,
            width: 100,
            height: 50,
            dst_x: 200,
            dst_y: 300,
        };
        // The second monitor's window is the first monitor's rectangle shifted
        // right by 1920 -- the read has to happen in root coordinates.
        assert_eq!(
            region_read_rect(blit, base, root, (400, 400)),
            Some(Rect::new(1930, 20, 100, 50))
        );
    }

    #[test]
    fn a_region_read_accepts_the_whole_frame() {
        let base = Rect::new(0, 0, 1280, 720);
        let blit = RegionBlit {
            src_x: 0,
            src_y: 0,
            width: 1280,
            height: 720,
            dst_x: 0,
            dst_y: 0,
        };
        assert_eq!(
            region_read_rect(blit, base, Rect::new(0, 0, 1280, 720), (1280, 720)),
            Some(base)
        );
    }

    #[test]
    fn a_region_read_refuses_a_window_that_hangs_off_the_screen() {
        // A monitor whose left edge is outside the root, with a window that
        // straddles the edge. The whole-frame path clamps and reports what it
        // read, so this layer declines instead of inventing a destination shift.
        // The window has to *overlap* the root: one entirely off-screen is
        // already refused by the intersection being empty, which would leave
        // the containment check untested.
        let base = Rect::new(-50, 0, 1920, 1080);
        let root = Rect::new(0, 0, 1920, 1080);
        let blit = RegionBlit {
            src_x: 0,
            src_y: 0,
            width: 100,
            height: 100,
            dst_x: 0,
            dst_y: 0,
        };
        assert_eq!(region_read_rect(blit, base, root, (200, 200)), None);
    }

    #[test]
    fn a_region_read_refuses_a_blit_from_outside_its_frame() {
        let base = Rect::new(0, 0, 100, 100);
        let root = Rect::new(0, 0, 1920, 1080);
        // One pixel wider than the monitor it claims to come from: honouring
        // this would read a column of the neighbouring monitor.
        let blit = RegionBlit {
            src_x: 1,
            src_y: 0,
            width: 100,
            height: 10,
            dst_x: 0,
            dst_y: 0,
        };
        assert_eq!(region_read_rect(blit, base, root, (200, 200)), None);
    }

    #[test]
    fn a_region_read_refuses_a_window_larger_than_its_destination() {
        let base = Rect::new(0, 0, 1920, 1080);
        let root = Rect::new(0, 0, 1920, 1080);
        let blit = RegionBlit {
            src_x: 0,
            src_y: 0,
            width: 100,
            height: 100,
            dst_x: 50,
            dst_y: 0,
        };
        assert_eq!(region_read_rect(blit, base, root, (149, 100)), None);
        assert!(region_read_rect(blit, base, root, (150, 100)).is_some());
    }

    #[test]
    fn a_region_read_refuses_an_empty_or_unencodable_window() {
        let base = Rect::new(0, 0, 1920, 1080);
        let root = Rect::new(0, 0, 1920, 1080);
        let point = |width, height| RegionBlit {
            src_x: 0,
            src_y: 0,
            width,
            height,
            dst_x: 0,
            dst_y: 0,
        };
        // A zero-extent blit copies nothing and needs no read at all.
        assert_eq!(region_read_rect(point(0, 10), base, root, (10, 10)), None);
        assert_eq!(region_read_rect(point(10, 0), base, root, (10, 10)), None);
        // `GetImage` cannot encode a 65536-pixel extent nor a negative origin.
        let wide = Rect::new(0, 0, 70_000, 10);
        assert_eq!(
            region_read_rect(
                RegionBlit {
                    src_x: 0,
                    src_y: 0,
                    width: 65_536,
                    height: 10,
                    dst_x: 0,
                    dst_y: 0,
                },
                wide,
                Rect::new(0, 0, 70_000, 10),
                (70_000, 10)
            ),
            None
        );
        let far = RegionBlit {
            src_x: 10,
            src_y: 0,
            width: 10,
            height: 10,
            dst_x: 0,
            dst_y: 0,
        };
        // A frame origin near `i32::MAX` plus a source offset wraps; the answer
        // has to be `None` rather than a rectangle on the other side of zero.
        assert_eq!(
            region_read_rect(far, Rect::new(i32::MAX - 5, 0, 1920, 10), root, (10, 10)),
            None
        );
    }
}
