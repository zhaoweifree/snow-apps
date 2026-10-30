//! AArch64 baseline Advanced SIMD kernels. No optional FP16 extension is required.
use super::f16::{
    EPSILON, HDR_INPUT_BLACK_NITS, PQ_ABSOLUTE_NITS, PQ_C1, PQ_C2, PQ_C3, PQ_M1, PQ_M2,
    SDR_REFERENCE_WHITE_NITS,
};
use super::{HdrPreparedContext, f16, scalar};
use crate::{CapturePixelFormat, color_effect::ScreenColorTransform};
use std::arch::aarch64::*;

#[inline]
unsafe fn half_to_float(value: uint16x4_t) -> float32x4_t {
    unsafe {
        let bits = vmovl_u16(value);
        let magnitude = vandq_u32(bits, vdupq_n_u32(0x7fff));
        let exponent = vandq_u32(magnitude, vdupq_n_u32(0x7c00));
        let mantissa = vandq_u32(magnitude, vdupq_n_u32(0x3ff));
        let normal = vreinterpretq_f32_u32(vaddq_u32(
            vshlq_n_u32::<13>(magnitude),
            vdupq_n_u32(0x3800_0000),
        ));
        let subnormal = vmulq_n_f32(vcvtq_f32_u32(mantissa), 1.0 / 16_777_216.0);
        let special = vorrq_u32(vshlq_n_u32::<13>(mantissa), vdupq_n_u32(0x7f80_0000));
        // half::f16::to_f32 quiets signaling NaNs. Preserve that behavior before
        // floating operations: ARM's numeric min/max also quiets signaling NaNs
        // but only on the following operation chooses the numeric operand.
        let special = vreinterpretq_f32_u32(vbslq_u32(
            vcgtq_u32(mantissa, vdupq_n_u32(0)),
            vorrq_u32(special, vdupq_n_u32(0x0040_0000)),
            special,
        ));
        let value = vbslq_f32(vceqq_u32(exponent, vdupq_n_u32(0)), subnormal, normal);
        let value = vbslq_f32(vceqq_u32(exponent, vdupq_n_u32(0x7c00)), special, value);
        vreinterpretq_f32_u32(vorrq_u32(
            vreinterpretq_u32_f32(value),
            vshlq_n_u32::<16>(vandq_u32(bits, vdupq_n_u32(0x8000))),
        ))
    }
}

#[inline]
unsafe fn clamp(value: float32x4_t, minimum: f32, maximum: f32) -> float32x4_t {
    unsafe {
        vminnmq_f32(
            vmaxnmq_f32(value, vdupq_n_f32(minimum)),
            vdupq_n_f32(maximum),
        )
    }
}

#[inline]
unsafe fn pack_bytes(channels: [uint32x4_t; 4]) -> uint32x4_t {
    unsafe {
        vorrq_u32(
            vorrq_u32(channels[0], vshlq_n_u32::<8>(channels[1])),
            vorrq_u32(
                vshlq_n_u32::<16>(channels[2]),
                vshlq_n_u32::<24>(channels[3]),
            ),
        )
    }
}

pub(crate) unsafe fn convert_bgra(src: *const u8, dst: *mut u8, count: usize) {
    unsafe { convert_bgra_impl::<false>(src, dst, count) }
}

pub(crate) unsafe fn convert_bgra_opaque(src: *const u8, dst: *mut u8, count: usize) {
    unsafe { convert_bgra_impl::<true>(src, dst, count) }
}

unsafe fn convert_bgra_impl<const OPAQUE: bool>(src: *const u8, dst: *mut u8, count: usize) {
    unsafe {
        let shuffle = vld1q_u8([2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15].as_ptr());
        let mut index = 0;
        while index + 4 <= count {
            let pixels = vqtbl1q_u8(vld1q_u8(src.add(index * 4)), shuffle);
            let pixels = if OPAQUE {
                vreinterpretq_u8_u32(vorrq_u32(
                    vreinterpretq_u32_u8(pixels),
                    vdupq_n_u32(0xff00_0000),
                ))
            } else {
                pixels
            };
            vst1q_u8(dst.add(index * 4), pixels);
            index += 4;
        }
        if OPAQUE {
            scalar::convert_bgra_to_rgba_scalar_opaque_unchecked(
                src.add(index * 4),
                dst.add(index * 4),
                count - index,
            );
        } else {
            scalar::convert_bgra_to_rgba_scalar_unchecked(
                src.add(index * 4),
                dst.add(index * 4),
                count - index,
            );
        }
    }
}

#[inline]
unsafe fn load_rgb4(src: *const u8, lut: *const u8) -> uint32x4_t {
    unsafe {
        // Read whole pixels directly into integer registers. Deinterleaving the
        // half words first only adds lane-extract traffic to these scalar LUT
        // gathers on NEON, which has no gather instruction.
        let pixel = |index: usize| {
            let bits = std::ptr::read_unaligned(src.add(index * 8).cast::<u64>());
            u32::from(*lut.add((bits & 0xffff) as usize))
                | (u32::from(*lut.add(((bits >> 16) & 0xffff) as usize)) << 8)
                | (u32::from(*lut.add(((bits >> 32) & 0xffff) as usize)) << 16)
        };
        let first = pixel(0);
        let second = pixel(1);
        let third = pixel(2);
        let fourth = pixel(3);
        let value = vdupq_n_u32(first);
        let value = vsetq_lane_u32::<1>(second, value);
        let value = vsetq_lane_u32::<2>(third, value);
        vsetq_lane_u32::<3>(fourth, value)
    }
}

