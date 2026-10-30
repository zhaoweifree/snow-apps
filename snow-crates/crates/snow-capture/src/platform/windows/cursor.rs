use windows::Win32::Graphics::Dxgi::{
    DXGI_OUTDUPL_FRAME_INFO, DXGI_OUTDUPL_POINTER_POSITION, DXGI_OUTDUPL_POINTER_SHAPE_INFO,
    DXGI_OUTDUPL_POINTER_SHAPE_TYPE, DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR,
    DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR, DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME,
};

use snow_cursor::{
    CursorCompositionMode, CursorShape, CursorShapeCapture, CursorSnapshot,
    WindowsCrosshairNormalizer,
};

pub(crate) fn sample_dxgi_cursor(
    position: Option<DXGI_OUTDUPL_POINTER_POSITION>,
    native_shape: Option<&CursorShape>,
    output_origin: (i32, i32),
    normalizer: &mut WindowsCrosshairNormalizer,
) -> Option<CursorSnapshot> {
    let pointer = position?;
    let Some(shape) = native_shape else {
        return (!pointer.Visible.as_bool()).then_some(CursorSnapshot {
            absolute_x: 0,
            absolute_y: 0,
            visible: false,
            shape: CursorShapeCapture::Unavailable,
        });
    };

    // DXGI gives the native image's top-left, not the cursor's hotspot. Resolve
    // that hotspot before substituting an image whose hotspot can differ.
    let absolute_x = output_origin
        .0
        .saturating_add(pointer.Position.x)
        .saturating_add(shape.hotspot_x as i32);
    let absolute_y = output_origin
        .1
        .saturating_add(pointer.Position.y)
        .saturating_add(shape.hotspot_y as i32);
    Some(CursorSnapshot {
        absolute_x,
        absolute_y,
        visible: pointer.Visible.as_bool(),
        shape: CursorShapeCapture::Captured(normalizer.normalize_shape(shape)),
    })
}

pub(crate) fn update_dxgi_pointer_position(
    position: &mut Option<DXGI_OUTDUPL_POINTER_POSITION>,
    frame_info: &DXGI_OUTDUPL_FRAME_INFO,
) {
    // Desktop-only updates (for example, typing) contain no valid pointer
    // position or visibility. Preserve the last mouse update in those frames.
    // https://learn.microsoft.com/windows/win32/api/dxgi1_2/ns-dxgi1_2-dxgi_outdupl_frame_info
    if frame_info.LastMouseUpdateTime != 0 {
        *position = Some(frame_info.PointerPosition);
    }
}

pub(crate) fn decode_dxgi_pointer_shape(
    shape_info: &DXGI_OUTDUPL_POINTER_SHAPE_INFO,
    buffer: &[u8],
) -> Option<CursorShape> {
    let hotspot_x = shape_info.HotSpot.x.max(0) as u32;
    let hotspot_y = shape_info.HotSpot.y.max(0) as u32;
    let width = shape_info.Width;
    let height = shape_info.Height;
    let pitch = shape_info.Pitch as usize;

    match DXGI_OUTDUPL_POINTER_SHAPE_TYPE(shape_info.Type as i32) {
        DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR => Some(CursorShape::from_rgba(
            hotspot_x,
            hotspot_y,
            width,
            height,
            CursorCompositionMode::AlphaBlend,
            decode_color_pointer_rgba(width, height, pitch, buffer)?,
        )),
        DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR => Some(CursorShape::from_rgba(
            hotspot_x,
            hotspot_y,
            width,
            height,
            CursorCompositionMode::MaskedColor,
            decode_color_pointer_rgba(width, height, pitch, buffer)?,
        )),
        DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME => Some(decode_monochrome_dxgi_shape(
            hotspot_x, hotspot_y, width, height, pitch, buffer,
        )?),
        _ => None,
    }
}

