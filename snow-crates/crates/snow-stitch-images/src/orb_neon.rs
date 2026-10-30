//! ARM64 counterparts of the runtime-dispatched ORB kernels.
use super::*;
use std::arch::aarch64::*;

#[cfg(test)]
static EXECUTED: [std::sync::atomic::AtomicUsize; 4] =
    [const { std::sync::atomic::AtomicUsize::new(0) }; 4];
#[inline]
fn executed(backend: usize) {
    #[cfg(test)]
    EXECUTED[backend].fetch_add(1, std::sync::atomic::Ordering::Relaxed);
    #[cfg(not(test))]
    let _ = backend;
}

#[target_feature(enable = "neon")]
unsafe fn bytes_to_f32(bytes: uint8x8_t) -> [float32x4_t; 2] {
    let words = vmovl_u8(bytes);
    [
        vcvtq_f32_u32(vmovl_u16(vget_low_u16(words))),
        vcvtq_f32_u32(vmovl_u16(vget_high_u16(words))),
    ]
}

#[target_feature(enable = "neon")]
pub(super) unsafe fn grayscale_chunk(source: &[u8], output: &mut [u8], channels: usize) {
    unsafe {
        let mut x = 0;
        while x + 8 <= output.len() {
            let (r, g, b) = if channels == 3 {
                let pixels = vld3_u8(source.as_ptr().add(x * 3));
                (pixels.0, pixels.1, pixels.2)
            } else {
                let pixels = vld4_u8(source.as_ptr().add(x * 4));
                (pixels.0, pixels.1, pixels.2)
            };
            let r = bytes_to_f32(r);
            let g = bytes_to_f32(g);
            let b = bytes_to_f32(b);
            let convert = |half| {
                let mut value = vmulq_n_f32(r[half], 0.299);
                value = vaddq_f32(value, vmulq_n_f32(g[half], 0.587));
                value = vaddq_f32(value, vmulq_n_f32(b[half], 0.114));
                vmovn_u32(vcvtq_u32_f32(vrndaq_f32(value)))
            };
            vst1_u8(
                output.as_mut_ptr().add(x),
                vmovn_u16(vcombine_u16(convert(0), convert(1))),
            );
            x += 8;
        }
        if x > 0 {
            executed(0);
        }
        for (target, pixel) in output[x..]
            .iter_mut()
            .zip(source[x * channels..].chunks_exact(channels))
        {
            *target = grayscale_pixel(pixel);
        }
    }
}

#[target_feature(enable = "neon")]
pub(super) unsafe fn grayscale(frame: &Frame) -> Image {
    let channels = frame.pixel_format().channels() as usize;
    if channels == 1 {
        return grayscale_scalar(frame);
    }
    let mut pixels = RasterBuffer::zeroed(frame.width() as usize * frame.height() as usize);
    pixels
        .par_chunks_mut(32 * 1024)
        .enumerate()
        .for_each(|(index, output)| {
            let start = index * 32 * 1024 * channels;
            unsafe {
                grayscale_chunk(
                    &frame.pixels()[start..start + output.len() * channels],
                    output,
                    channels,
                )
            };
        });
    Image::from_buffer(frame.width() as usize, frame.height() as usize, pixels)
}

pub(super) fn fast_polarity_masks(differences: &[i16; 16], threshold: i16) -> (u32, u32) {
    unsafe {
        let weights = vld1q_u16([1, 2, 4, 8, 16, 32, 64, 128].as_ptr());
        let first = vld1q_s16(differences.as_ptr());
        let second = vld1q_s16(differences.as_ptr().add(8));
        let mask = |a, b| u32::from(vaddvq_u16(vandq_u16(vcgtq_s16(a, b), weights)));
        let dark = vdupq_n_s16(threshold);
        let bright = vdupq_n_s16(-threshold);
        (
            mask(first, dark) | (mask(second, dark) << 8),
            mask(bright, first) | (mask(bright, second) << 8),
        )
    }
}

pub(super) fn strongest_fast_arc(differences: &[i16; 16]) -> i16 {
    unsafe {
        let first = vld1q_s16(differences.as_ptr());
        let second = vld1q_s16(differences.as_ptr().add(8));
        let mut minima = [first, second];
        let mut maxima = minima;
        macro_rules! reduce {
            ($shift:literal) => {{
                let [a, b] = minima;
                minima = [
                    vminq_s16(a, vextq_s16::<$shift>(a, b)),
                    vminq_s16(b, vextq_s16::<$shift>(b, a)),
                ];
                let [a, b] = maxima;
                maxima = [
                    vmaxq_s16(a, vextq_s16::<$shift>(a, b)),
                    vmaxq_s16(b, vextq_s16::<$shift>(b, a)),
                ];
            }};
        }
        reduce!(1);
        reduce!(2);
        reduce!(4);
        let dark = vmaxq_s16(vminq_s16(minima[0], second), vminq_s16(minima[1], first));
        let bright = vminq_s16(vmaxq_s16(maxima[0], second), vmaxq_s16(maxima[1], first));
        vmaxvq_s16(vmaxq_s16(dark, vnegq_s16(bright)))
    }
}