#[inline]
unsafe fn alpha_bytes4(src: *const u8) -> uint32x4_t {
    unsafe {
        let first = vshrq_n_u64::<48>(vreinterpretq_u64_u8(vld1q_u8(src)));
        let second = vshrq_n_u64::<48>(vreinterpretq_u64_u8(vld1q_u8(src.add(16))));
        let bits = vcombine_u32(vmovn_u64(first), vmovn_u64(second));
        let magnitude = vandq_u32(bits, vdupq_n_u32(0x7fff));
        let valid = vandq_u32(
            vcltq_u32(bits, vdupq_n_u32(0x8000)),
            vcleq_u32(magnitude, vdupq_n_u32(0x7c00)),
        );
        let magnitude = vminq_u32(magnitude, vdupq_n_u32(0x3c00));
        let significand = vorrq_u32(vandq_u32(magnitude, vdupq_n_u32(0x3ff)), vdupq_n_u32(0x400));
        let shift = vsubq_s32(
            vreinterpretq_s32_u32(vshrq_n_u32::<10>(magnitude)),
            vdupq_n_s32(25),
        );
        // The half significand times 255 is exact in u32. Rounded variable
        // right shifts match (clamp(half, 0, 1)*255 + 0.5) as u8 exactly.
        // All subnormals round to zero; negatives and NaNs are masked to zero.
        vandq_u32(vrshlq_u32(vmulq_n_u32(significand, 255), shift), valid)
    }
}

pub(crate) unsafe fn convert_f16(src: *const u8, dst: *mut u8, count: usize) {
    unsafe { convert_f16_impl::<16>(src, dst, count) }
}

pub(crate) unsafe fn convert_f16_opaque(src: *const u8, dst: *mut u8, count: usize) {
    unsafe { convert_f16_opaque_impl(src, dst, count) }
}

unsafe fn convert_f16_impl<const BATCH: usize>(src: *const u8, dst: *mut u8, count: usize) {
    unsafe {
        let lut = f16::f16_to_srgb_lut().as_ptr();
        let mut index = 0;
        while index + BATCH <= count {
            for group in 0..BATCH / 4 {
                let offset = index + group * 4;
                let rgb = load_rgb4(src.add(offset * 8), lut);
                let alpha = vshlq_n_u32::<24>(alpha_bytes4(src.add(offset * 8)));
                vst1q_u8(
                    dst.add(offset * 4),
                    vreinterpretq_u8_u32(vorrq_u32(rgb, alpha)),
                );
            }
            index += BATCH;
        }
        while index + 4 <= count {
            let rgb = load_rgb4(src.add(index * 8), lut);
            let alpha = vshlq_n_u32::<24>(alpha_bytes4(src.add(index * 8)));
            vst1q_u8(
                dst.add(index * 4),
                vreinterpretq_u8_u32(vorrq_u32(rgb, alpha)),
            );
            index += 4;
        }
        f16::convert_f16_rgba_to_srgb_scalar_unchecked(
            src.add(index * 8),
            dst.add(index * 4),
            count - index,
        );
    }
}

// Exact scalar gathers are unavoidable on baseline NEON. Four independent
// pixels before a bulk store avoid lane-insert chains and the spills of wider
// unrolled blocks. Native Release measurements select this opaque specialization.
unsafe fn convert_f16_opaque_impl(src: *const u8, dst: *mut u8, count: usize) {
    unsafe {
        let lut = f16::f16_to_srgb_lut().as_ptr();
        let mut index = 0;
        while index + 4 <= count {
            let pixels: [u32; 4] = std::array::from_fn(|lane| {
                let bits = std::ptr::read_unaligned(src.add((index + lane) * 8).cast::<u64>());
                u32::from(*lut.add((bits & 0xffff) as usize))
                    | (u32::from(*lut.add(((bits >> 16) & 0xffff) as usize)) << 8)
                    | (u32::from(*lut.add(((bits >> 32) & 0xffff) as usize)) << 16)
                    | 0xff00_0000
            });
            std::ptr::copy_nonoverlapping(pixels.as_ptr().cast::<u8>(), dst.add(index * 4), 16);
            index += 4;
        }
        f16::convert_f16_rgba_to_srgb_scalar_opaque_unchecked(
            src.add(index * 8),
            dst.add(index * 4),
            count - index,
        );
    }
}

