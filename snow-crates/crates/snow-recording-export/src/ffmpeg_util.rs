//! Shared FFmpeg helpers used by both `recording` and `editing`.

use std::collections::VecDeque;
use std::sync::OnceLock;

use ffmpeg_next as ffmpeg;

use crate::error::{RecordingExportError as ScreenRecorderError, Result};

/// Initialize FFmpeg exactly once. Thread-safe via `OnceLock`.
pub fn ensure_ffmpeg_initialized() -> Result<()> {
    static INIT: OnceLock<std::result::Result<(), String>> = OnceLock::new();
    INIT.get_or_init(|| ffmpeg::init().map_err(|err| err.to_string()))
        .clone()
        .map_err(|err| ScreenRecorderError::Encode(format!("failed to initialize ffmpeg: {err}")))
}

/// Check whether an FFmpeg error is EAGAIN.
pub fn is_eagain(err: &ffmpeg::Error) -> bool {
    matches!(
        err,
        ffmpeg::Error::Other { errno } if *errno == ffmpeg::error::EAGAIN
    )
}

/// Detach a video frame from buffers still retained by an FFmpeg codec.
///
/// `avcodec_send_frame` is allowed to keep references to the submitted
/// buffers. Calling this before the next write preserves the previous frame
/// while reusing the `AVFrame` wrapper and only allocates when it is shared.
///
/// # Safety
///
/// The frame's format, dimensions, strides and plane pointers must describe valid,
/// reference-counted video storage. FFmpeg may read every plane to detach a shared buffer;
/// changing metadata through `Video`'s safe setters does not establish this invariant.
pub unsafe fn ensure_video_frame_writable(frame: &mut ffmpeg::frame::Video) -> Result<()> {
    let status = unsafe { ffmpeg::ffi::av_frame_make_writable(frame.as_mut_ptr()) };
    if status < 0 {
        return Err(ScreenRecorderError::Encode(format!(
            "failed to make video frame writable: {}",
            ffmpeg::Error::from(status)
        )));
    }
    Ok(())
}

/// Validate reference-counted CPU planes before native code can retain or read them.
/// Safe Video metadata setters can enlarge an image without enlarging its buffers.
pub(crate) fn validate_cpu_video_frame(frame: &ffmpeg::frame::Video) -> Result<()> {
    let invalid = || ScreenRecorderError::InvalidConfig("invalid CPU video frame storage".into());
    // SAFETY: Video owns a live AVFrame. Inspect metadata without making any plane slices.
    let raw = unsafe { &*frame.as_ptr() };
    if raw.width <= 0 || raw.height <= 0 || raw.nb_samples != 0 {
        return Err(invalid());
    }
    let format = frame.format().into();
    let mut row_bytes = [0; 4];
    let mut packed_sizes = [0; 4];
    // SAFETY: Both arrays have the four entries required by the image-layout API.
    // FFmpeg rejects unknown and hardware-only formats without reading frame pixels.
    unsafe {
        if ffmpeg::ffi::av_image_fill_linesizes(row_bytes.as_mut_ptr(), format, raw.width) < 0 {
            return Err(invalid());
        }
        let strides = row_bytes.map(|bytes| bytes as isize);
        if ffmpeg::ffi::av_image_fill_plane_sizes(
            packed_sizes.as_mut_ptr(),
            format,
            raw.height,
            strides.as_ptr(),
        ) < 0
        {
            return Err(invalid());
        }
    }
    for (plane, packed_size) in packed_sizes.into_iter().enumerate() {
        if packed_size == 0 {
            continue;
        }
        let image_row_bytes = usize::try_from(row_bytes[plane]).map_err(|_| invalid())?;
        // PAL8's second plane is a 1024-byte palette with no row stride.
        let (rows, row_bytes) = match packed_size.checked_div(image_row_bytes) {
            Some(rows) => (rows, image_row_bytes),
            None => (1, packed_size),
        };
        let stride = raw.linesize[plane].unsigned_abs() as usize;
        if rows == 0 || (image_row_bytes != 0 && stride < row_bytes) || raw.data[plane].is_null() {
            return Err(invalid());
        }
        let displacement = stride
            .checked_mul(rows - 1)
            .filter(|&bytes| bytes <= isize::MAX as usize)
            .ok_or_else(invalid)?;
        // SAFETY: This only locates the AVBufferRef that owns the plane address.
        let buffer =
            unsafe { ffmpeg::ffi::av_frame_get_plane_buffer(frame.as_ptr(), plane as i32) };
        if buffer.is_null() {
            return Err(invalid());
        }
        // SAFETY: The frame retains this AVBufferRef throughout the validation.
        let buffer = unsafe { &*buffer };
        if buffer.data.is_null() || buffer.size > isize::MAX as usize {
            return Err(invalid());
        }
        let offset = raw.data[plane]
            .addr()
            .checked_sub(buffer.data.addr())
            .ok_or_else(invalid)?;
        let (start, end) = if raw.linesize[plane] < 0 {
            (
                offset.checked_sub(displacement),
                offset.checked_add(row_bytes),
            )
        } else {
            (
                Some(offset),
                offset
                    .checked_add(displacement)
                    .and_then(|end| end.checked_add(row_bytes)),
            )
        };
        if start.is_none() || end.is_none_or(|end| end > buffer.size) {
            return Err(invalid());
        }
    }
    Ok(())
}

