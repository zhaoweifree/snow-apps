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
    if bits >= 32 { u32::MAX } else { (1u32 << bits) - 1 }
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
        let required = self.required_len();
        if source.len() < required {
            return Err(LinuxError::other(format!(
                "the GetImage reply holds {} bytes, {required} are needed for {}x{}",
                source.len(),
                self.width,
                self.height
            )));
        }
        let expected = self.output_len();
        if destination.len() != expected {
            return Err(LinuxError::other(format!(
                "the destination holds {} bytes, {expected} are needed for {}x{}",
                destination.len(),
                self.width,
                self.height
            )));
        }

        if self.is_native_bgrx() {
            return self.decode_native_bgrx(source, destination, order);
        }
        self.decode_generic(source, destination, order)
    }

    /// Mask-driven reference decode, used for every layout the fast path does
    /// not cover. This is the definition of "correct" for the decoder, so the
    /// tests compare the fast path's output against it.
    fn decode_generic(
        &self,
        source: &[u8],
        destination: &mut [u8],
        order: OutputOrder,
    ) -> LinuxResult<()> {
        let pixel_bytes = self.pixel_bytes();
        let stride = self.bytes_per_line as usize;
        for (row, out_row) in destination
            .chunks_exact_mut(self.width as usize * 4)
            .enumerate()
        {
            let source_row = &source[row * stride..];
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
        Ok(())
    }

    /// 32-bit `B,G,R,X` source rows, copied (and optionally reordered) as a
    /// unit instead of pixel by pixel.
    fn decode_native_bgrx(
        &self,
        source: &[u8],
        destination: &mut [u8],
        order: OutputOrder,
    ) -> LinuxResult<()> {
        let row_bytes = self.width as usize * 4;
        let stride = self.bytes_per_line as usize;
        for (row, out_row) in destination.chunks_exact_mut(row_bytes).enumerate() {
            out_row.copy_from_slice(&source[row * stride..row * stride + row_bytes]);
            if order == OutputOrder::Rgba {
                for pixel in out_row.chunks_exact_mut(4) {
                    pixel.swap(0, 2);
                }
            }
            // X11 leaves the padding byte undefined (commonly zero, sometimes
            // stale), so opacity is asserted rather than forwarded.
            for pixel in out_row.chunks_exact_mut(4) {
                pixel[3] = 0xff;
            }
        }
        Ok(())
    }
}

/// Assemble one pixel from its packed bytes in the server's byte order.
fn read_pixel(bytes: &[u8], lsb_first: bool) -> u32 {
    if lsb_first {
        bytes
            .iter()
            .enumerate()
            .fold(0u32, |acc, (index, byte)| acc | (u32::from(*byte) << (index * 8)))
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

#[cfg(test)]
mod tests {
    use super::*;

    /// The overwhelmingly common X11 root: depth 24 visual, 32 bits per pixel,
    /// little-endian, `0x00RRGGBB`.
    fn native_layout(width: u32, height: u32) -> ImageLayout {
        let stride = scanline_bytes(width, 32, 32);
        ImageLayout::new(width, height, 32, stride, true, 0x00ff_0000, 0x0000_ff00, 0x0000_00ff)
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
        assert!(layout.decode_into(&[0u8; 4], &mut out, OutputOrder::Bgra).is_err());
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
        let bgr = ImageLayout::new(1, 1, 32, 4, true, 0x0000_00ff, 0x0000_ff00, 0x00ff_0000)
            .unwrap();
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
            layout.decode_generic(&source, &mut reference, order).unwrap();

            assert_eq!(fast, reference, "fast path diverged for {order:?}");
            // Opacity is asserted, never forwarded from the padding byte.
            assert!(fast.chunks_exact(4).all(|pixel| pixel[3] == 0xff));
        }
    }
}