// Same bounded log/exp range reduction as the x64 HDR kernel. Separate multiply
// and add operations retain its non-FMA rounding and <=1 encoded-byte budget.
#[inline]
unsafe fn pow(value: float32x4_t, exponent: f32) -> float32x4_t {
    unsafe {
        let one = vdupq_n_f32(1.0);
        let bits = vreinterpretq_u32_f32(clamp(value, f32::MIN_POSITIVE, f32::MAX));
        let e = vsubq_s32(
            vreinterpretq_s32_u32(vshrq_n_u32::<23>(bits)),
            vdupq_n_s32(127),
        );
        let m = vreinterpretq_f32_u32(vorrq_u32(
            vandq_u32(bits, vdupq_n_u32(0x7fffff)),
            vdupq_n_u32(0x3f80_0000),
        ));
        let adjust = vcgtq_f32(m, vdupq_n_f32(std::f32::consts::SQRT_2));
        let m = vbslq_f32(adjust, vmulq_n_f32(m, 0.5), m);
        let e = vcvtq_f32_s32(vaddq_s32(
            e,
            vreinterpretq_s32_u32(vandq_u32(adjust, vdupq_n_u32(1))),
        ));
        let z = vdivq_f32(vsubq_f32(m, one), vaddq_f32(m, one));
        let z2 = vmulq_f32(z, z);
        let mut polynomial = vdupq_n_f32(1.0 / 9.0);
        for coefficient in [1.0 / 7.0, 1.0 / 5.0, 1.0 / 3.0, 1.0] {
            polynomial = vaddq_f32(vmulq_f32(polynomial, z2), vdupq_n_f32(coefficient));
        }
        let log2 = vaddq_f32(
            e,
            vmulq_n_f32(vmulq_f32(z, polynomial), 2.0 * std::f32::consts::LOG2_E),
        );
        let power = clamp(vmulq_n_f32(log2, exponent), -126.0, 127.0);
        let integer = vrndnq_f32(power);
        let fraction = vmulq_n_f32(vsubq_f32(power, integer), std::f32::consts::LN_2);
        let mut exponential = vdupq_n_f32(1.0 / 5040.0);
        for coefficient in [
            1.0 / 720.0,
            1.0 / 120.0,
            1.0 / 24.0,
            1.0 / 6.0,
            0.5,
            1.0,
            1.0,
        ] {
            exponential = vaddq_f32(vmulq_f32(exponential, fraction), vdupq_n_f32(coefficient));
        }
        let scale = vreinterpretq_f32_s32(vshlq_n_s32::<23>(vaddq_s32(
            vcvtq_s32_f32(integer),
            vdupq_n_s32(127),
        )));
        let result = vmulq_f32(exponential, scale);
        let result = vbslq_f32(vceqq_f32(value, vdupq_n_f32(f32::INFINITY)), value, result);
        vreinterpretq_f32_u32(vandq_u32(
            vreinterpretq_u32_f32(result),
            vcgtq_f32(value, vdupq_n_f32(0.0)),
        ))
    }
}

#[inline]
unsafe fn encode_srgb(value: float32x4_t) -> uint32x4_t {
    unsafe {
        let value = clamp(value, 0.0, 1.0);
        let nonlinear = vsubq_f32(
            vmulq_n_f32(pow(value, 1.0 / 2.4), 1.055),
            vdupq_n_f32(0.055),
        );
        let encoded = vbslq_f32(
            vcleq_f32(value, vdupq_n_f32(0.003_130_8)),
            vmulq_n_f32(value, 12.92),
            nonlinear,
        );
        vcvtq_u32_f32(clamp(
            vaddq_f32(vmulq_n_f32(encoded, 255.0), vdupq_n_f32(0.5)),
            0.0,
            255.0,
        ))
    }
}

#[inline]
unsafe fn matrix(rgb: [float32x4_t; 3], rows: [[f32; 4]; 3]) -> [float32x4_t; 3] {
    unsafe {
        rows.map(|row| {
            vaddq_f32(
                vaddq_f32(
                    vaddq_f32(vmulq_n_f32(rgb[0], row[0]), vmulq_n_f32(rgb[1], row[1])),
                    vmulq_n_f32(rgb[2], row[2]),
                ),
                vdupq_n_f32(row[3]),
            )
        })
    }
}

