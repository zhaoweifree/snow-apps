//! Output conversion shared by all recording export paths. GIF input remains
//! RGB until encoding, where differences are determined before quantization.
use std::ops::{Deref, DerefMut};

use ffmpeg_next as ffmpeg;

use crate::error::{RecordingExportError, Result};

/// Recording downscales use pixel-area coverage, including the RGB48
/// intermediate used by HDR overlays. Native conversion and any enlarged axis
/// keep the caller's filter; color-conversion options are always preserved.
pub(crate) fn recording_scaler(
    input: ffmpeg::format::Pixel,
    input_width: u32,
    input_height: u32,
    output: ffmpeg::format::Pixel,
    width: u32,
    height: u32,
    flags: ffmpeg::software::scaling::Flags,
) -> std::result::Result<ffmpeg::software::scaling::Context, ffmpeg::Error> {
    let flags = if width <= input_width
        && height <= input_height
        && (width < input_width || height < input_height)
    {
        use ffmpeg::software::scaling::Flags;
        let filters = Flags::FAST_BILINEAR
            | Flags::BILINEAR
            | Flags::BICUBIC
            | Flags::X
            | Flags::POINT
            | Flags::AREA
            | Flags::BICUBLIN
            | Flags::GAUSS
            | Flags::SINC
            | Flags::LANCZOS
            | Flags::SPLINE;
        (flags & !filters) | Flags::AREA
    } else {
        flags
    };
    ffmpeg::software::scaling::Context::get(
        input,
        input_width,
        input_height,
        output,
        width,
        height,
        flags,
    )
}

pub(crate) struct OutputScaler {
    scaler: ffmpeg::software::scaling::Context,
}

impl OutputScaler {
    pub(crate) fn get(
        input: ffmpeg::format::Pixel,
        input_width: u32,
        input_height: u32,
        output: ffmpeg::format::Pixel,
        width: u32,
        height: u32,
        flags: ffmpeg::software::scaling::Flags,
    ) -> std::result::Result<Self, ffmpeg::Error> {
        let scaler = recording_scaler(
            input,
            input_width,
            input_height,
            output,
            width,
            height,
            flags,
        )?;
        Ok(Self { scaler })
    }

    pub(crate) fn run(
        &mut self,
        input: &ffmpeg::frame::Video,
        output: &mut ffmpeg::frame::Video,
    ) -> std::result::Result<(), ffmpeg::Error> {
        self.scaler.run(input, output)
    }

    pub(crate) fn run_owned_rgba(
        &mut self,
        input: &[u8],
        output: &mut ffmpeg::frame::Video,
    ) -> Result<()> {
        convert_owned_rgba(&mut self.scaler, input, output)
    }
}

impl Deref for OutputScaler {
    type Target = ffmpeg::software::scaling::Context;
    fn deref(&self) -> &Self::Target {
        &self.scaler
    }
}

impl DerefMut for OutputScaler {
    fn deref_mut(&mut self) -> &mut Self::Target {
        &mut self.scaler
    }
}