fn decode_color_pointer_rgba(
    width: u32,
    height: u32,
    pitch: usize,
    buffer: &[u8],
) -> Option<Vec<u8>> {
    let row_bytes = (width as usize).checked_mul(4)?;
    let total_bytes = pitch.checked_mul(height as usize)?;
    if pitch < row_bytes || buffer.len() < total_bytes {
        return None;
    }

    let mut rgba = vec![0u8; row_bytes.checked_mul(height as usize)?];
    for y in 0..height as usize {
        let src_row = y.checked_mul(pitch)?;
        let dst_row = y.checked_mul(row_bytes)?;
        for x in 0..width as usize {
            let src = src_row.checked_add(x.checked_mul(4)?)?;
            let dst = dst_row.checked_add(x.checked_mul(4)?)?;
            rgba[dst] = buffer[src + 2];
            rgba[dst + 1] = buffer[src + 1];
            rgba[dst + 2] = buffer[src];
            rgba[dst + 3] = buffer[src + 3];
        }
    }
    Some(rgba)
}

fn decode_monochrome_dxgi_shape(
    hotspot_x: u32,
    hotspot_y: u32,
    width: u32,
    stacked_height: u32,
    pitch: usize,
    buffer: &[u8],
) -> Option<CursorShape> {
    // DXGI's height includes both one-bit planes: AND above XOR. Treating
    // it as the visible height rejects valid buffers and retains a stale
    // cursor shape when switching from a color cursor to an inverse cursor.
    // Microsoft Desktop Duplication sample, OutputManager::ProcessMonoMask:
    // https://github.com/microsoft/Windows-classic-samples/blob/main/Samples/DXGIDesktopDuplication/cpp/OutputManager.cpp
    if width == 0
        || stacked_height < 2
        || !stacked_height.is_multiple_of(2)
        || pitch < (width as usize).div_ceil(8)
    {
        return None;
    }
    let height = stacked_height / 2;
    let half_len = pitch.checked_mul(height as usize)?;
    let total_len = half_len.checked_mul(2)?;
    if buffer.len() < total_len {
        return None;
    }

    let mut rgba = vec![0u8; checked_rgba_len(width, height)?];
    for y in 0..height as usize {
        for x in 0..width as usize {
            let and_set = monochrome_mask_bit(buffer, pitch, y, x);
            let xor_set = monochrome_mask_bit(&buffer[half_len..], pitch, y, x);
            let (r, g, b, a) = match (and_set, xor_set) {
                (false, false) => (0, 0, 0, 0),
                (false, true) => (255, 255, 255, 0),
                (true, false) => (0, 0, 0, 255),
                (true, true) => (255, 255, 255, 255),
            };
            let dst = (y * width as usize + x).checked_mul(4)?;
            rgba[dst] = r;
            rgba[dst + 1] = g;
            rgba[dst + 2] = b;
            rgba[dst + 3] = a;
        }
    }

    Some(CursorShape::from_rgba(
        hotspot_x,
        hotspot_y,
        width,
        height,
        CursorCompositionMode::MaskedColor,
        rgba,
    ))
}

fn monochrome_mask_bit(buffer: &[u8], pitch: usize, y: usize, x: usize) -> bool {
    let byte_index = y * pitch + x / 8;
    let mask = 0x80u8 >> (x & 7);
    buffer
        .get(byte_index)
        .is_some_and(|byte| (byte & mask) != 0)
}

fn checked_rgba_len(width: u32, height: u32) -> Option<usize> {
    (width as usize)
        .checked_mul(height as usize)?
        .checked_mul(4)
}

#[cfg(test)]
mod tests {
    use super::*;
    use snow_cursor::{AttachedCursorSample, CursorShapeState, crosshair_cursor_shape};
    use windows::Win32::Foundation::POINT;

    fn native_crosshair() -> CursorShape {
        let mut planes = vec![0xff; 32 * 4];
        planes.resize(32 * 4 * 2, 0);
        for y in 6..27 {
            for x in 6..27 {
                if x == 16 || y == 16 {
                    planes[32 * 4 + y * 4 + x / 8] |= 0x80 >> (x & 7);
                }
            }
        }
        // Intentionally different from the replacement hotspot so a premature
        // substitution cannot pass the native-coordinate regression check.
        decode_dxgi_pointer_shape(
            &DXGI_OUTDUPL_POINTER_SHAPE_INFO {
                Type: DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME.0 as u32,
                Width: 32,
                Height: 64,
                Pitch: 4,
                HotSpot: POINT { x: 3, y: 9 },
            },
            &planes,
        )
        .unwrap()
    }