struct RgbaFrameLayout {
    row_bytes: usize,
    packed_len: usize,
    stride: usize,
}

fn rgba_frame_layout(frame: &mut ffmpeg::frame::Video) -> Result<RgbaFrameLayout> {
    let invalid = || ScreenRecorderError::Encode("invalid RGBA frame storage".into());
    if frame.format() != ffmpeg::format::Pixel::RGBA
        || frame.width() == 0
        || frame.height() == 0
        || frame.width() > i32::MAX as u32
        || frame.height() > i32::MAX as u32
    {
        return Err(invalid());
    }
    let row_bytes = (frame.width() as usize)
        .checked_mul(4)
        .ok_or_else(invalid)?;
    let packed_len = row_bytes
        .checked_mul(frame.height() as usize)
        .ok_or_else(invalid)?;
    // SAFETY: Video owns a live AVFrame. Read metadata without constructing a plane slice:
    // ffmpeg-next's safe dimension setters can make data_mut() exceed the allocation.
    let (stride, data) = unsafe {
        let raw = &*frame.as_ptr();
        (raw.linesize[0], raw.data[0])
    };
    let stride = usize::try_from(stride).map_err(|_| invalid())?;
    let storage_len = stride
        .checked_mul(frame.height() as usize)
        .filter(|&len| len <= isize::MAX as usize)
        .ok_or_else(invalid)?;
    if stride < row_bytes || data.is_null() {
        return Err(invalid());
    }
    // SAFETY: This only looks up the reference-counted allocation containing plane zero.
    let buffer = unsafe { ffmpeg::ffi::av_frame_get_plane_buffer(frame.as_mut_ptr(), 0) };
    if buffer.is_null() {
        return Err(invalid());
    }
    // SAFETY: The AVBufferRef is owned by the frame and remains live throughout validation.
    let buffer = unsafe { &*buffer };
    let end = data
        .addr()
        .checked_sub(buffer.data.addr())
        .and_then(|offset| offset.checked_add(storage_len));
    if buffer.data.is_null() || end.is_none_or(|end| end > buffer.size) {
        return Err(invalid());
    }
    Ok(RgbaFrameLayout {
        row_bytes,
        packed_len,
        stride,
    })
}

/// Copy a complete, tightly packed RGBA image into a reference-counted RGBA video frame.
///
/// The frame supplies the dimensions and may have padded rows. Invalid storage or a source
/// length other than `width * height * 4` returns an error before writing any pixels.
/// Buffers retained by an encoder or another frame are detached before the copy.
pub fn copy_rgba_into_frame(frame: &mut ffmpeg::frame::Video, rgba: &[u8]) -> Result<()> {
    let layout = rgba_frame_layout(frame)?;
    if rgba.len() != layout.packed_len {
        return Err(ScreenRecorderError::Encode(format!(
            "RGBA size mismatch: expected {} bytes, got {}",
            layout.packed_len,
            rgba.len()
        )));
    }
    // Validate before FFmpeg can copy a shared buffer using its dimensions, then validate
    // again because making it writable may replace the allocation and its row stride.
    // SAFETY: The RGBA layout and its complete plane storage were validated above.
    unsafe { ensure_video_frame_writable(frame)? };
    let layout = rgba_frame_layout(frame)?;
    let dst = frame.data_mut(0);
    if layout.stride == layout.row_bytes {
        dst.copy_from_slice(rgba);
    } else {
        for (row, source) in dst
            .chunks_exact_mut(layout.stride)
            .zip(rgba.chunks_exact(layout.row_bytes))
        {
            row[..layout.row_bytes].copy_from_slice(source);
        }
    }
    Ok(())
}