#[inline]
unsafe fn map_luma(value: float32x4_t, prepared: &HdrPreparedContext) -> float32x4_t {
    unsafe {
        if let Some(lut) = &prepared.luma_lut {
            let position = vmulq_n_f32(clamp(value, 0.0, lut.input_max()), lut.inv_step());
            let indices = vminq_u32(
                vcvtq_u32_f32(position),
                vdupq_n_u32((f16::HDR_LUMA_LUT_SIZE - 1) as u32),
            );
            let mut lanes = [0; 4];
            vst1q_u32(lanes.as_mut_ptr(), indices);
            let low = lanes.map(|index| *lut.values_ptr().add(index as usize));
            let high = lanes.map(|index| {
                *lut.values_ptr()
                    .add((index as usize + 1).min(f16::HDR_LUMA_LUT_SIZE - 1))
            });
            let low = vld1q_f32(low.as_ptr());
            let high = vld1q_f32(high.as_ptr());
            return vaddq_f32(
                low,
                vmulq_f32(
                    vsubq_f32(high, low),
                    vsubq_f32(position, vcvtq_f32_u32(indices)),
                ),
            );
        }
        let curve = prepared.curve;
        let p = pow(
            vmulq_n_f32(
                vmaxnmq_f32(
                    vmulq_n_f32(value, SDR_REFERENCE_WHITE_NITS),
                    vdupq_n_f32(HDR_INPUT_BLACK_NITS),
                ),
                1.0 / PQ_ABSOLUTE_NITS,
            ),
            PQ_M1,
        );
        let x = pow(
            vdivq_f32(
                vaddq_f32(vdupq_n_f32(PQ_C1), vmulq_n_f32(p, PQ_C2)),
                vaddq_f32(vdupq_n_f32(1.0), vmulq_n_f32(p, PQ_C3)),
            ),
            PQ_M2,
        );
        let mut y = vdivq_f32(
            vsubq_f32(x, vdupq_n_f32(curve.ib)),
            vdupq_n_f32(curve.denom),
        );
        let t = vdivq_f32(
            vsubq_f32(y, vdupq_n_f32(curve.ks)),
            vdupq_n_f32((1.0 - curve.ks).max(EPSILON)),
        );
        let t2 = vmulq_f32(t, t);
        let t3 = vmulq_f32(t2, t);
        let polynomial = vaddq_f32(
            vaddq_f32(
                vmulq_n_f32(
                    vaddq_f32(
                        vsubq_f32(vmulq_n_f32(t3, 2.0), vmulq_n_f32(t2, 3.0)),
                        vdupq_n_f32(1.0),
                    ),
                    curve.ks,
                ),
                vmulq_n_f32(
                    vaddq_f32(vsubq_f32(t3, vmulq_n_f32(t2, 2.0)), t),
                    1.0 - curve.ks,
                ),
            ),
            vmulq_n_f32(
                vaddq_f32(vmulq_n_f32(t3, -2.0), vmulq_n_f32(t2, 3.0)),
                curve.max_lum,
            ),
        );
        y = vbslq_f32(vcgeq_f32(y, vdupq_n_f32(curve.ks)), polynomial, y);
        let one_minus = vmaxnmq_f32(vsubq_f32(vdupq_n_f32(1.0), y), vdupq_n_f32(0.0));
        let squared = vmulq_f32(one_minus, one_minus);
        let black = vaddq_f32(y, vmulq_n_f32(vmulq_f32(squared, squared), curve.min_lum));
        y = vbslq_f32(vcgeq_f32(y, vdupq_n_f32(0.0)), black, y);
        let mapped = clamp(
            vaddq_f32(vmulq_n_f32(y, curve.denom), vdupq_n_f32(curve.ib)),
            curve.ob,
            curve.ow,
        );
        let p = pow(mapped, 1.0 / PQ_M2);
        let numerator = vmaxnmq_f32(vsubq_f32(p, vdupq_n_f32(PQ_C1)), vdupq_n_f32(0.0));
        let denominator = vmaxnmq_f32(
            vsubq_f32(vdupq_n_f32(PQ_C2), vmulq_n_f32(p, PQ_C3)),
            vdupq_n_f32(EPSILON),
        );
        vmaxnmq_f32(
            vmulq_n_f32(
                pow(vdivq_f32(numerator, denominator), 1.0 / PQ_M1),
                PQ_ABSOLUTE_NITS / SDR_REFERENCE_WHITE_NITS,
            ),
            vdupq_n_f32(0.0),
        )
    }
}

pub(crate) unsafe fn convert_hdr(
    src: *const u8,
    dst: *mut u8,
    count: usize,
    prepared: &HdrPreparedContext,
) {
    unsafe { convert_hdr_impl::<false>(src, dst, count, prepared) }
}

pub(crate) unsafe fn convert_hdr_opaque(
    src: *const u8,
    dst: *mut u8,
    count: usize,
    prepared: &HdrPreparedContext,
) {
    unsafe { convert_hdr_impl::<true>(src, dst, count, prepared) }
}

