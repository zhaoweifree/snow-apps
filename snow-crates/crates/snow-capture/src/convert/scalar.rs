/// Byte permutation that turns four little-endian `BGRA` pixels into four
/// little-endian `RGBA` pixels: within each pixel swap byte 0 with byte 2 and
/// leave green and alpha where they are.
///
/// Every SIMD kernel in this crate is "apply this table to each 16-byte block,
/// then optionally OR in an opaque alpha", so the table lives here once instead
/// of being spelled out per ISA. A divergence would show up as a red/blue
/// swapped capture that only reproduces on one architecture -- the kind of bug
/// that costs a day on the target machine.
///
/// Only the x86 SSSE3 kernel executes a table lookup; AArch64 goes through
/// `simd_arm`, which gathers red, green and blue with explicit lane maths and
/// never reads this. The gate is therefore `x86_64` plus `test`, where the
/// table-free reference below still walks it.
#[cfg(any(target_arch = "x86_64", test))]
pub(crate) const BGRA_TO_RGBA_SHUFFLE: [u8; 16] =
    [2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15];

/// Table-free reference for a single 16-byte block: exactly what the SIMD
/// table lookup does, one byte at a time.
///
/// Tests compare this against [`swap_bgra_to_rgba`], which is what makes the
/// shared table trustworthy for the NEON kernel that an x86 host cannot
/// execute. Test-only on purpose: it must never end up on a hot path, because
/// it is far slower than both the scalar and the SIMD kernels.
#[cfg(test)]
pub(crate) fn permute_bgra_block(block: &[u8; 16]) -> [u8; 16] {
    let mut out = [0u8; 16];
    for (lane, source) in BGRA_TO_RGBA_SHUFFLE.iter().enumerate() {
        out[lane] = block[*source as usize];
    }
    out
}

#[inline(always)]
pub(crate) fn swap_bgra_to_rgba(pixel: u32) -> u32 {
    ((pixel & 0x0000_00FF) << 16)
        | (pixel & 0x0000_FF00)
        | ((pixel & 0x00FF_0000) >> 16)
        | (pixel & 0xFF00_0000)
}

#[inline(always)]
pub(crate) fn swap_bgra_to_rgba_opaque(pixel: u32) -> u32 {
    swap_bgra_to_rgba(pixel) | 0xFF00_0000
}

#[inline(always)]
unsafe fn convert_bgra_to_rgba_scalar_impl<const FORCE_OPAQUE_ALPHA: bool>(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
) {
    let mut src_px = src as *const u32;
    let mut dst_px = dst as *mut u32;
    let mut remaining = pixel_count;

    while remaining >= 8 {
        unsafe {
            let p0 = std::ptr::read_unaligned(src_px);
            let p1 = std::ptr::read_unaligned(src_px.add(1));
            let p2 = std::ptr::read_unaligned(src_px.add(2));
            let p3 = std::ptr::read_unaligned(src_px.add(3));
            let p4 = std::ptr::read_unaligned(src_px.add(4));
            let p5 = std::ptr::read_unaligned(src_px.add(5));
            let p6 = std::ptr::read_unaligned(src_px.add(6));
            let p7 = std::ptr::read_unaligned(src_px.add(7));

            std::ptr::write_unaligned(
                dst_px,
                if FORCE_OPAQUE_ALPHA {
                    swap_bgra_to_rgba_opaque(p0)
                } else {
                    swap_bgra_to_rgba(p0)
                },
            );
            std::ptr::write_unaligned(
                dst_px.add(1),
                if FORCE_OPAQUE_ALPHA {
                    swap_bgra_to_rgba_opaque(p1)
                } else {
                    swap_bgra_to_rgba(p1)
                },
            );
            std::ptr::write_unaligned(
                dst_px.add(2),
                if FORCE_OPAQUE_ALPHA {
                    swap_bgra_to_rgba_opaque(p2)
                } else {
                    swap_bgra_to_rgba(p2)
                },
            );
            std::ptr::write_unaligned(
                dst_px.add(3),
                if FORCE_OPAQUE_ALPHA {
                    swap_bgra_to_rgba_opaque(p3)
                } else {
                    swap_bgra_to_rgba(p3)
                },
            );
            std::ptr::write_unaligned(
                dst_px.add(4),
                if FORCE_OPAQUE_ALPHA {
                    swap_bgra_to_rgba_opaque(p4)
                } else {
                    swap_bgra_to_rgba(p4)
                },
            );
            std::ptr::write_unaligned(
                dst_px.add(5),
                if FORCE_OPAQUE_ALPHA {
                    swap_bgra_to_rgba_opaque(p5)
                } else {
                    swap_bgra_to_rgba(p5)
                },
            );
            std::ptr::write_unaligned(
                dst_px.add(6),
                if FORCE_OPAQUE_ALPHA {
                    swap_bgra_to_rgba_opaque(p6)
                } else {
                    swap_bgra_to_rgba(p6)
                },
            );
            std::ptr::write_unaligned(
                dst_px.add(7),
                if FORCE_OPAQUE_ALPHA {
                    swap_bgra_to_rgba_opaque(p7)
                } else {
                    swap_bgra_to_rgba(p7)
                },
            );
        }

        src_px = unsafe { src_px.add(8) };
        dst_px = unsafe { dst_px.add(8) };
        remaining -= 8;
    }

    while remaining != 0 {
        unsafe {
            let pixel = std::ptr::read_unaligned(src_px);
            std::ptr::write_unaligned(
                dst_px,
                if FORCE_OPAQUE_ALPHA {
                    swap_bgra_to_rgba_opaque(pixel)
                } else {
                    swap_bgra_to_rgba(pixel)
                },
            );
        }

        src_px = unsafe { src_px.add(1) };
        dst_px = unsafe { dst_px.add(1) };
        remaining -= 1;
    }
}

pub(crate) unsafe fn convert_bgra_to_rgba_scalar_unchecked(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
) {
    unsafe {
        convert_bgra_to_rgba_scalar_impl::<false>(src, dst, pixel_count);
    }
}

pub(crate) unsafe fn convert_bgra_to_rgba_scalar_opaque_unchecked(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
) {
    unsafe {
        convert_bgra_to_rgba_scalar_impl::<true>(src, dst, pixel_count);
    }
}