/// Durations are submitted in presentation order but consumed in decode order.
#[derive(Default)]
pub(crate) struct VideoPacketDurations {
    pending: VecDeque<(i64, i64)>,
    first_pts: Option<i64>,
    first_dts: Option<i64>,
    end_pts: i64,
}

impl VideoPacketDurations {
    pub(crate) fn push_back(&mut self, (pts, duration): (i64, i64)) {
        self.first_pts.get_or_insert(pts);
        self.end_pts = self.end_pts.max(pts.saturating_add(duration));
        self.pending.push_back((pts, duration));
    }

    #[cfg(test)]
    pub(crate) fn len(&self) -> usize {
        self.pending.len()
    }

    #[cfg(test)]
    pub(crate) fn is_empty(&self) -> bool {
        self.pending.is_empty()
    }

    pub(crate) fn apply(&mut self, packet: &mut ffmpeg::Packet, draining: bool) {
        let index = packet.pts().map_or(Some(0), |pts| {
            self.pending.iter().position(|(at, _)| *at == pts)
        });
        if let Some((_, duration)) = index.and_then(|index| self.pending.remove(index)) {
            packet.set_duration(duration);
        }
        if let Some(dts) = packet.dts() {
            self.first_dts.get_or_insert(dts);
        }
        if draining
            && self.pending.is_empty()
            && let (Some(first_pts), Some(first_dts), Some(pts), Some(dts)) =
                (self.first_pts, self.first_dts, packet.pts(), packet.dts())
        {
            // MP4 builds sample durations from consecutive DTS. Its last decode
            // sample must cover the whole presentation span, even when the final
            // still image was emitted earlier because of B-frame reordering.
            // Keep the last CTS no larger than the initial reorder offset so
            // extending the decode tail cannot extend the presentation endpoint.
            let reorder_offset = first_pts.saturating_sub(first_dts).max(0);
            let dts = dts.max(pts.saturating_sub(reorder_offset));
            packet.set_dts(Some(dts));
            let tail = self
                .end_pts
                .saturating_sub(reorder_offset)
                .saturating_sub(dts);
            packet.set_duration(packet.duration().max(tail));
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cpu_video_storage_accepts_padded_planar_packed_palette_and_bottom_up_frames() {
        for format in [
            ffmpeg::format::Pixel::RGBA,
            ffmpeg::format::Pixel::RGB24,
            ffmpeg::format::Pixel::YUV420P,
            ffmpeg::format::Pixel::NV12,
            ffmpeg::format::Pixel::YUV420P10LE,
            ffmpeg::format::Pixel::P010LE,
            ffmpeg::format::Pixel::PAL8,
        ] {
            for (width, height) in [(1, 1), (17, 13)] {
                let mut frame = ffmpeg::frame::Video::new(format, width, height);
                validate_cpu_video_frame(&frame).unwrap();
                for plane in 0..frame.planes() {
                    let rows = frame.plane_height(plane) as usize;
                    let stride = frame.stride(plane);
                    // SAFETY: Move each plane to its last visible row and reverse its
                    // stride, preserving the complete owning allocation and pixels.
                    unsafe {
                        let raw = &mut *frame.as_mut_ptr();
                        raw.data[plane] = raw.data[plane].add((rows - 1) * stride);
                        raw.linesize[plane] = -raw.linesize[plane];
                    }
                }
                validate_cpu_video_frame(&frame).unwrap();
            }
        }
    }

    #[test]
    fn cpu_video_storage_rejects_missing_planes_short_strides_and_enlarged_metadata() {
        let mut empty = ffmpeg::frame::Video::empty();
        empty.set_format(ffmpeg::format::Pixel::YUV420P);
        empty.set_width(16);
        empty.set_height(128);
        assert!(validate_cpu_video_frame(&empty).is_err());
        let mut frame = ffmpeg::frame::Video::new(ffmpeg::format::Pixel::YUV420P, 1, 1);
        frame.set_height(10_000);
        assert!(validate_cpu_video_frame(&frame).is_err());
        for plane in 0..3 {
            let mut frame = ffmpeg::frame::Video::new(ffmpeg::format::Pixel::YUV420P, 16, 16);
            let stride = frame.stride(plane) as i32;
            // SAFETY: Deliberately invalidate one metadata field without freeing or
            // accessing pixels. Validation must reject it before native pixel reads.
            unsafe {
                (*frame.as_mut_ptr()).linesize[plane] = 1;
            }
            assert!(validate_cpu_video_frame(&frame).is_err());
            // SAFETY: Restore the valid stride so this case isolates a missing plane.
            unsafe {
                (*frame.as_mut_ptr()).linesize[plane] = stride;
                (*frame.as_mut_ptr()).data[plane] = std::ptr::null_mut();
            }
            assert!(validate_cpu_video_frame(&frame).is_err());
        }
        let mut palette = ffmpeg::frame::Video::new(ffmpeg::format::Pixel::PAL8, 16, 16);
        // SAFETY: A palette is required even though PAL8 reports one strided plane.
        unsafe {
            (*palette.as_mut_ptr()).data[1] = std::ptr::null_mut();
        }
        assert!(validate_cpu_video_frame(&palette).is_err());
    }

    #[test]
    fn rgba_copy_handles_packed_and_padded_rows_without_changing_padding() {
        for (width, height) in [(1, 1), (3, 2), (8, 3), (17, 2)] {
            let mut frame = ffmpeg::frame::Video::new(ffmpeg::format::Pixel::RGBA, width, height);
            frame.data_mut(0).fill(0xa5);
            let pixels: Vec<_> = (0..width * height * 4).map(|i| (i % 251) as u8).collect();

            copy_rgba_into_frame(&mut frame, &pixels).unwrap();

            let row_bytes = width as usize * 4;
            for (y, row) in frame.data(0).chunks_exact(frame.stride(0)).enumerate() {
                assert_eq!(
                    &row[..row_bytes],
                    &pixels[y * row_bytes..(y + 1) * row_bytes]
                );
                assert!(row[row_bytes..].iter().all(|&byte| byte == 0xa5));
            }
        }
    }

    #[test]
    fn rgba_copy_rejects_incorrect_source_lengths_before_writing() {
        let mut frame = ffmpeg::frame::Video::new(ffmpeg::format::Pixel::RGBA, 1, 1);
        frame.data_mut(0).fill(0xa5);
        for pixels in [&[][..], &[1, 2, 3][..], &[1, 2, 3, 4, 5][..]] {
            assert!(matches!(
                copy_rgba_into_frame(&mut frame, pixels),
                Err(ScreenRecorderError::Encode(_))
            ));
            assert!(frame.data(0).iter().all(|&byte| byte == 0xa5));
        }
    }

    #[test]
    fn rgba_copy_rejects_missing_storage_and_other_pixel_formats() {
        let mut empty = ffmpeg::frame::Video::empty();
        assert!(copy_rgba_into_frame(&mut empty, &[]).is_err());
        empty.set_format(ffmpeg::format::Pixel::RGBA);
        empty.set_width(1);
        empty.set_height(1);
        assert!(copy_rgba_into_frame(&mut empty, &[0; 4]).is_err());

        for format in [ffmpeg::format::Pixel::BGRA, ffmpeg::format::Pixel::YUV420P] {
            let mut frame = ffmpeg::frame::Video::new(format, 2, 2);
            frame.data_mut(0).fill(0xa5);
            assert!(copy_rgba_into_frame(&mut frame, &[0; 16]).is_err());
            assert!(frame.data(0).iter().all(|&byte| byte == 0xa5));
        }
    }

    #[test]
    fn rgba_copy_rejects_dimensions_that_exceed_the_backing_allocation() {
        let mut frame = ffmpeg::frame::Video::new(ffmpeg::format::Pixel::RGBA, 1, 1);
        frame.data_mut(0).fill(0xa5);
        let mut retained = ffmpeg::frame::Video::empty();
        // SAFETY: Both frames are live, and the destination has no existing buffers.
        assert_eq!(
            unsafe { ffmpeg::ffi::av_frame_ref(retained.as_mut_ptr(), frame.as_ptr()) },
            0
        );
        for width in [0, u32::MAX] {
            frame.set_width(width);
            assert!(copy_rgba_into_frame(&mut frame, &[0; 4]).is_err());
        }
        let too_wide = frame.stride(0) as u32 / 4 + 1;
        frame.set_width(too_wide);
        assert!(copy_rgba_into_frame(&mut frame, &vec![0; too_wide as usize * 4]).is_err());
        frame.set_width(1);
        for height in [0, u32::MAX] {
            frame.set_height(height);
            assert!(copy_rgba_into_frame(&mut frame, &[0; 4]).is_err());
        }
        frame.set_height(10_000);
        assert!(copy_rgba_into_frame(&mut frame, &[0; 40_000]).is_err());
        frame.set_height(1);
        assert!(frame.data(0).iter().all(|&byte| byte == 0xa5));
        assert!(retained.data(0).iter().all(|&byte| byte == 0xa5));
        assert_eq!(frame.data(0).as_ptr(), retained.data(0).as_ptr());
    }

    #[test]
    fn rgba_copy_preserves_buffers_retained_by_another_frame() {
        let mut frame = ffmpeg::frame::Video::new(ffmpeg::format::Pixel::RGBA, 8, 2);
        frame.data_mut(0).fill(0x11);
        let mut retained = ffmpeg::frame::Video::empty();
        // SAFETY: Both frames are live, and the destination has no existing buffers.
        assert_eq!(
            unsafe { ffmpeg::ffi::av_frame_ref(retained.as_mut_ptr(), frame.as_ptr()) },
            0
        );

        copy_rgba_into_frame(&mut frame, &[0xee; 64]).unwrap();

        assert!(retained.data(0).iter().all(|&byte| byte == 0x11));
        assert!(frame.data(0).iter().all(|&byte| byte == 0xee));
    }

    #[test]
    fn rgba_copy_can_read_from_a_retained_reference_to_its_own_buffer() {
        let mut frame = ffmpeg::frame::Video::new(ffmpeg::format::Pixel::RGBA, 8, 2);
        frame.data_mut(0).fill(0x11);
        let mut retained = ffmpeg::frame::Video::empty();
        // SAFETY: Both frames are live, and the destination has no existing buffers.
        assert_eq!(
            unsafe { ffmpeg::ffi::av_frame_ref(retained.as_mut_ptr(), frame.as_ptr()) },
            0
        );
        let source = retained.data(0);

        copy_rgba_into_frame(&mut frame, source).unwrap();

        assert_eq!(frame.data(0), source);
        assert_ne!(frame.data(0).as_ptr(), source.as_ptr());
    }

    #[test]
    fn making_a_retained_frame_writable_preserves_submitted_pixels() {
        let mut frame = ffmpeg::frame::Video::new(ffmpeg::format::Pixel::YUV420P, 16, 16);
        frame.data_mut(0).fill(0x11);

        // Model the reference an encoder may retain after avcodec_send_frame.
        let mut retained = ffmpeg::frame::Video::empty();
        let ref_status =
            unsafe { ffmpeg::ffi::av_frame_ref(retained.as_mut_ptr(), frame.as_ptr()) };
        assert_eq!(ref_status, 0);

        // SAFETY: The frame has the unchanged layout and storage allocated by Video::new.
        unsafe { ensure_video_frame_writable(&mut frame).unwrap() };
        frame.data_mut(0).fill(0xee);

        assert!(retained.data(0).iter().all(|byte| *byte == 0x11));
        assert!(frame.data(0).iter().all(|byte| *byte == 0xee));
    }
}