unsafe fn convert_hdr_impl<const OPAQUE: bool>(
    src: *const u8,
    dst: *mut u8,
    count: usize,
    prepared: &HdrPreparedContext,
) {
    unsafe {
        let zero = vdupq_n_f32(0.0);
        let one = vdupq_n_f32(1.0);
        let mut index = 0;
        while index + 4 <= count {
            let channels = vld4_u16(src.add(index * 8).cast());
            let mut rgb = [
                half_to_float(channels.0),
                half_to_float(channels.1),
                half_to_float(channels.2),
            ];
            if let Some(rows) = prepared.screen_color_rows {
                rgb = matrix(rgb, rows);
            }
            rgb = rgb.map(|channel| vmulq_n_f32(vmaxnmq_f32(channel, zero), prepared.inv_boost));
            let maximum = vmaxnmq_f32(rgb[0], vmaxnmq_f32(rgb[1], rgb[2]));
            if vminvq_u32(vcleq_f32(
                maximum,
                vdupq_n_f32(f16::HDR_SDR_TRANSITION_START),
            )) == 0
            {
                let luma = vmaxnmq_f32(
                    vaddq_f32(
                        vaddq_f32(vmulq_n_f32(rgb[0], 0.2126), vmulq_n_f32(rgb[1], 0.7152)),
                        vmulq_n_f32(rgb[2], 0.0722),
                    ),
                    zero,
                );
                let output_luma = vminnmq_f32(map_luma(luma, prepared), one);
                let denominator = vmaxnmq_f32(
                    vmaxnmq_f32(luma, vdupq_n_f32(EPSILON)),
                    vmulq_f32(maximum, output_luma),
                );
                let scale = vbslq_f32(
                    vcleq_f32(luma, vdupq_n_f32(EPSILON)),
                    zero,
                    vdivq_f32(output_luma, denominator),
                );
                let t = clamp(
                    vmulq_n_f32(
                        vsubq_f32(maximum, vdupq_n_f32(f16::HDR_SDR_TRANSITION_START)),
                        f16::HDR_SDR_TRANSITION_INV_WIDTH,
                    ),
                    0.0,
                    1.0,
                );
                let weight = vmulq_f32(
                    vmulq_f32(t, t),
                    vsubq_f32(vdupq_n_f32(3.0), vmulq_n_f32(t, 2.0)),
                );
                rgb = rgb.map(|channel| {
                    let hdr = vmulq_f32(channel, scale);
                    let blend = vaddq_f32(channel, vmulq_f32(vsubq_f32(hdr, channel), weight));
                    vbslq_f32(
                        vcgeq_f32(t, one),
                        hdr,
                        vbslq_f32(vcleq_f32(t, zero), channel, blend),
                    )
                });
            }
            let mut encoded = rgb.map(|channel| encode_srgb(channel));
            if prepared.output_pixel_format == CapturePixelFormat::Bgra8 {
                encoded.swap(0, 2);
            }
            let alpha = if OPAQUE {
                vdupq_n_u32(255)
            } else {
                vcvtq_u32_f32(vaddq_f32(
                    vmulq_n_f32(clamp(half_to_float(channels.3), 0.0, 1.0), 255.0),
                    vdupq_n_f32(0.5),
                ))
            };
            vst1q_u8(
                dst.add(index * 4),
                vreinterpretq_u8_u32(pack_bytes([encoded[0], encoded[1], encoded[2], alpha])),
            );
            index += 4;
        }
        if OPAQUE {
            f16::convert_f16_rgba_to_srgb_hdr_scalar_prepared_opaque_unchecked(
                src.add(index * 8),
                dst.add(index * 4),
                count - index,
                prepared,
            );
        } else {
            f16::convert_f16_rgba_to_srgb_hdr_scalar_prepared_unchecked(
                src.add(index * 8),
                dst.add(index * 4),
                count - index,
                prepared,
            );
        }
    }
}

/// Return the number of pixels converted, leaving the scalar tail to the caller.
pub(super) unsafe fn convert_color_row(
    src: *const u8,
    dst: *mut u8,
    count: usize,
    source_bgra: bool,
    target_bgra: bool,
    opaque: bool,
    transform: ScreenColorTransform,
) -> usize {
    unsafe {
        let mut index = 0;
        while index + 4 <= count {
            let pixels = vreinterpretq_u32_u8(vld1q_u8(src.add(index * 4)));
            let mut output;
            if transform.inverted {
                output = veorq_u32(pixels, vdupq_n_u32(0x00ff_ffff));
                if source_bgra != target_bgra {
                    output = vorrq_u32(
                        vandq_u32(output, vdupq_n_u32(0xff00_ff00)),
                        vorrq_u32(
                            vshlq_n_u32::<16>(vandq_u32(output, vdupq_n_u32(255))),
                            vandq_u32(vshrq_n_u32::<16>(output), vdupq_n_u32(255)),
                        ),
                    );
                }
                if opaque {
                    output = vorrq_u32(output, vdupq_n_u32(0xff00_0000));
                }
            } else {
                let low = vcvtq_f32_u32(vandq_u32(pixels, vdupq_n_u32(255)));
                let green = vcvtq_f32_u32(vandq_u32(vshrq_n_u32::<8>(pixels), vdupq_n_u32(255)));
                let high = vcvtq_f32_u32(vandq_u32(vshrq_n_u32::<16>(pixels), vdupq_n_u32(255)));
                let rgb = if source_bgra {
                    [high, green, low]
                } else {
                    [low, green, high]
                };
                let mut encoded = matrix(rgb, transform.rows).map(|channel| {
                    vcvtq_u32_f32(vaddq_f32(clamp(channel, 0.0, 255.0), vdupq_n_f32(0.5)))
                });
                if target_bgra {
                    encoded.swap(0, 2);
                }
                let alpha = if opaque {
                    vdupq_n_u32(255)
                } else {
                    vshrq_n_u32::<24>(pixels)
                };
                output = pack_bytes([encoded[0], encoded[1], encoded[2], alpha]);
            }
            vst1q_u8(dst.add(index * 4), vreinterpretq_u8_u32(output));
            index += 4;
        }
        index
    }
}

