//! Pixel-format conversion for cursor bitmaps.

/// Convert a premultiplied ARGB32 buffer to straight-alpha RGBA8.
///
/// XFixes returns cursor bitmaps as premultiplied ARGB (alpha in the high
/// byte); the image pipeline composites them with `AlphaBlend`, which expects
/// straight alpha. `expected_pixels` bounds the read so a short or oversized
/// server reply can never produce a mis-sized buffer.
///
/// Returns `None` when the reply is too short to contain a full bitmap.
pub fn premultiplied_argb32_to_rgba8(source: &[u32], expected_pixels: usize) -> Option<Vec<u8>> {
    let source = source.get(..expected_pixels)?;
    let mut rgba = Vec::with_capacity(expected_pixels * 4);

    for &pixel in source {
        let alpha = (pixel >> 24) & 0xff;
        rgba.push(unpremultiply((pixel >> 16) & 0xff, alpha));
        rgba.push(unpremultiply((pixel >> 8) & 0xff, alpha));
        rgba.push(unpremultiply(pixel & 0xff, alpha));
        rgba.push(alpha as u8);
    }

    Some(rgba)
}

/// Undo premultiplication for one channel, rounding to nearest.
///
/// Fully transparent pixels report 0 rather than dividing by zero: with no
/// alpha contribution any colour is equivalent, and 0 keeps the buffer
/// deterministic so shape hashing stays stable.
fn unpremultiply(channel: u32, alpha: u32) -> u8 {
    if alpha == 0 {
        return 0;
    }
    ((channel * 255 + alpha / 2) / alpha).min(255) as u8
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn opaque_pixel_passes_through_unchanged() {
        let rgba = premultiplied_argb32_to_rgba8(&[0xff_11_22_33], 1).unwrap();
        assert_eq!(rgba, vec![0x11, 0x22, 0x33, 0xff]);
    }

    #[test]
    fn transparent_pixel_is_deterministic_zero() {
        let rgba = premultiplied_argb32_to_rgba8(&[0x00_ff_ff_ff], 1).unwrap();
        assert_eq!(rgba, vec![0, 0, 0, 0]);
    }

    #[test]
    fn half_alpha_is_unpremultiplied() {
        // Premultiplied 0x80 with alpha 0x80 -> straight 0xff.
        let rgba = premultiplied_argb32_to_rgba8(&[0x80_80_40_20], 1).unwrap();
        assert_eq!(rgba, vec![0xff, 0x80, 0x40, 0x80]);
    }

    #[test]
    fn short_reply_is_rejected_instead_of_panicking() {
        assert!(premultiplied_argb32_to_rgba8(&[0xff_00_00_00], 2).is_none());
        assert!(premultiplied_argb32_to_rgba8(&[], 0).is_some());
    }

    #[test]
    fn oversized_reply_is_truncated_to_the_expected_bitmap() {
        let rgba = premultiplied_argb32_to_rgba8(&[0xff_00_00_00, 0xff_ff_ff_ff], 1).unwrap();
        assert_eq!(rgba.len(), 4);
    }

    #[test]
    fn channel_conversion_saturates_instead_of_overflowing() {
        // Inconsistent input (channel > alpha) must clamp, not wrap.
        assert_eq!(unpremultiply(0xff, 1), 0xff);
    }
}