#[target_feature(enable = "neon")]
pub(super) unsafe fn fast_scores(
    width: u32,
    height: u32,
    pixels: &[u8],
    threshold: i16,
    border: u32,
) -> RasterBuffer {
    let mut scores = RasterBuffer::zeroed(pixels.len());
    scores
        .par_chunks_mut(width as usize)
        .enumerate()
        .for_each(|(y, row)| {
            if !(border as usize..(height - border) as usize).contains(&y) {
                return;
            }
            unsafe {
                let w = width as usize;
                let mut x = border as usize;
                while x + 8 <= w - border as usize {
                    let center_index = y * w + x;
                    let center =
                        vreinterpretq_s16_u16(vmovl_u8(vld1_u8(pixels.as_ptr().add(center_index))));
                    let mut dark = vdupq_n_u16(0);
                    let mut bright = dark;
                    for offset in [3 * w as isize, 3, -3 * w as isize, -3] {
                        let sample = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(
                            pixels.as_ptr().add(center_index).offset(offset),
                        )));
                        dark = vsubq_u16(
                            dark,
                            vcgtq_s16(vsubq_s16(center, sample), vdupq_n_s16(threshold)),
                        );
                        bright = vsubq_u16(
                            bright,
                            vcgtq_s16(vsubq_s16(sample, center), vdupq_n_s16(threshold)),
                        );
                    }
                    let passes = vorrq_u16(
                        vcgtq_u16(dark, vdupq_n_u16(1)),
                        vcgtq_u16(bright, vdupq_n_u16(1)),
                    );
                    let mut lanes = [0_u16; 8];
                    vst1q_u16(lanes.as_mut_ptr(), passes);
                    for (lane, pass) in lanes.into_iter().enumerate() {
                        if pass != 0 {
                            row[x + lane] = score(
                                circle_values(width, pixels, (x + lane) as u32, y as u32),
                                threshold,
                            )
                            .unwrap_or(0);
                        }
                    }
                    x += 8;
                }
                if x > border as usize {
                    executed(1);
                }
                for target in x as u32..width - border {
                    if may_pass_fast_9_16(width, pixels, target, y as u32, threshold) {
                        row[target as usize] =
                            score(circle_values(width, pixels, target, y as u32), threshold)
                                .unwrap_or(0);
                    }
                }
            }
        });
    scores
}

#[target_feature(enable = "neon")]
unsafe fn load_four_bytes(source: *const u8) -> float32x4_t {
    // A four-byte load avoids reading past the final reflected-border interior.
    unsafe {
        let packed = std::ptr::read_unaligned(source.cast::<u32>());
        let bytes = vcreate_u8(u64::from(packed));
        vcvtq_f32_u32(vmovl_u16(vget_low_u16(vmovl_u8(bytes))))
    }
}

#[target_feature(enable = "neon")]
pub(super) unsafe fn blur(image: &Image) -> Image {
    blur_with_rows(
        image,
        |row, y| unsafe {
            if image.width <= 6 {
                horizontal_blur_row_scalar(&image.pixels, image.width, row, y);
                return;
            }
            for (x, target) in row.iter_mut().take(3).enumerate() {
                *target = horizontal_blur_reflected(&image.pixels, image.width, x, y);
            }
            let mut x = 3;
            while x + 4 <= image.width - 3 {
                let source = image.pixels.as_ptr().add(y * image.width + x - 3);
                let mut value = vmulq_n_f32(load_four_bytes(source), BLUR_KERNEL[0]);
                for (offset, weight) in BLUR_KERNEL.iter().enumerate().skip(1) {
                    value = vaddq_f32(
                        value,
                        vmulq_n_f32(load_four_bytes(source.add(offset)), *weight),
                    );
                }
                vst1q_f32(row.as_mut_ptr().add(x), value);
                x += 4;
            }
            if x > 3 {
                executed(2);
            }
            for (x, target) in row.iter_mut().enumerate().skip(x) {
                *target = horizontal_blur_reflected(&image.pixels, image.width, x, y);
            }
        },
        |rows, output| unsafe {
            let mut x = 0;
            while x + 4 <= output.len() {
                let load = |row: usize| vld1q_f32(rows[row].as_ptr().add(x));
                let mut value = vmulq_n_f32(load(3), BLUR_KERNEL[3]);
                for offset in 1..4 {
                    value = vaddq_f32(
                        value,
                        vmulq_n_f32(
                            vaddq_f32(load(3 - offset), load(3 + offset)),
                            BLUR_KERNEL[3 + offset],
                        ),
                    );
                }
                let mut values = [0.0_f32; 4];
                vst1q_f32(values.as_mut_ptr(), value);
                for (target, value) in output[x..x + 4].iter_mut().zip(values) {
                    *target = cv_round(value).clamp(0, 255) as u8;
                }
                x += 4;
            }
            if x > 0 {
                executed(2);
            }
            for (x, target) in output.iter_mut().enumerate().skip(x) {
                *target = vertical_blur_pixel(&rows, x);
            }
        },
    )
}