#[cfg(any(windows, test))]
pub(crate) unsafe fn row_equal(lhs: *const u8, rhs: *const u8, len: usize) -> bool {
    unsafe {
        // Sparse UI damage often appears at the right edge of a long row.
        // Match the x64 scanner's early tail/middle probes before its full scan.
        if len >= 16
            && vmaxvq_u8(veorq_u8(
                vld1q_u8(lhs.add(len - 16)),
                vld1q_u8(rhs.add(len - 16)),
            )) != 0
        {
            return false;
        }
        if len >= 64 {
            let middle = (len / 2) & !15;
            if vmaxvq_u8(veorq_u8(
                vld1q_u8(lhs.add(middle)),
                vld1q_u8(rhs.add(middle)),
            )) != 0
            {
                return false;
            }
        }
        let mut offset = 0;
        while offset + 64 <= len {
            let difference = |index| veorq_u8(vld1q_u8(lhs.add(index)), vld1q_u8(rhs.add(index)));
            let first = vorrq_u8(difference(offset), difference(offset + 16));
            let second = vorrq_u8(difference(offset + 32), difference(offset + 48));
            if vmaxvq_u8(vorrq_u8(first, second)) != 0 {
                return false;
            }
            offset += 64;
        }
        while offset + 16 <= len {
            if vmaxvq_u8(veorq_u8(
                vld1q_u8(lhs.add(offset)),
                vld1q_u8(rhs.add(offset)),
            )) != 0
            {
                return false;
            }
            offset += 16;
        }
        (offset..len).all(|index| *lhs.add(index) == *rhs.add(index))
    }
}

