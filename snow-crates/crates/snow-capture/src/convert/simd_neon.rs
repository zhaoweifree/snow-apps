//! NEON `BGRA` -> `RGBA` kernels for AArch64.
//!
//! # Why this is a small port and not a rewrite
//!
//! The x86 kernels are "apply a 16-entry byte table to every 16-byte block,
//! then optionally OR in an opaque alpha". ARM's `TBL` instruction is the same
//! operation with the same contract (indices outside `0..=15` read as zero, and
//! every index in [`BGRA_TO_RGBA_SHUFFLE`] is inside that range), so the kernels
//! here are structurally identical to `ssse3_bgra_core`: same unroll (four
//! 16-byte blocks per iteration), same four-pixel secondary loop, same scalar
//! tail. Both load [`BGRA_TO_RGBA_SHUFFLE`] itself -- not a copy of it -- so the
//! two architectures cannot drift apart in the byte mapping.
//!
//! # What NEON does *not* get
//!
//! * **Non-temporal stores.** AArch64 has `STNP`, but only for a register pair
//!   (16 bytes) and Rust exposes no intrinsic for it. The `_nt` entry points
//!   below therefore alias the regular kernel: same output, temporal stores.
//!   That is deliberately not a silent downgrade -- it is written down here,
//!   and the dispatch table keeps the same shape on both architectures so the
//!   callers do not sprout `cfg` branches.
//! * **A feature tier below NEON.** ARMv8-A mandates Advanced SIMD, so unlike
//!   x86 (AVX-512 / AVX2 / SSSE3 / scalar) there is exactly one vector tier.
//!   [`neon_available`] exists for symmetry with `is_x86_feature_detected!` and
//!   documents that it is always true on a conforming AArch64 target.
//!
//! The host this crate is developed on cannot execute AArch64 code, so these
//! kernels are compile-verified and table-verified (see `permute_bgra_block`)
//! but **not run-verified**. The channel-order tests in `convert::tests` are the
//! acceptance test once they run on UOS.

use super::scalar::{
    BGRA_TO_RGBA_SHUFFLE, convert_bgra_to_rgba_scalar_opaque_unchecked,
    convert_bgra_to_rgba_scalar_unchecked,
};

/// Whether the process can execute NEON instructions.
///
/// Always true for the targets this crate supports: Advanced SIMD is part of
/// the ARMv8-A baseline and `aarch64-unknown-linux-gnu` enables it in its
/// target spec, so there is no reduced-feature ARM to fall back to. Kept as a
/// function so the dispatch sites read the same as the x86 ones.
#[inline]
pub(crate) fn neon_available() -> bool {
    std::arch::is_aarch64_feature_detected!("neon")
}

#[target_feature(enable = "neon")]
pub(crate) unsafe fn convert_bgra_to_rgba_neon_unchecked(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
) {
    unsafe { neon_bgra_core::<false>(src, dst, pixel_count) }
}

#[target_feature(enable = "neon")]
pub(crate) unsafe fn convert_bgra_to_rgba_neon_opaque_unchecked(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
) {
    unsafe { neon_bgra_core::<true>(src, dst, pixel_count) }
}

/// Streaming-store variant. Aliases the regular kernel: see the module docs on
/// why AArch64 gets no non-temporal path.
#[target_feature(enable = "neon")]
pub(crate) unsafe fn convert_bgra_to_rgba_neon_nt_unchecked(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
) {
    unsafe { neon_bgra_core::<false>(src, dst, pixel_count) }
}

#[target_feature(enable = "neon")]
pub(crate) unsafe fn convert_bgra_to_rgba_neon_nt_opaque_unchecked(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
) {
    unsafe { neon_bgra_core::<true>(src, dst, pixel_count) }
}

#[target_feature(enable = "neon")]
pub(crate) unsafe fn convert_bgra_to_rgba_neon_nt_nofence_unchecked(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
) {
    unsafe { neon_bgra_core::<false>(src, dst, pixel_count) }
}

