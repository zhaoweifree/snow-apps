use super::{convert::*, handles::*, style_exports::*, types::*};
use snow_draw_engine::{
    ActiveTool, InputEvent, MagnifierStyle, Modifiers, Point, PointerButton, PointerButtons,
    PointerDevice, PointerEvent, PointerEventType, Runtime, SceneDisplayItem, ViewportConfig,
    ViewportId,
};

#[test]
fn magnifier_abi_layout_tool_and_all_arrowheads_round_trip() {
    assert_eq!(SnowActiveTool::Magnifier as u32, 19);
    assert_eq!(SnowStyleToolbarSource::DefaultMagnifier as u32, 30);
    assert_eq!(SnowStyleToolbarSource::SelectedMagnifier as u32, 31);
    assert_eq!(SnowSceneDisplayItemKind::Magnifier as u32, 9);
    assert_eq!(SnowOverlayRectKind::MagnifierMoveHandle as u32, 15);
    assert_eq!(SnowOverlayRectKind::MagnifierSelectionFrame as u32, 16);
    assert_eq!(std::mem::size_of::<SnowMagnifierStyle>(), 80);
    assert_eq!(std::mem::offset_of!(SnowMagnifierStyle, arrow_ratio), 72);
    assert_eq!(std::mem::offset_of!(SnowMagnifierStyle, corner_radii), 40);
    assert_eq!(std::mem::offset_of!(SnowMagnifierStyle, stroke_width), 8);
    assert_eq!(std::mem::offset_of!(SnowMagnifierStyle, factor), 16);
    assert_eq!(
        std::mem::offset_of!(SnowMagnifierStyle, connection_type),
        24
    );
    assert_eq!(std::mem::offset_of!(SnowMagnifierStyle, source_outline), 28);
    assert_eq!(std::mem::size_of::<SnowMagnifierGeometry>(), 48);
    assert_eq!(
        std::mem::offset_of!(SnowMagnifierGeometry, connection_type),
        40
    );
    assert_eq!(
        std::mem::offset_of!(SnowMagnifierGeometry, source_outline),
        44
    );
    assert_eq!(SnowMagnifierConnectionType::None as u32, 0);
    assert_eq!(SnowMagnifierConnectionType::LeaderLine as u32, 1);
    assert_eq!(SnowMagnifierConnectionType::Gradient as u32, 2);
    assert_eq!(
        std::mem::offset_of!(SnowMagnifierStyle, leader_arrowhead),
        32
    );
    assert_eq!(
        snow_active_tool_to_rust(SnowActiveTool::Magnifier),
        ActiveTool::Magnifier
    );
    assert_eq!(
        snow_active_tool_mask_to_rust(1 << 19),
        ActiveTool::Magnifier.policy_bit()
    );
    for raw in 0..=15 {
        let head: SnowArrowhead = unsafe { std::mem::transmute(raw) };
        let style = SnowMagnifierStyle {
            leader_arrowhead: head,
            factor: 3.5,
            arrow_ratio: 2.3,
            corner_radii: SnowCornerRadii {
                top_left: 3.0,
                top_right: 6.0,
                bottom_right: 12.0,
                bottom_left: 24.0,
            },
            ..Default::default()
        };
        assert_eq!(SnowMagnifierStyle::from(MagnifierStyle::from(style)), style);
    }
    for connection_type in [
        SnowMagnifierConnectionType::None,
        SnowMagnifierConnectionType::LeaderLine,
        SnowMagnifierConnectionType::Gradient,
    ] {
        for source_outline in [0, 1] {
            let style = SnowMagnifierStyle {
                connection_type,
                source_outline,
                ..Default::default()
            };
            assert_eq!(SnowMagnifierStyle::from(MagnifierStyle::from(style)), style);
        }
    }
}