    fn pointer(x: i32, y: i32, visible: bool) -> DXGI_OUTDUPL_POINTER_POSITION {
        DXGI_OUTDUPL_POINTER_POSITION {
            Position: POINT { x, y },
            Visible: visible.into(),
        }
    }

    #[test]
    fn dxgi_crosshair_replacement_preserves_native_absolute_hotspot_and_visibility() {
        let native = native_crosshair();
        let custom = crosshair_cursor_shape(32, 32).unwrap();
        let mut normalizer = WindowsCrosshairNormalizer::with_system_shape(native.clone());
        for visible in [true, false, true] {
            let sample = sample_dxgi_cursor(
                Some(pointer(40, 60, visible)),
                Some(&native),
                (-1000, -200),
                &mut normalizer,
            )
            .unwrap();
            assert_eq!((sample.absolute_x, sample.absolute_y), (-957, -131));
            assert_eq!(sample.visible, visible);
            assert_eq!(sample.shape.shape(), Some(&custom));
            assert_eq!((native.hotspot_x, native.hotspot_y), (3, 9));
        }
    }

    #[test]
    fn dxgi_crosshair_replacement_preserves_other_shapes_and_revisited_transitions() {
        let native = native_crosshair();
        let custom = crosshair_cursor_shape(32, 32).unwrap();
        let mut changed = native.clone();
        let mut pixels = changed.shape_rgba.to_vec();
        pixels[0] = 1;
        changed = CursorShape::from_rgba(
            changed.hotspot_x,
            changed.hotspot_y,
            changed.width,
            changed.height,
            changed.composition_mode,
            pixels,
        );
        let arrow = CursorShape::from_rgba(
            0,
            0,
            1,
            1,
            CursorCompositionMode::AlphaBlend,
            vec![37, 91, 163, 255],
        );
        let mut normalizer = WindowsCrosshairNormalizer::with_system_shape(native.clone());
        for (input, expected) in [
            (&native, &custom),
            (&changed, &changed),
            (&arrow, &arrow),
            (&native, &custom),
        ] {
            let sample = sample_dxgi_cursor(
                Some(pointer(4, 6, true)),
                Some(input),
                (100, 200),
                &mut normalizer,
            )
            .unwrap();
            assert_eq!(sample.shape.shape(), Some(expected));
            assert_eq!(sample.absolute_x, 104 + input.hotspot_x as i32);
            assert_eq!(sample.absolute_y, 206 + input.hotspot_y as i32);
        }
        assert!(sample_dxgi_cursor(None, Some(&native), (0, 0), &mut normalizer).is_none());
        assert!(
            sample_dxgi_cursor(Some(pointer(0, 0, true)), None, (0, 0), &mut normalizer).is_none()
        );
        let hidden =
            sample_dxgi_cursor(Some(pointer(0, 0, false)), None, (0, 0), &mut normalizer).unwrap();
        assert!(!hidden.visible);
        assert_eq!(hidden.shape, CursorShapeCapture::Unavailable);
    }

    #[test]
    fn dxgi_crosshair_replacement_composites_black_and_white_at_clipped_rgba_and_bgra_edges() {
        use crate::frame::Frame;

        let native = native_crosshair();
        let custom = crosshair_cursor_shape(32, 32).unwrap();
        let mut normalizer = WindowsCrosshairNormalizer::with_system_shape(native.clone());
        for bgra in [false, true] {
            for background in [
                [0, 0, 0, 255],
                [255; 4],
                [127, 127, 127, 255],
                [37, 91, 163, 255],
            ] {
                for (x, y) in [(0, 0), (10, 10), (19, 19)] {
                    let observed = sample_dxgi_cursor(
                        Some(pointer(x - 3, y - 9, true)),
                        Some(&native),
                        (0, 0),
                        &mut normalizer,
                    )
                    .unwrap();
                    let stored = if bgra {
                        [background[2], background[1], background[0], background[3]]
                    } else {
                        background
                    };
                    let mut frame = if bgra {
                        Frame::from_bgra8(20, 20, stored.repeat(400)).unwrap()
                    } else {
                        Frame::from_rgba8(20, 20, stored.repeat(400)).unwrap()
                    };
                    frame.metadata.cursor = Some(AttachedCursorSample {
                        x: observed.absolute_x,
                        y: observed.absolute_y,
                        visible: observed.visible,
                        shape: CursorShapeState::Embedded(observed.shape.into_shape().unwrap()),
                    });
                    assert!(frame.composite_attached_cursor());
                    let mut black = 0;
                    let mut white = 0;
                    for py in 0..20i32 {
                        for px in 0..20i32 {
                            let sx = px - x + custom.hotspot_x as i32;
                            let sy = py - y + custom.hotspot_y as i32;
                            let expected = if (0..32).contains(&sx) && (0..32).contains(&sy) {
                                let offset = ((sy * 32 + sx) * 4) as usize;
                                let pixel = &custom.shape_rgba[offset..offset + 4];
                                if pixel[3] == 255 {
                                    black += usize::from(pixel[0] == 0);
                                    white += usize::from(pixel[0] == 255);
                                    [pixel[0], pixel[1], pixel[2], pixel[3]]
                                } else {
                                    stored
                                }
                            } else {
                                stored
                            };
                            let offset = ((py * 20 + px) * 4) as usize;
                            assert_eq!(&frame.as_bytes()[offset..offset + 4], &expected);
                        }
                    }
                    assert!(
                        black > 0 && white > 0,
                        "clipped cursor must retain both colors"
                    );
                }
            }
        }
    }