#[target_feature(enable = "neon")]
pub(crate) unsafe fn convert_bgra_to_rgba_neon_nt_nofence_opaque_unchecked(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
) {
    unsafe { neon_bgra_core::<true>(src, dst, pixel_count) }
}

/// Shared body for every entry point above.
///
/// `FORCE_OPAQUE_ALPHA` matches the x86 kernels: OR `0xFF` into each pixel's
/// high byte instead of carrying the source alpha through.
#[target_feature(enable = "neon")]
unsafe fn neon_bgra_core<const FORCE_OPAQUE_ALPHA: bool>(
    src: *const u8,
    dst: *mut u8,
    pixel_count: usize,
) {
    use std::arch::aarch64::{
        vdupq_n_u32, vld1q_u8, vorrq_u8, vqtbl1q_u8, vreinterpretq_u8_u32, vst1q_u8,
    };

    let table = unsafe { vld1q_u8(BGRA_TO_RGBA_SHUFFLE.as_ptr()) };
    // `0xFF00_0000` per 32-bit lane is alpha-in-the-high-byte on little-endian
    // AArch64, which is where RGBA puts it after the shuffle. The mask is built
    // as u32 lanes and reinterpreted, exactly like the x86 kernels' `set1_epi32`.
    //
    // Note the register-only intrinsics below (`vdupq_n_u32`, `vqtbl1q_u8`,
    // `vorrq_u8`, `vreinterpretq_u8_u32`) are *safe* in `core::arch::aarch64`:
    // Advanced SIMD is part of the AArch64 baseline, so unlike the x86 kernels
    // there is no call an unsafe precondition could break. Only the pointer
    // loads and stores need `unsafe`.
    let alpha_mask = if FORCE_OPAQUE_ALPHA {
        Some(vreinterpretq_u8_u32(vdupq_n_u32(0xFF00_0000)))
    } else {
        None
    };

    // A macro, not a helper `fn`: `#[target_feature]` cannot be combined with
    // `#[inline(always)]`, and a plain closure would not inherit `neon`.
    macro_rules! swizzle {
        ($block:expr) => {{
            let permuted = vqtbl1q_u8($block, table);
            match alpha_mask {
                Some(mask) => vorrq_u8(permuted, mask),
                None => permuted,
            }
        }};
    }

    let mut x = 0usize;

    // Four blocks per iteration, mirroring the SSSE3 kernel. `x86_64` unrolls
    // 64 bytes because its widest register is 64 bytes; NEON's widest is 16,
    // so four blocks is both the natural load width and the same amount of
    // work in flight.
    while x + 16 <= pixel_count {
        let offset = x * 4;
        unsafe {
            let block0 = vld1q_u8(src.add(offset));
            let block1 = vld1q_u8(src.add(offset + 16));
            let block2 = vld1q_u8(src.add(offset + 32));
            let block3 = vld1q_u8(src.add(offset + 48));

            let out0 = swizzle!(block0);
            let out1 = swizzle!(block1);
            let out2 = swizzle!(block2);
            let out3 = swizzle!(block3);

            vst1q_u8(dst.add(offset), out0);
            vst1q_u8(dst.add(offset + 16), out1);
            vst1q_u8(dst.add(offset + 32), out2);
            vst1q_u8(dst.add(offset + 48), out3);
        }
        x += 16;
    }

    while x + 4 <= pixel_count {
        let offset = x * 4;
        unsafe {
            let block = vld1q_u8(src.add(offset));
            vst1q_u8(dst.add(offset), swizzle!(block));
        }
        x += 4;
    }

    if x < pixel_count {
        unsafe {
            if FORCE_OPAQUE_ALPHA {
                convert_bgra_to_rgba_scalar_opaque_unchecked(
                    src.add(x * 4),
                    dst.add(x * 4),
                    pixel_count - x,
                );
            } else {
                convert_bgra_to_rgba_scalar_unchecked(
                    src.add(x * 4),
                    dst.add(x * 4),
                    pixel_count - x,
                );
            }
        }
    }
}