#[test]
fn magnifier_ffi_invalid_values_and_selected_intent_leave_defaults_unchanged() {
    let mut runtime = SnowRuntimeImpl {
        runtime: Runtime::default(),
    };
    let id = runtime
        .runtime
        .create_viewport(ViewportConfig::default())
        .unwrap();
    runtime
        .runtime
        .set_viewport_active_tool(id, ActiveTool::Magnifier)
        .unwrap();
    let mut viewport = SnowViewportImpl { id };
    let mut changed = std::ptr::null_mut();
    let original = runtime
        .runtime
        .viewport_style_toolbar_state(id)
        .unwrap()
        .magnifier_style;
    unsafe {
        for raw in [-1_i32, 3, i32::MAX] {
            let mut invalid = SnowMagnifierStyle::default();
            std::ptr::addr_of_mut!(invalid.connection_type)
                .cast::<i32>()
                .write(raw);
            assert_eq!(
                snow_viewport_set_magnifier_style_patch_ex(
                    &mut runtime,
                    &mut viewport,
                    &invalid,
                    SNOW_MAGNIFIER_STYLE_PROPERTY_CONNECTION_TYPE,
                    1,
                    &mut changed
                ),
                SnowError::InvalidArgument
            );
            assert_eq!(
                runtime
                    .runtime
                    .viewport_style_toolbar_state(id)
                    .unwrap()
                    .magnifier_style,
                original
            );
            assert!(changed.is_null());
        }
        for style in [
            SnowMagnifierStyle {
                arrow_ratio: f64::NAN,
                ..Default::default()
            },
            SnowMagnifierStyle {
                arrow_ratio: 0.4,
                ..Default::default()
            },
            SnowMagnifierStyle {
                arrow_ratio: 3.1,
                ..Default::default()
            },
            SnowMagnifierStyle {
                factor: f64::NAN,
                ..Default::default()
            },
            SnowMagnifierStyle {
                factor: 0.9,
                ..Default::default()
            },
            SnowMagnifierStyle {
                factor: 10.1,
                ..Default::default()
            },
            SnowMagnifierStyle {
                stroke_width: 72.1,
                ..Default::default()
            },
            SnowMagnifierStyle {
                source_outline: 2,
                ..Default::default()
            },
            SnowMagnifierStyle {
                corner_radii: SnowCornerRadii {
                    bottom_right: -1.0,
                    ..Default::default()
                },
                ..Default::default()
            },
            SnowMagnifierStyle {
                corner_radii: SnowCornerRadii {
                    top_left: f64::NAN,
                    ..Default::default()
                },
                ..Default::default()
            },
        ] {
            assert_eq!(
                snow_viewport_set_magnifier_style_patch_ex(
                    &mut runtime,
                    &mut viewport,
                    &style,
                    63,
                    1,
                    &mut changed
                ),
                SnowError::InvalidArgument
            );
            assert_eq!(
                runtime
                    .runtime
                    .viewport_style_toolbar_state(id)
                    .unwrap()
                    .magnifier_style,
                original
            );
        }
        let style = SnowMagnifierStyle {
            factor: 5.0,
            ..Default::default()
        };
        assert_eq!(
            snow_viewport_set_magnifier_style_patch_ex(
                &mut runtime,
                &mut viewport,
                &style,
                8,
                0,
                &mut changed
            ),
            SnowError::InvalidState
        );
        assert_eq!(
            runtime
                .runtime
                .viewport_style_toolbar_state(id)
                .unwrap()
                .magnifier_style,
            original
        );
        assert_eq!(
            snow_viewport_set_magnifier_style_patch_ex(
                &mut runtime,
                &mut viewport,
                &style,
                8,
                2,
                &mut changed
            ),
            SnowError::InvalidArgument
        );
        let mut invalid = style;
        std::ptr::addr_of_mut!(invalid.shape)
            .cast::<i32>()
            .write(99);
        assert_eq!(
            snow_viewport_set_magnifier_style_patch_ex(
                &mut runtime,
                &mut viewport,
                &invalid,
                63,
                1,
                &mut changed
            ),
            SnowError::InvalidArgument
        );
        invalid = style;
        std::ptr::addr_of_mut!(invalid.leader_arrowhead)
            .cast::<i32>()
            .write(99);
        assert_eq!(
            snow_viewport_set_magnifier_style_patch_ex(
                &mut runtime,
                &mut viewport,
                &invalid,
                63,
                1,
                &mut changed
            ),
            SnowError::InvalidArgument
        );
    }
}

fn pointer(runtime: &mut Runtime, viewport: ViewportId, kind: PointerEventType, x: f64, y: f64) {
    runtime
        .process_input(
            viewport,
            InputEvent::Pointer(PointerEvent {
                pressure: None,
                timestamp_ms: 0,
                pointer_id: 1,
                event_type: kind,
                device: PointerDevice::Mouse,
                position: Point::new(x + 400.0, y + 300.0),
                button: Some(PointerButton::Primary),
                buttons: PointerButtons(PointerButtons::PRIMARY),
                modifiers: Modifiers::default(),
            }),
        )
        .unwrap();
}