#[cfg(any(windows, test))]
pub(crate) unsafe fn row_diff_bounds(
    lhs: *const u8,
    rhs: *const u8,
    len: usize,
) -> Option<(usize, usize)> {
    unsafe {
        let mut start = 0;
        while start + 16 <= len {
            if vmaxvq_u8(veorq_u8(vld1q_u8(lhs.add(start)), vld1q_u8(rhs.add(start)))) != 0 {
                break;
            }
            start += 16;
        }
        while start < len && *lhs.add(start) == *rhs.add(start) {
            start += 1;
        }
        if start == len {
            return None;
        }
        let mut end = len;
        while end >= start + 16 {
            if vmaxvq_u8(veorq_u8(
                vld1q_u8(lhs.add(end - 16)),
                vld1q_u8(rhs.add(end - 16)),
            )) != 0
            {
                break;
            }
            end -= 16;
        }
        while end > start && *lhs.add(end - 1) == *rhs.add(end - 1) {
            end -= 1;
        }
        Some((start, end))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    #[ignore = "run explicitly in the performance Release build"]
    fn kernel_performance_comparison() {
        use std::hint::black_box;
        use std::time::{Duration, Instant};
        if cfg!(debug_assertions) {
            panic!("use the performance Release build");
        }
        const COUNT: usize = 1280 * 720;
        const ITERATIONS: usize = 12;
        fn measure(mut run: impl FnMut()) -> Duration {
            run();
            (0..3)
                .map(|_| {
                    let start = Instant::now();
                    for _ in 0..ITERATIONS {
                        run();
                    }
                    start.elapsed()
                })
                .min()
                .unwrap()
        }
        let bgra: Vec<u8> = (0..COUNT * 4).map(|i| (i * 73 + 19) as u8).collect();
        let half: Vec<u8> = (0..COUNT)
            .flat_map(|i| {
                let value = (i % 1024) as f32 / 128.0;
                [value, value * 0.7, value * 0.3, 0.5]
                    .into_iter()
                    .flat_map(|channel| half::f16::from_f32(channel).to_bits().to_le_bytes())
            })
            .collect();
        let sdr: Vec<u8> = (0..COUNT)
            .flat_map(|i| {
                let value = (i % 1024) as f32 / 1023.0;
                [value, value * 0.7, value * 0.3, 0.5]
                    .into_iter()
                    .flat_map(|channel| half::f16::from_f32(channel).to_bits().to_le_bytes())
            })
            .collect();
        let mut output = vec![0; COUNT * 4];
        for (pattern, pixels) in [("sdr", &sdr), ("clamped", &half)] {
            for (name, source, scalar, neon) in [
                (
                    "bgra",
                    &bgra,
                    scalar::convert_bgra_to_rgba_scalar_unchecked as super::super::PixelKernel,
                    convert_bgra as super::super::PixelKernel,
                ),
                (
                    "bgra_opaque",
                    &bgra,
                    scalar::convert_bgra_to_rgba_scalar_opaque_unchecked
                        as super::super::PixelKernel,
                    convert_bgra_opaque as super::super::PixelKernel,
                ),
                (
                    "f16_sdr",
                    pixels,
                    f16::convert_f16_rgba_to_srgb_scalar_unchecked as super::super::PixelKernel,
                    convert_f16 as super::super::PixelKernel,
                ),
                (
                    "f16_sdr_opaque",
                    pixels,
                    f16::convert_f16_rgba_to_srgb_scalar_opaque_unchecked
                        as super::super::PixelKernel,
                    convert_f16_opaque as super::super::PixelKernel,
                ),
            ] {
                let scalar_time = measure(|| unsafe {
                    scalar(
                        black_box(source.as_ptr()),
                        black_box(output.as_mut_ptr()),
                        COUNT,
                    );
                    black_box(&output);
                });
                let neon_time = measure(|| unsafe {
                    neon(
                        black_box(source.as_ptr()),
                        black_box(output.as_mut_ptr()),
                        COUNT,
                    );
                    black_box(&output);
                });
                println!(
                    "capture_neon,{pattern}_{name},scalar_ns={},neon_ns={},speedup={:.3}",
                    scalar_time.as_nanos() / ITERATIONS as u128,
                    neon_time.as_nanos() / ITERATIONS as u128,
                    scalar_time.as_secs_f64() / neon_time.as_secs_f64()
                );
            }
        }
        for lut in [false, true] {
            let prepared = f16::prepare_hdr_context(super::super::HdrFrameContext {
                tonemap_use_lut: lut,
                ..Default::default()
            });
            let scalar_time = measure(|| unsafe {
                f16::convert_f16_rgba_to_srgb_hdr_scalar_prepared_unchecked(
                    black_box(half.as_ptr()),
                    black_box(output.as_mut_ptr()),
                    COUNT,
                    &prepared,
                );
                black_box(&output);
            });
            let neon_time = measure(|| unsafe {
                convert_hdr(
                    black_box(half.as_ptr()),
                    black_box(output.as_mut_ptr()),
                    COUNT,
                    &prepared,
                );
                black_box(&output);
            });
            println!(
                "capture_neon,hdr_lut_{lut},scalar_ns={},neon_ns={},speedup={:.3}",
                scalar_time.as_nanos() / ITERATIONS as u128,
                neon_time.as_nanos() / ITERATIONS as u128,
                scalar_time.as_secs_f64() / neon_time.as_secs_f64()
            );
        }
    }

    #[test]
    fn dispatch_selects_neon_for_regular_opaque_and_cached_store_variants() {
        use super::super::*;
        for selector in [
            select_bgra_kernel,
            select_bgra_kernel_nt,
            select_bgra_kernel_nt_nofence,
        ] {
            assert!(std::ptr::fn_addr_eq(
                selector(),
                convert_bgra as PixelKernel
            ));
        }
        for selector in [
            select_bgra_opaque_kernel,
            select_bgra_opaque_kernel_nt,
            select_bgra_opaque_kernel_nt_nofence,
        ] {
            assert!(std::ptr::fn_addr_eq(
                selector(),
                convert_bgra_opaque as PixelKernel
            ));
        }
        for selector in [
            select_f16_kernel,
            select_f16_kernel_nt,
            select_f16_kernel_nt_nofence,
        ] {
            assert!(std::ptr::fn_addr_eq(selector(), convert_f16 as PixelKernel));
        }
        for selector in [
            select_f16_opaque_kernel,
            select_f16_opaque_kernel_nt,
            select_f16_opaque_kernel_nt_nofence,
        ] {
            assert!(std::ptr::fn_addr_eq(
                selector(),
                convert_f16_opaque as PixelKernel
            ));
        }
        assert!(std::ptr::fn_addr_eq(
            select_f16_hdr_prepared_kernel(),
            convert_hdr as HdrPreparedPixelKernel
        ));
        assert!(std::ptr::fn_addr_eq(
            select_f16_hdr_prepared_opaque_kernel(),
            convert_hdr_opaque as HdrPreparedPixelKernel
        ));
    }

    #[test]
    fn half_decode_matches_every_binary16_pattern_without_optional_fp16() {
        for base in (0..=u16::MAX as u32).step_by(4) {
            let bits = std::array::from_fn::<_, 4, _>(|lane| (base + lane as u32) as u16);
            let mut actual = [0.0; 4];
            unsafe { vst1q_f32(actual.as_mut_ptr(), half_to_float(vld1_u16(bits.as_ptr()))) };
            for (bits, actual) in bits.into_iter().zip(actual) {
                let expected = half::f16::from_bits(bits).to_f32();
                assert!(
                    actual.to_bits() == expected.to_bits() || actual.is_nan() && expected.is_nan(),
                    "half {bits:04x}: {actual} != {expected}"
                );
            }
        }
    }

    #[test]
    fn bgra_neon_matches_scalar_for_unaligned_tails_and_in_place_buffers() {
        for count in 0..68 {
            for offset in 0..16 {
                let source: Vec<u8> = (0..count * 4 + 32).map(|i| (i * 71 + 19) as u8).collect();
                for opaque in [false, true] {
                    let mut actual = vec![0xa5; source.len()];
                    let mut expected = actual.clone();
                    unsafe {
                        if opaque {
                            convert_bgra_opaque(
                                source.as_ptr().add(offset),
                                actual.as_mut_ptr().add(offset),
                                count,
                            );
                            scalar::convert_bgra_to_rgba_scalar_opaque_unchecked(
                                source.as_ptr().add(offset),
                                expected.as_mut_ptr().add(offset),
                                count,
                            );
                        } else {
                            convert_bgra(
                                source.as_ptr().add(offset),
                                actual.as_mut_ptr().add(offset),
                                count,
                            );
                            scalar::convert_bgra_to_rgba_scalar_unchecked(
                                source.as_ptr().add(offset),
                                expected.as_mut_ptr().add(offset),
                                count,
                            );
                        }
                    }
                    assert_eq!(
                        actual, expected,
                        "count={count}, offset={offset}, opaque={opaque}"
                    );
                    let mut in_place = source.clone();
                    unsafe {
                        let pointer = in_place.as_mut_ptr().add(offset);
                        if opaque {
                            convert_bgra_opaque(pointer, pointer, count);
                        } else {
                            convert_bgra(pointer, pointer, count);
                        }
                    }
                    assert_eq!(
                        &in_place[offset..offset + count * 4],
                        &expected[offset..offset + count * 4]
                    );
                }
            }
        }
    }

    #[test]
    fn f16_sdr_preserves_exact_lut_bytes_for_all_half_patterns_and_alpha() {
        let source: Vec<u8> = (0..=u16::MAX)
            .flat_map(|value| {
                [
                    value,
                    value.wrapping_mul(73),
                    value.wrapping_add(511),
                    value,
                ]
                .into_iter()
                .flat_map(u16::to_le_bytes)
            })
            .collect();
        for opaque in [false, true] {
            let mut actual = vec![0; source.len() / 2];
            let mut expected = actual.clone();
            unsafe {
                if opaque {
                    convert_f16_opaque(source.as_ptr(), actual.as_mut_ptr(), 65_536);
                    f16::convert_f16_rgba_to_srgb_scalar_opaque_unchecked(
                        source.as_ptr(),
                        expected.as_mut_ptr(),
                        65_536,
                    );
                } else {
                    convert_f16(source.as_ptr(), actual.as_mut_ptr(), 65_536);
                    f16::convert_f16_rgba_to_srgb_scalar_unchecked(
                        source.as_ptr(),
                        expected.as_mut_ptr(),
                        65_536,
                    );
                }
            }
            for (index, (actual, expected)) in actual.into_iter().zip(expected).enumerate() {
                assert_eq!(
                    actual,
                    expected,
                    "half pattern {}, channel {}, opaque={opaque}",
                    index / 4,
                    index % 4
                );
            }
        }
    }

    #[test]
    fn f16_neon_preserves_unaligned_source_destination_and_scalar_tails() {
        for count in 0..36 {
            for offset in 0..16 {
                let mut source = vec![0xcc; count * 8 + 32];
                for index in 0..count * 4 {
                    source[offset + index * 2..offset + index * 2 + 2]
                        .copy_from_slice(&((index * 1033) as u16).to_le_bytes());
                }
                for opaque in [false, true] {
                    let mut actual = vec![0xa5; count * 4 + 32];
                    let mut expected = actual.clone();
                    unsafe {
                        if opaque {
                            convert_f16_opaque(
                                source.as_ptr().add(offset),
                                actual.as_mut_ptr().add(offset),
                                count,
                            );
                            f16::convert_f16_rgba_to_srgb_scalar_opaque_unchecked(
                                source.as_ptr().add(offset),
                                expected.as_mut_ptr().add(offset),
                                count,
                            );
                        } else {
                            convert_f16(
                                source.as_ptr().add(offset),
                                actual.as_mut_ptr().add(offset),
                                count,
                            );
                            f16::convert_f16_rgba_to_srgb_scalar_unchecked(
                                source.as_ptr().add(offset),
                                expected.as_mut_ptr().add(offset),
                                count,
                            );
                        }
                    }
                    assert_eq!(
                        actual, expected,
                        "count={count}, offset={offset}, opaque={opaque}"
                    );
                }
            }
        }
    }

    #[test]
    fn gdi_neon_scan_matches_first_last_reference_for_unaligned_rows_and_tails() {
        for len in 0..260 {
            for offset in 0..16 {
                let left: Vec<u8> = (0..len + 32).map(|i| (i * 73 + 19) as u8).collect();
                let mut right = left.clone();
                unsafe {
                    assert!(row_equal(
                        left.as_ptr().add(offset),
                        right.as_ptr().add(offset),
                        len
                    ));
                    assert_eq!(
                        row_diff_bounds(left.as_ptr().add(offset), right.as_ptr().add(offset), len),
                        None
                    );
                }
                for first in 0..len {
                    let last = first + (len - first) / 2;
                    right[offset + first] ^= 0xff;
                    if last != first {
                        right[offset + last] ^= 0x81;
                    }
                    unsafe {
                        assert!(!row_equal(
                            left.as_ptr().add(offset),
                            right.as_ptr().add(offset),
                            len
                        ));
                        assert_eq!(
                            row_diff_bounds(
                                left.as_ptr().add(offset),
                                right.as_ptr().add(offset),
                                len
                            ),
                            Some((first, last + 1))
                        );
                    }
                    right[offset + first] ^= 0xff;
                    if last != first {
                        right[offset + last] ^= 0x81;
                    }
                }
            }
        }
    }
}