#[target_feature(enable = "neon")]
pub(super) unsafe fn descriptor(image: &Image, keypoint: Keypoint) -> [u8; 32] {
    const RADIANS_PER_DEGREE: f32 = (std::f64::consts::PI / 180.0) as f32;
    let scale = 1.0 / scale_for_level(keypoint.octave);
    let center_x = cv_round(keypoint.x * scale) as isize;
    let center_y = cv_round(keypoint.y * scale) as isize;
    let radians = keypoint.angle * RADIANS_PER_DEGREE;
    let cosine = radians.cos();
    let sine = radians.sin();
    let (pattern_x, pattern_y) = orb_pattern_xy();
    let mut samples = [0_u8; 512];
    unsafe {
        for start in (0..512).step_by(4) {
            let x = vcvtq_f32_s32(vld1q_s32(pattern_x.as_ptr().add(start)));
            let y = vcvtq_f32_s32(vld1q_s32(pattern_y.as_ptr().add(start)));
            let rx = vcvtnq_s32_f32(vsubq_f32(vmulq_n_f32(x, cosine), vmulq_n_f32(y, sine)));
            let ry = vcvtnq_s32_f32(vaddq_f32(vmulq_n_f32(x, sine), vmulq_n_f32(y, cosine)));
            let mut xs = [0_i32; 4];
            let mut ys = [0_i32; 4];
            vst1q_s32(xs.as_mut_ptr(), rx);
            vst1q_s32(ys.as_mut_ptr(), ry);
            for lane in 0..4 {
                samples[start + lane] = image.pixels[(center_y + ys[lane] as isize) as usize
                    * image.width
                    + (center_x + xs[lane] as isize) as usize];
            }
        }
    }
    executed(3);
    std::array::from_fn(|byte| {
        (0..8).fold(0, |value, bit| {
            value
                | (u8::from(samples[byte * 16 + bit * 2] < samples[byte * 16 + bit * 2 + 1]) << bit)
        })
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn neon_orb_kernels_execute_and_match_scalar_for_formats_tails_arcs_and_angles() {
        assert!(std::arch::is_aarch64_feature_detected!("neon"));
        let before = EXECUTED
            .each_ref()
            .map(|n| n.load(std::sync::atomic::Ordering::Relaxed));
        for channels in [3, 4] {
            for len in 0..67 {
                let storage: Vec<u8> = (0..len * channels + 1)
                    .map(|i| (i * 73 + i / 7 * 19 + 11) as u8)
                    .collect();
                let source = &storage[1..];
                let expected: Vec<u8> =
                    source.chunks_exact(channels).map(grayscale_pixel).collect();
                let mut output = vec![0; len + 1];
                unsafe { grayscale_chunk(source, &mut output[1..], channels) };
                assert_eq!(&output[1..], expected);
                super::super::grayscale_chunk(source, &mut output[1..], channels);
                assert_eq!(&output[1..], expected);
            }
        }
        for format in [PixelFormat::Gray8, PixelFormat::Rgb8, PixelFormat::Rgba8] {
            let frame = Frame::new(
                337,
                101,
                format,
                (0..337 * 101 * format.channels() as usize)
                    .map(|i| (i * 73 + i / 5) as u8)
                    .collect(),
            )
            .unwrap();
            assert_eq!(unsafe { grayscale(&frame) }, grayscale_scalar(&frame));
            assert_eq!(super::super::grayscale(&frame), grayscale_scalar(&frame));
        }
        let mut state = 17_u32;
        for _ in 0..4096 {
            let differences = std::array::from_fn(|_| {
                state = state.wrapping_mul(1664525).wrapping_add(1013904223);
                (state % 511) as i16 - 255
            });
            assert_eq!(
                strongest_fast_arc(&differences),
                strongest_fast_arc_scalar(&differences)
            );
            for threshold in [0, 1, 20, 127, 254, 255] {
                let expected = differences
                    .iter()
                    .enumerate()
                    .fold((0, 0), |(a, b), (i, d)| {
                        (
                            a | (u32::from(*d > threshold) << i),
                            b | (u32::from(-*d > threshold) << i),
                        )
                    });
                assert_eq!(fast_polarity_masks(&differences, threshold), expected);
            }
        }
        for start in 0..16 {
            for threshold in [0_i16, 20, 254] {
                let mut differences = [0; 16];
                for offset in 0..9 {
                    differences[(start + offset) % 16] = threshold + 1;
                }
                assert_eq!(score(differences, threshold), Some(threshold as u8));
            }
        }
        for width in [7, 8, 13, 14, 15, 16, 17, 22, 31, 32, 33, 65, 127] {
            let height = 19;
            let pixels: Vec<u8> = (0..width * height)
                .map(|i| (i * 73 + i / 11 * 29) as u8)
                .collect();
            for threshold in [0, 20, 254, 255] {
                assert_eq!(
                    unsafe { fast_scores(width, height, &pixels, threshold, 3) },
                    fast_scores_scalar(width, height, &pixels, threshold, 3)
                );
            }
        }
        for width in 1..42 {
            for height in [1, 2, 3, 7, 8, 17] {
                let image = Image::new(
                    width,
                    height,
                    (0..width * height)
                        .map(|i| (i * 73 + i / 11 * 29) as u8)
                        .collect(),
                );
                assert_eq!(
                    unsafe { blur(&image) },
                    blur_scalar(&image),
                    "blur {width}x{height}"
                );
            }
        }
        let image = Image::new(
            128,
            112,
            (0..128 * 112)
                .map(|i| (i * 73 + i / 11 * 29) as u8)
                .collect(),
        );
        for octave in [0, 1, 3, 7] {
            let scale = scale_for_level(octave);
            for (x, y) in [(31.0, 31.0), (63.0, 55.0), (96.0, 80.0)] {
                for angle in [
                    0.0, 0.5, 17.25, 44.999, 45.0, 89.5, 137.0, 179.999, 225.0, 315.75, 359.5,
                ] {
                    let keypoint = Keypoint {
                        x: x * scale,
                        y: y * scale,
                        response: 1.0,
                        angle,
                        octave,
                    };
                    assert_eq!(
                        unsafe { descriptor(&image, keypoint) },
                        descriptor_scalar(&image, keypoint)
                    );
                }
            }
        }
        for (index, n) in EXECUTED.iter().enumerate() {
            assert!(
                n.load(std::sync::atomic::Ordering::Relaxed) > before[index],
                "NEON backend {index} executed no vector blocks"
            );
        }
    }
    #[test]
    #[ignore = "paired Release-only ARM64 kernel measurement"]
    fn neon_release_benchmark_orb() {
        use std::hint::black_box;
        let frame = Frame::new(
            1280,
            720,
            PixelFormat::Rgba8,
            (0..1280 * 720 * 4)
                .map(|i| (i * 73 + i / 11 * 29) as u8)
                .collect(),
        )
        .unwrap();
        crate::neon_benchmark::paired(
            "stitch_orb_grayscale",
            8,
            || grayscale_scalar(black_box(&frame)),
            || unsafe { grayscale(black_box(&frame)) },
        );
        let image = grayscale_scalar(&frame);
        crate::neon_benchmark::paired(
            "stitch_orb_blur",
            8,
            || blur_scalar(black_box(&image)),
            || unsafe { blur(black_box(&image)) },
        );
        let small = Image::new(512, 384, image.pixels[..512 * 384].to_vec());
        crate::neon_benchmark::paired(
            "stitch_orb_fast_prefilter",
            4,
            || fast_scores_scalar(512, 384, black_box(&small.pixels), 20, 3),
            || unsafe { fast_scores(512, 384, black_box(&small.pixels), 20, 3) },
        );
        let differences = std::array::from_fn(|i| (i as i16 * 73 % 511) - 255);
        crate::neon_benchmark::paired(
            "stitch_fast_arc",
            65536,
            || strongest_fast_arc_scalar(black_box(&differences)),
            || strongest_fast_arc(black_box(&differences)),
        );
        let keypoints: Vec<_> = (0..256)
            .map(|i| Keypoint {
                x: 40.0 + (i % 32) as f32 * 13.0,
                y: 40.0 + (i / 32) as f32 * 13.0,
                response: 1.0,
                angle: i as f32 * 1.337,
                octave: 0,
            })
            .collect();
        crate::neon_benchmark::paired(
            "stitch_orb_descriptors",
            8,
            || {
                keypoints
                    .iter()
                    .map(|keypoint| descriptor_scalar(black_box(&small), *keypoint))
                    .collect::<Vec<_>>()
            },
            || {
                keypoints
                    .iter()
                    .map(|keypoint| unsafe { descriptor(black_box(&small), *keypoint) })
                    .collect::<Vec<_>>()
            },
        );
    }
}