#[test]
fn magnifier_scene_bridge_preserves_source_and_complete_straight_leader_geometry() {
    let mut runtime = Runtime::default();
    let viewport = runtime.create_viewport(ViewportConfig::default()).unwrap();
    runtime
        .set_viewport_surface_size(viewport, 800, 600)
        .unwrap();
    runtime
        .set_viewport_active_tool(viewport, ActiveTool::Magnifier)
        .unwrap();
    pointer(&mut runtime, viewport, PointerEventType::Down, -50.0, -40.0);
    pointer(&mut runtime, viewport, PointerEventType::Move, 50.0, 40.0);
    pointer(&mut runtime, viewport, PointerEventType::Up, 50.0, 40.0);
    let patch = runtime.acquire_patch(viewport, None).unwrap();
    let id = patch
        .scene
        .ops
        .iter()
        .flat_map(|op| &op.insert_items)
        .find_map(|item| {
            if let SceneDisplayItem::Magnifier(value) = item {
                Some(value.lens.id)
            } else {
                None
            }
        })
        .unwrap();
    runtime
        .set_viewport_active_tool(viewport, ActiveTool::Select)
        .unwrap();
    runtime
        .select_element_with_viewport_changes(
            viewport,
            snow_draw_engine::ElementId {
                index: id.index,
                generation: id.generation,
            },
        )
        .unwrap();
    pointer(&mut runtime, viewport, PointerEventType::Down, 75.0, 0.0);
    pointer(&mut runtime, viewport, PointerEventType::Move, 275.0, 0.0);
    pointer(&mut runtime, viewport, PointerEventType::Up, 275.0, 0.0);
    for raw in 0..=15 {
        let head: SnowArrowhead = unsafe { std::mem::transmute(raw) };
        let style = SnowMagnifierStyle {
            leader_arrowhead: head,
            arrow_ratio: 0.5 + f64::from(raw % 6) * 0.5,
            connection_type: SnowMagnifierConnectionType::LeaderLine,
            ..Default::default()
        };
        runtime
            .set_viewport_magnifier_style_patch(
                viewport,
                style.into(),
                SNOW_MAGNIFIER_STYLE_PROPERTY_LEADER_ARROWHEAD
                    | SNOW_MAGNIFIER_STYLE_PROPERTY_ARROW_RATIO
                    | SNOW_MAGNIFIER_STYLE_PROPERTY_CONNECTION_TYPE,
                false,
            )
            .unwrap();
        let patch = runtime.acquire_patch(viewport, None).unwrap();
        let item = patch
            .scene
            .ops
            .iter()
            .flat_map(|op| &op.insert_items)
            .find(|item| matches!(item, SceneDisplayItem::Magnifier(_)))
            .unwrap();
        let converted = snow_scene_patch_item_from_rust(item, false);
        let view = converted.view;
        assert_eq!(
            view.corner_radii,
            SnowMagnifierStyle::default().corner_radii
        );
        assert_eq!(view.kind, SnowSceneDisplayItemKind::Magnifier);
        assert_eq!(
            view.magnifier.connection_type,
            SnowMagnifierConnectionType::LeaderLine
        );
        assert_eq!(view.magnifier.source_outline, 1);
        assert_eq!((view.center_x, view.center_y), (200.0, 0.0));
        assert_eq!(
            (
                view.magnifier.source_center_x,
                view.magnifier.source_center_y
            ),
            (0.0, 0.0)
        );
        assert_eq!(
            (
                view.magnifier.source_width,
                view.magnifier.source_height,
                view.magnifier.magnification_factor
            ),
            (100.0, 80.0, 2.0)
        );
        assert_eq!(view.arrow_type, SnowArrowType::Straight);
        assert_eq!(view.arrow_shaft_type, SnowArrowShaftType::Plain);
        assert_eq!(view.arrow_ratio, style.arrow_ratio);
        assert_eq!(view.arrow_start_head, SnowArrowhead::None);
        assert_eq!(view.arrow_end_head, head);
        assert_eq!(view.arrow_point_count, 2);
        assert_eq!(view.arrow_path_command_count, 2);
        assert_eq!(view.arrowhead_primitive_count > 0, raw != 0);
        let points = unsafe {
            std::slice::from_raw_parts(view.arrow_points, view.arrow_point_count as usize)
        };
        assert_eq!((points[1].x, points[1].y), (0.0, 0.0));
    }
}
