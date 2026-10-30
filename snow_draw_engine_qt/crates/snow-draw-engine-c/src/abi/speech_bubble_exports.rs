use crate::abi::{convert::*, handles::*, types::*};
use snow_draw_engine::{Point, TextElementInfo};
use snow_draw_engine_document::{FillStyle, RectangleData, RectangleElementKind};

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct SnowSpeechBubbleTextLayoutRequest {
    pub info: SnowTextElementInfo,
    pub style: SnowTextStyle,
    pub key: u64,
}
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct SnowSpeechBubbleTextLayoutMetrics {
    pub text_id: SnowElementId,
    pub key: u64,
    pub size: SnowTextLayoutSize,
    pub minimum_grapheme_width: f64,
    pub text_ink_reserve: f64,
}

/// # Safety
/// Runtime must be live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_runtime_speech_bubble_text_count(runtime: SnowRuntime) -> u32 {
    ffi_value(0, || {
        with_runtime_ref(runtime, |engine| {
            Ok(engine.speech_bubble_text_count() as u32)
        })
        .unwrap_or(0)
    })
}
/// # Safety
/// Handles must be live and outputs writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_get_speech_bubble_text_target(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    use_point: u8,
    x: f64,
    y: f64,
    out_info: *mut SnowTextElementInfo,
    out_style: *mut SnowTextStyle,
    out_found: *mut u8,
) -> SnowError {
    ffi_error(|| {
        if out_info.is_null()
            || out_style.is_null()
            || out_found.is_null()
            || (use_point != 0 && (!x.is_finite() || !y.is_finite()))
        {
            return SnowError::InvalidArgument;
        }
        ffi_status(with_runtime_viewport_ref(
            runtime,
            viewport,
            |engine, id| {
                let target = engine
                    .speech_bubble_text_target(id, (use_point != 0).then_some(Point::new(x, y)))
                    .map_err(SnowError::from)?;
                write_out(out_found, u8::from(target.is_some()));
                if let Some((info, style)) = target {
                    write_out(out_info, snow_text_element_info_from_rust(info));
                    write_out(out_style, style.into());
                }
                Ok(())
            },
        ))
    })
}
/// # Safety
/// Handles must be live and outputs writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_take_speech_bubble_text_edit_request(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    out_owner: *mut SnowElementId,
    out_requested: *mut u8,
) -> SnowError {
    ffi_error(|| {
        if out_owner.is_null() || out_requested.is_null() {
            return SnowError::InvalidArgument;
        }
        ffi_status(with_runtime_viewport_mut(
            runtime,
            viewport,
            |engine, id| {
                let target = engine
                    .take_speech_bubble_text_edit_request(id)
                    .map_err(SnowError::from)?;
                write_out(out_requested, u8::from(target.is_some()));
                write_out(
                    out_owner,
                    target.map(snow_element_id_from_rust).unwrap_or_default(),
                );
                Ok(())
            },
        ))
    })
}
/// # Safety
/// Input must be readable and outputs writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_speech_bubble_resolve_text_layout(
    info: *const SnowTextElementInfo,
    minimum_grapheme_width: f64,
    text_ink_reserve: f64,
    wrapped_text_height: f64,
    out_body_width: *mut f64,
    out_body_height: *mut f64,
    out_content_width: *mut f64,
    out_content_height: *mut f64,
) -> SnowError {
    ffi_error(|| {
        if info.is_null()
            || out_body_width.is_null()
            || out_body_height.is_null()
            || out_content_width.is_null()
            || out_content_height.is_null()
        {
            return SnowError::InvalidArgument;
        }
        let info = unsafe { &*info };
        let body = RectangleData {
            text_element_id: None,
            stroke_rendering_style: Default::default(),
            stroke_rendering_seed: 0,
            rectangle_kind: RectangleElementKind::Rectangle,
            highlight_shape: match info.bubble_shape {
                SnowRectangleShape::Rectangle => snow_draw_engine::HighlightShape::Rectangle,
                SnowRectangleShape::Ellipse => snow_draw_engine::HighlightShape::Ellipse,
                SnowRectangleShape::Diamond => snow_draw_engine::HighlightShape::Diamond,
            },
            center: Point::new(info.center_x, info.center_y),
            width: info.bubble_body_width,
            height: info.bubble_body_height,
            rotation: info.rotation,
            stroke_width: info.bubble_stroke_width,
            corner_radii: info.bubble_corner_radii.into(),
            stroke: Default::default(),
            fill: Default::default(),
            fill_style: FillStyle::Solid,
            stroke_style: Default::default(),
            opacity: 1.0,
        };
        match snow_draw_engine::speech_bubble_text_constraints(
            body,
            minimum_grapheme_width,
            text_ink_reserve,
            wrapped_text_height,
        ) {
            Ok(c) => {
                write_out(out_body_width, c.body_width);
                write_out(out_body_height, c.body_height);
                write_out(out_content_width, c.content_width);
                write_out(out_content_height, c.content_height);
                SnowError::Ok
            }
            Err(e) => e.into(),
        }
    })
}
/// # Safety
/// Handles must be live; output storage must hold capacity entries and count writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_get_speech_bubble_text_layout_requests(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    out_items: *mut SnowSpeechBubbleTextLayoutRequest,
    capacity: u32,
    out_count: *mut u32,
) -> SnowError {
    ffi_error(|| {
        if out_count.is_null() || (capacity != 0 && out_items.is_null()) {
            return SnowError::InvalidArgument;
        }
        ffi_status(with_runtime_viewport_ref(
            runtime,
            viewport,
            |engine, id| {
                let requests = engine
                    .speech_bubble_text_layout_requests(id)
                    .map_err(SnowError::from)?;
                write_out(out_count, requests.len() as u32);
                if capacity != 0 {
                    if capacity < requests.len() as u32 {
                        return Err(SnowError::InvalidArgument);
                    }
                    for (i, r) in requests.into_iter().enumerate() {
                        let mut info: TextElementInfo = engine
                            .text_element_info(r.text_id)
                            .map_err(SnowError::from)?;
                        info.bubble_body = Some(r.body);
                        info.center = r.body.center;
                        info.rotation = r.body.rotation;
                        let style = snow_draw_engine::TextStyle {
                            color: r.text.color,
                            font_size: r.text.font_size,
                            bold: r.text.bold,
                            italic: r.text.italic,
                            font_family: r.text.font_family,
                            fill: r.text.fill,
                            fill_style: r.text.fill_style,
                            stroke: r.text.stroke,
                            stroke_width: r.text.stroke_width,
                            corner_radii: r.text.corner_radii,
                            horizontal_align: r.text.horizontal_align,
                            vertical_align: r.text.vertical_align,
                            opacity: r.text.opacity,
                        };
                        write_out(
                            unsafe { out_items.add(i) },
                            SnowSpeechBubbleTextLayoutRequest {
                                info: snow_text_element_info_from_rust(info),
                                style: style.into(),
                                key: r.key,
                            },
                        );
                    }
                }
                Ok(())
            },
        ))
    })
}
/// # Safety
/// Handles must be live; metrics readable for count and changed-viewports output writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_viewport_apply_speech_bubble_text_layout_metrics_ex(
    runtime: SnowRuntime,
    viewport: SnowViewport,
    items: *const SnowSpeechBubbleTextLayoutMetrics,
    count: u32,
    out_changed: *mut SnowChangedViewportList,
) -> SnowError {
    ffi_error(|| {
        if out_changed.is_null() || (count != 0 && items.is_null()) {
            return SnowError::InvalidArgument;
        }
        let items = if count == 0 {
            &[][..]
        } else {
            unsafe { std::slice::from_raw_parts(items, count as usize) }
        };
        let layouts: Vec<_> = items
            .iter()
            .map(|m| {
                (
                    snow_element_id_to_rust(m.text_id),
                    snow_draw_engine::SpeechBubbleTextMeasurement {
                        key: m.key,
                        size: m.size.into(),
                        minimum_grapheme_width: m.minimum_grapheme_width,
                        text_ink_reserve: m.text_ink_reserve,
                    },
                )
            })
            .collect();
        ffi_status(with_runtime_viewport_mut(
            runtime,
            viewport,
            |engine, id| {
                let result = engine
                    .apply_speech_bubble_text_layouts(id, &layouts)
                    .map_err(SnowError::from)?;
                write_changed_viewports(out_changed, result.changed_viewports);
                Ok(())
            },
        ))
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn speech_bubble_ffi_constraints_and_null_validation() {
        for shape in [
            SnowRectangleShape::Rectangle,
            SnowRectangleShape::Ellipse,
            SnowRectangleShape::Diamond,
        ] {
            let info = SnowTextElementInfo {
                bubble_body_width: 20.0,
                bubble_body_height: 10.0,
                bubble_shape: shape,
                bubble_stroke_width: 2.0,
                bubble_corner_radii: SnowCornerRadii {
                    top_left: 100.0,
                    top_right: 50.0,
                    bottom_right: 30.0,
                    bottom_left: 20.0,
                },
                ..Default::default()
            };
            let (mut w, mut h, mut cw, mut ch) = (0.0, 0.0, 0.0, 0.0);
            unsafe {
                assert_eq!(
                    snow_speech_bubble_resolve_text_layout(
                        &info, 30.0, 4.0, 90.0, &mut w, &mut h, &mut cw, &mut ch
                    ),
                    SnowError::Ok
                );
            }
            assert!(w >= 20.0 && h >= 10.0 && cw >= 30.0 - 1e-9 && ch >= 90.0 - 1e-9);
            let previous = (w, h, cw, ch);
            unsafe {
                assert_eq!(
                    snow_speech_bubble_resolve_text_layout(
                        &info,
                        f64::NAN,
                        4.0,
                        90.0,
                        &mut w,
                        &mut h,
                        &mut cw,
                        &mut ch
                    ),
                    SnowError::InvalidArgument
                );
            }
            assert_eq!((w, h, cw, ch), previous);
            unsafe {
                assert_eq!(
                    snow_speech_bubble_resolve_text_layout(
                        std::ptr::null(),
                        30.0,
                        4.0,
                        90.0,
                        &mut w,
                        &mut h,
                        &mut cw,
                        &mut ch
                    ),
                    SnowError::InvalidArgument
                );
            }
        }
    }
    #[test]
    fn speech_bubble_abi_values_preserve_existing_tools_and_shapes() {
        assert_eq!(SnowActiveTool::SpeechBubble as u32, 20);
        assert_eq!(SnowShapeKind::SpeechBubble as u32, 7);
        assert_eq!(SnowStyleToolbarSource::DefaultSpeechBubble as u32, 32);
        assert_eq!(SnowStyleToolbarSource::SelectedSpeechBubble as u32, 33);
        assert_eq!(SnowSceneDisplayItemKind::SpeechBubble as u32, 10);
        assert_eq!(SnowOverlayRectKind::SpeechBubbleAddTailHandle as u32, 17);
    }
}