    fn pointer_update(timestamp: i64, x: i32, y: i32, visible: bool) -> DXGI_OUTDUPL_FRAME_INFO {
        DXGI_OUTDUPL_FRAME_INFO {
            LastMouseUpdateTime: timestamp,
            PointerPosition: DXGI_OUTDUPL_POINTER_POSITION {
                Position: POINT { x, y },
                Visible: visible.into(),
            },
            ..Default::default()
        }
    }

    #[test]
    fn dxgi_pointer_preserves_stationary_cursor_during_desktop_updates() {
        let mut position = None;
        update_dxgi_pointer_position(&mut position, &pointer_update(1, 120, 80, true));

        // Typing updates the desktop without updating the mouse. DXGI's
        // pointer fields are invalid in those frames, even if zero-filled.
        for timestamp in 2..32 {
            update_dxgi_pointer_position(
                &mut position,
                &DXGI_OUTDUPL_FRAME_INFO {
                    LastPresentTime: timestamp,
                    AccumulatedFrames: 1,
                    ..Default::default()
                },
            );
            let pointer = position.expect("the last mouse update must remain available");
            assert!(
                pointer.Visible.as_bool(),
                "desktop updates must not hide the cursor"
            );
            assert_eq!((pointer.Position.x, pointer.Position.y), (120, 80));
        }
    }

    #[test]
    fn dxgi_pointer_waits_for_first_valid_mouse_update() {
        let mut position = None;
        for visible in [false, true] {
            update_dxgi_pointer_position(&mut position, &pointer_update(0, 120, 80, visible));
            assert!(
                position.is_none(),
                "invalid pointer fields must not initialize state"
            );
        }
        update_dxgi_pointer_position(&mut position, &pointer_update(1, 120, 80, true));
        assert!(position.unwrap().Visible.as_bool());
    }

    #[test]
    fn dxgi_pointer_applies_real_movement_and_visibility_changes() {
        let mut position = None;
        for (timestamp, x, y, visible) in
            [(1, 120, 80, true), (2, 120, 80, false), (3, 240, 90, true)]
        {
            // Pointer-only updates are valid even without a desktop present.
            update_dxgi_pointer_position(&mut position, &pointer_update(timestamp, x, y, visible));
            // Invalid fields must neither hide nor resurrect the cursor.
            update_dxgi_pointer_position(&mut position, &pointer_update(0, -1, -1, !visible));
            let pointer = position.unwrap();
            assert_eq!(pointer.Visible.as_bool(), visible);
            assert_eq!((pointer.Position.x, pointer.Position.y), (x, y));
        }
    }