pub(crate) fn convert_owned_rgba(
    scaler: &mut ffmpeg::software::scaling::Context,
    rgba: &[u8],
    output: &mut ffmpeg::frame::Video,
) -> Result<()> {
    let input = scaler.input();
    let stride = input
        .width
        .checked_mul(4)
        .and_then(|value| i32::try_from(value).ok())
        .ok_or_else(|| RecordingExportError::InvalidConfig("RGBA row size overflow".into()))?;
    let expected = stride as usize * input.height as usize;
    if input.format != ffmpeg::format::Pixel::RGBA
        || rgba.len() < expected + ffmpeg::ffi::AV_INPUT_BUFFER_PADDING_SIZE as usize
        || output.format() != scaler.output().format
        || (output.width(), output.height()) != (scaler.output().width, scaler.output().height)
    {
        return Err(RecordingExportError::InvalidConfig(
            "invalid owned RGBA conversion layout".into(),
        ));
    }
    let height = input.height;
    let mut source = [std::ptr::null(); 8];
    source[0] = rgba.as_ptr();
    let mut strides = [0; 8];
    strides[0] = stride;
    // SAFETY: sws_scale is synchronous and does not retain these source pointers.
    // The caller owns the initialized RGBA pixels and SIMD tail padding through
    // this call; each source row has exactly `stride` bytes. The destination is
    // a fully allocated, writable AVFrame matching the scaler output. Encoding
    // references only that separate destination, so RGBA storage can be recycled.
    let rows = unsafe {
        ffmpeg::ffi::sws_scale(
            scaler.as_mut_ptr(),
            source.as_ptr(),
            strides.as_ptr(),
            0,
            height as i32,
            (*output.as_mut_ptr()).data.as_ptr(),
            (*output.as_mut_ptr()).linesize.as_ptr(),
        )
    };
    if rows != output.height() as i32 {
        return Err(RecordingExportError::Encode(format!(
            "owned RGBA conversion returned {rows} rows"
        )));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reduction_policy_replaces_every_filter_and_preserves_conversion_options() {
        crate::ffmpeg_util::ensure_ffmpeg_initialized().unwrap();
        use ffmpeg::software::scaling::Flags;
        let options = Flags::ACCURATE_RND | Flags::BITEXACT | Flags::FULL_CHR_H_INT;
        for filter in [
            Flags::FAST_BILINEAR,
            Flags::BILINEAR,
            Flags::BICUBIC,
            Flags::X,
            Flags::POINT,
            Flags::AREA,
            Flags::BICUBLIN,
            Flags::GAUSS,
            Flags::SINC,
            Flags::LANCZOS,
            Flags::SPLINE,
        ] {
            for output in [(16, 12), (32, 24), (64, 48), (16, 48)] {
                let mut scaler = recording_scaler(
                    ffmpeg::format::Pixel::RGBA,
                    32,
                    24,
                    ffmpeg::format::Pixel::RGB24,
                    output.0,
                    output.1,
                    filter | options,
                )
                .unwrap();
                let mut configured = 0;
                // Inspect FFmpeg's initialized context, including modifier bits
                // that affect chroma conversion and deterministic rounding.
                unsafe {
                    assert_eq!(
                        ffmpeg::ffi::av_opt_get_int(
                            scaler.as_mut_ptr().cast(),
                            c"sws_flags".as_ptr(),
                            0,
                            &mut configured
                        ),
                        0
                    );
                }
                let expected = if output == (16, 12) {
                    Flags::AREA
                } else {
                    filter
                };
                assert_eq!(configured, i64::from((expected | options).bits()));
            }
        }
    }

    fn rgba_pixels(width: u32, height: u32, pixel: impl Fn(u32, u32) -> [u8; 4]) -> Vec<u8> {
        let mut pixels = Vec::with_capacity(width as usize * height as usize * 4);
        for y in 0..height {
            for x in 0..width {
                pixels.extend_from_slice(&pixel(x, y));
            }
        }
        pixels.resize(
            pixels.len() + ffmpeg::ffi::AV_INPUT_BUFFER_PADDING_SIZE as usize,
            0,
        );
        pixels
    }

    fn rgba_frame(pixels: &[u8], width: u32, height: u32) -> ffmpeg::frame::Video {
        let mut frame = ffmpeg::frame::Video::new(ffmpeg::format::Pixel::RGBA, width, height);
        let stride = frame.stride(0);
        frame.data_mut(0).fill(0xA5);
        for y in 0..height as usize {
            let source = y * width as usize * 4;
            frame.data_mut(0)[y * stride..][..width as usize * 4]
                .copy_from_slice(&pixels[source..][..width as usize * 4]);
        }
        frame
    }

    fn rgb_pixels(frame: &ffmpeg::frame::Video) -> Vec<u8> {
        frame
            .data(0)
            .chunks(frame.stride(0))
            .take(frame.height() as usize)
            .flat_map(|row| row[..frame.width() as usize * 3].iter().copied())
            .collect()
    }

    fn scale_both(pixels: &[u8], source: (u32, u32), output: (u32, u32)) -> Vec<u8> {
        crate::ffmpeg_util::ensure_ffmpeg_initialized().unwrap();
        let mut scaler = OutputScaler::get(
            ffmpeg::format::Pixel::RGBA,
            source.0,
            source.1,
            ffmpeg::format::Pixel::RGB24,
            output.0,
            output.1,
            ffmpeg::software::scaling::Flags::BICUBIC,
        )
        .unwrap();
        let frame = rgba_frame(pixels, source.0, source.1);
        let mut from_frame =
            ffmpeg::frame::Video::new(ffmpeg::format::Pixel::RGB24, output.0, output.1);
        scaler.run(&frame, &mut from_frame).unwrap();
        let mut from_owned =
            ffmpeg::frame::Video::new(ffmpeg::format::Pixel::RGB24, output.0, output.1);
        scaler.run_owned_rgba(pixels, &mut from_owned).unwrap();
        let result = rgb_pixels(&from_frame);
        assert_eq!(result, rgb_pixels(&from_owned));
        result
    }

    #[test]
    fn reduction_preserves_thin_stems_at_both_sampling_phases() {
        for vertical in [false, true] {
            for phase in 0..2 {
                let pixels = rgba_pixels(32, 24, |x, y| {
                    let value = if (if vertical { x } else { y }) % 8 == phase {
                        0
                    } else {
                        255
                    };
                    [value, value, value, 255]
                });
                let result = scale_both(&pixels, (32, 24), (16, 12));
                for y in 0..12 {
                    for x in 0..16 {
                        let expected = if (if vertical { x } else { y }) % 4 == 0 {
                            128
                        } else {
                            255
                        };
                        let actual = result[(y * 16 + x) * 3];
                        assert!(
                            actual.abs_diff(expected) <= 1,
                            "stem spread at ({x}, {y}), phase {phase}, vertical {vertical}: {actual}"
                        );
                    }
                }
            }
        }
    }

    #[test]
    fn fractional_reduction_preserves_covered_stem_contrast() {
        for vertical in [false, true] {
            let pixels = rgba_pixels(40, 30, |x, y| {
                let value = if (if vertical { x } else { y }) == 7 {
                    0
                } else {
                    255
                };
                [value, value, value, 255]
            });
            let result = scale_both(&pixels, (40, 30), (16, 12));
            for y in 0..12 {
                for x in 0..16 {
                    // Source pixel 7 overlaps output footprints [5, 7.5] and
                    // [7.5, 10] equally: each covers 20% black and 80% white.
                    let position = if vertical { x } else { y };
                    let expected = if position == 2 || position == 3 {
                        204
                    } else {
                        255
                    };
                    assert!(result[(y * 16 + x) * 3].abs_diff(expected) <= 1);
                }
            }
        }
        // Neither row layout is tightly packed at this fractional ratio.
        let pixels = rgba_pixels(41, 29, |x, y| {
            [(x * 5) as u8, (y * 7) as u8, (x + y) as u8, 255]
        });
        scale_both(&pixels, (41, 29), (17, 13));
    }

    #[test]
    fn native_and_upscale_conversion_preserve_caller_filter() {
        crate::ffmpeg_util::ensure_ffmpeg_initialized().unwrap();
        let pixels = rgba_pixels(19, 13, |x, y| {
            [(x * 13) as u8, (y * 19) as u8, (x * 3 + y * 5) as u8, 255]
        });
        let frame = rgba_frame(&pixels, 19, 13);
        for output in [(19, 13), (38, 26), (17, 15)] {
            let mut scaler = OutputScaler::get(
                ffmpeg::format::Pixel::RGBA,
                19,
                13,
                ffmpeg::format::Pixel::RGB24,
                output.0,
                output.1,
                ffmpeg::software::scaling::Flags::POINT,
            )
            .unwrap();
            let mut expected_scaler = ffmpeg::software::scaling::Context::get(
                ffmpeg::format::Pixel::RGBA,
                19,
                13,
                ffmpeg::format::Pixel::RGB24,
                output.0,
                output.1,
                ffmpeg::software::scaling::Flags::POINT,
            )
            .unwrap();
            let mut actual =
                ffmpeg::frame::Video::new(ffmpeg::format::Pixel::RGB24, output.0, output.1);
            let mut expected =
                ffmpeg::frame::Video::new(ffmpeg::format::Pixel::RGB24, output.0, output.1);
            scaler.run(&frame, &mut actual).unwrap();
            expected_scaler.run(&frame, &mut expected).unwrap();
            assert_eq!(rgb_pixels(&actual), rgb_pixels(&expected));
            if output == (19, 13) {
                let native = pixels[..19 * 13 * 4]
                    .chunks_exact(4)
                    .flat_map(|pixel| pixel[..3].iter().copied())
                    .collect::<Vec<_>>();
                assert_eq!(rgb_pixels(&actual), native);
            }
        }
    }

    #[test]
    fn rgb48_reduction_preserves_thin_stroke_coverage_and_channel_precision() {
        crate::ffmpeg_util::ensure_ffmpeg_initialized().unwrap();
        let background = [60_000_u16, 40_000, 65_000];
        let stroke = [1_000_u16, 8_192, 12_345];
        let average: [u16; 3] = std::array::from_fn(|channel| {
            (u32::from(background[channel]) + u32::from(stroke[channel])).div_ceil(2) as u16
        });
        let control = |color: [u16; 3]| {
            let mut source = ffmpeg::frame::Video::new(ffmpeg::format::Pixel::RGB48LE, 32, 24);
            let stride = source.stride(0);
            source.data_mut(0).fill(0);
            for row in source.data_mut(0).chunks_mut(stride).take(24) {
                for pixel in row[..32 * 6].chunks_exact_mut(6) {
                    for (channel, value) in pixel.chunks_exact_mut(2).zip(color) {
                        channel.copy_from_slice(&value.to_le_bytes());
                    }
                }
            }
            // Use the preexisting BICUBIC path with a constant image: there is
            // no geometric coverage decision to reproduce or accommodate.
            // libswscale's RGB48 scaling converts through Y/UV with 15-bit
            // forward and 13-bit inverse matrix coefficients. Its existing
            // round-trip color bias remains even with ACCURATE_RND.
            let mut scaler = ffmpeg::software::scaling::Context::get(
                ffmpeg::format::Pixel::RGB48LE,
                32,
                24,
                ffmpeg::format::Pixel::RGB48LE,
                16,
                12,
                ffmpeg::software::scaling::Flags::BICUBIC,
            )
            .unwrap();
            let mut output = ffmpeg::frame::Video::new(ffmpeg::format::Pixel::RGB48LE, 16, 12);
            scaler.run(&source, &mut output).unwrap();
            let converted: [u16; 3] = std::array::from_fn(|channel| {
                let from = channel * 2;
                u16::from_le_bytes([output.data(0)[from], output.data(0)[from + 1]])
            });
            for (&actual, &expected) in converted.iter().zip(&color) {
                // One 12-bit code is 16 codes in this 16-bit buffer. This tight
                // precision check bounds the quantized matrix round trip and
                // rejects an 8-bit intermediate for these distinct colors.
                assert!(
                    actual.abs_diff(expected) <= 16,
                    "RGB48 matrix precision: {actual} vs {expected}"
                );
            }
            converted
        };
        let converted_background = control(background);
        let converted_average = control(average);
        for vertical in [false, true] {
            for phase in 0..2 {
                let mut source = ffmpeg::frame::Video::new(ffmpeg::format::Pixel::RGB48LE, 32, 24);
                let stride = source.stride(0);
                source.data_mut(0).fill(0);
                for y in 0..24 {
                    for x in 0..32 {
                        let position = if vertical { x } else { y };
                        let color = if position % 8 == phase {
                            stroke
                        } else {
                            background
                        };
                        let pixel = &mut source.data_mut(0)[y * stride + x * 6..][..6];
                        for (channel, value) in pixel.chunks_exact_mut(2).zip(color) {
                            channel.copy_from_slice(&value.to_le_bytes());
                        }
                    }
                }
                let mut scaler = recording_scaler(
                    ffmpeg::format::Pixel::RGB48LE,
                    32,
                    24,
                    ffmpeg::format::Pixel::RGB48LE,
                    16,
                    12,
                    ffmpeg::software::scaling::Flags::BICUBIC,
                )
                .unwrap();
                let mut output = ffmpeg::frame::Video::new(ffmpeg::format::Pixel::RGB48LE, 16, 12);
                scaler.run(&source, &mut output).unwrap();
                let stride = output.stride(0);
                for y in 0..12 {
                    for x in 0..16 {
                        let covered = (if vertical { x } else { y }) % 4 == 0;
                        let pixel = &output.data(0)[y * stride + x * 6..][..6];
                        for (channel, bytes) in pixel.chunks_exact(2).enumerate() {
                            let expected = if covered {
                                converted_average[channel]
                            } else {
                                converted_background[channel]
                            };
                            let actual = u16::from_le_bytes([bytes[0], bytes[1]]);
                            // Isolate geometric coverage from that existing
                            // color interpretation. Keep the two-code bound on
                            // filtering/rounding rather than broadening it to
                            // absorb the matrix's constant-color error.
                            assert!(
                                actual.abs_diff(expected) <= 2,
                                "RGB48 coverage at ({x}, {y}), channel={channel}, phase={phase}, vertical={vertical}: {actual} vs {expected}"
                            );
                        }
                    }
                }
            }
        }
    }
}