    #[test]
    fn dxgi_monochrome_decode_produces_expected_pixels() {
        // DXGI reports the combined height of the AND and XOR planes.
        // Exercise the actual decoder entry point, including padded scanlines.
        let info = monochrome_info(2, 4, 2);
        let shape =
            decode_dxgi_pointer_shape(&info, &[0x00, 0xa5, 0xc0, 0xa5, 0x40, 0xa5, 0x40, 0xa5])
                .expect("a valid stacked DXGI mask must decode");

        assert_eq!(shape.width, 2);
        assert_eq!(shape.height, 2);
        assert_eq!((shape.hotspot_x, shape.hotspot_y), (1, 1));
        assert_eq!(shape.composition_mode, CursorCompositionMode::MaskedColor);
        assert_eq!(
            shape.shape_rgba.as_ref(),
            &[
                0, 0, 0, 0, 255, 255, 255, 0, 0, 0, 0, 255, 255, 255, 255, 255
            ],
            "black, white, transparent and inverse pixels must retain their ROPs"
        );
    }

    fn monochrome_info(
        width: u32,
        stacked_height: u32,
        pitch: u32,
    ) -> DXGI_OUTDUPL_POINTER_SHAPE_INFO {
        DXGI_OUTDUPL_POINTER_SHAPE_INFO {
            Type: DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME.0 as u32,
            Width: width,
            Height: stacked_height,
            Pitch: pitch,
            HotSpot: POINT { x: 1, y: 1 },
        }
    }

    #[test]
    fn dxgi_monochrome_crosshair_inverts_gray_and_clips_at_hotspot() {
        use crate::frame::Frame;
        use snow_cursor::{AttachedCursorSample, CursorShapeState};

        let shape = decode_dxgi_pointer_shape(
            &monochrome_info(3, 6, 1),
            &[0xe0, 0xe0, 0xe0, 0x40, 0xe0, 0x40],
        )
        .expect("inverse crosshair must decode");

        for bgra in [false, true] {
            for (x, y) in [(1, 1), (0, 0), (2, 2)] {
                let pixels = [0x7f, 0x7f, 0x7f, 255].repeat(9);
                let mut frame = if bgra {
                    Frame::from_bgra8(3, 3, pixels).unwrap()
                } else {
                    Frame::from_rgba8(3, 3, pixels).unwrap()
                };
                frame.metadata.cursor = Some(AttachedCursorSample {
                    x,
                    y,
                    visible: true,
                    shape: CursorShapeState::Embedded(shape.clone()),
                });
                assert!(frame.composite_attached_cursor());
                for py in 0i32..3 {
                    for px in 0i32..3 {
                        let on_crosshair =
                            (px == x && (py - y).abs() <= 1) || (py == y && (px - x).abs() <= 1);
                        let value = if on_crosshair { 0x80 } else { 0x7f };
                        let offset = ((py * 3 + px) * 4) as usize;
                        assert_eq!(
                            &frame.as_bytes()[offset..offset + 4],
                            &[value, value, value, 255]
                        );
                    }
                }
            }
        }
    }

    #[test]
    fn dxgi_monochrome_rejects_incomplete_planes_and_invalid_layouts() {
        for (width, height, pitch, buffer_len) in [
            (0, 4, 1, 4),
            (2, 0, 1, 0),
            (2, 1, 1, 2),
            (2, 3, 1, 6),
            (9, 4, 1, 8),
            (2, 4, 0, 0),
            (2, 4, 2, 7),
        ] {
            assert!(
                decode_dxgi_pointer_shape(
                    &monochrome_info(width, height, pitch),
                    &vec![0xff; buffer_len],
                )
                .is_none(),
                "invalid monochrome layout: {width}x{height}, pitch {pitch}, bytes {buffer_len}"
            );
        }
    }

    #[test]
    fn dxgi_color_shapes_keep_full_height_and_bgra_channels() {
        for (kind, mode) in [
            (
                DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR,
                CursorCompositionMode::AlphaBlend,
            ),
            (
                DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR,
                CursorCompositionMode::MaskedColor,
            ),
        ] {
            let info = DXGI_OUTDUPL_POINTER_SHAPE_INFO {
                Type: kind.0 as u32,
                Width: 1,
                Height: 2,
                Pitch: 8,
                ..Default::default()
            };
            let shape = decode_dxgi_pointer_shape(
                &info,
                &[1, 2, 3, 0, 99, 99, 99, 99, 4, 5, 6, 255, 99, 99, 99, 99],
            )
            .unwrap();
            assert_eq!((shape.width, shape.height), (1, 2));
            assert_eq!(shape.composition_mode, mode);
            assert_eq!(shape.shape_rgba.as_ref(), &[3, 2, 1, 0, 6, 5, 4, 255]);
        }
    }
}
