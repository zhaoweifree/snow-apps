use super::duplicate_drag_tests::pointer;
use super::*;
use snow_draw_engine_core::{
    CornerRadii,
    arrow::{ArrowType, StrokeStyle},
};
use snow_draw_engine_display::{OverlayDisplayItem, SceneDisplayItem, UiShapeKind};
use snow_draw_engine_document::{HighlightShape, Transaction};
use snow_draw_engine_editor::{
    MAGNIFIER_STYLE_PROPERTY_ALL, MAGNIFIER_STYLE_PROPERTY_ARROW_RATIO,
    MAGNIFIER_STYLE_PROPERTY_CONNECTION_TYPE, MAGNIFIER_STYLE_PROPERTY_CORNER_RADII,
    MAGNIFIER_STYLE_PROPERTY_FACTOR, MAGNIFIER_STYLE_PROPERTY_SOURCE_OUTLINE, MagnifierStyle,
};
use snow_draw_engine_interaction::{
    Modifiers, PointerButton, PointerButtons, PointerCaptureCommand, PointerDevice, PointerEvent,
    PointerEventType, WheelDeltaKind, WheelEvent,
};

fn fixture(shape: HighlightShape, factor: f64) -> (Engine, ViewportId, ElementId) {
    let mut config = EngineConfig::default();
    config.style_defaults.editor.magnifier.shape = shape;
    config.style_defaults.editor.magnifier.factor = factor;
    let mut engine = Engine::new(config);
    let viewport = engine.create_viewport(ViewportConfig::default()).unwrap();
    engine
        .set_viewport_surface_size(viewport, 800, 600)
        .unwrap();
    engine
        .set_viewport_active_tool(viewport, ActiveTool::Magnifier)
        .unwrap();
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Down,
        350.0,
        260.0,
        false,
    );
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Move,
        450.0,
        340.0,
        false,
    );
    let preview = engine.acquire_patch(viewport, None).unwrap();
    assert!(
        preview
            .scene
            .ops
            .iter()
            .flat_map(|op| &op.insert_items)
            .any(|item| matches!(item, SceneDisplayItem::Magnifier(_)))
    );
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Up,
        450.0,
        340.0,
        false,
    );
    let id = engine.model.paint_order()[0];
    (engine, viewport, id)
}
fn select(engine: &mut Engine, viewport: ViewportId, id: ElementId) {
    engine
        .set_viewport_active_tool(viewport, ActiveTool::Select)
        .unwrap();
    engine
        .select_element_with_viewport_changes(viewport, id)
        .unwrap();
}
fn drag(engine: &mut Engine, viewport: ViewportId, from: Point<f64>, to: Point<f64>) {
    pointer(
        engine,
        viewport,
        PointerEventType::Down,
        from.x + 400.0,
        from.y + 300.0,
        false,
    );
    pointer(
        engine,
        viewport,
        PointerEventType::Move,
        to.x + 400.0,
        to.y + 300.0,
        false,
    );
    pointer(
        engine,
        viewport,
        PointerEventType::Up,
        to.x + 400.0,
        to.y + 300.0,
        false,
    );
}
fn handle(engine: &mut Engine, viewport: ViewportId, kind: UiShapeKind) -> Point<f64> {
    let patch = engine.acquire_patch(viewport, None).unwrap();
    patch
        .overlay
        .ops
        .iter()
        .flat_map(|op| &op.insert_items)
        .find_map(|item| match item {
            OverlayDisplayItem::Rectangle(item) if item.kind == kind => {
                Some(Point::new(item.center_x, item.center_y))
            }
            _ => None,
        })
        .unwrap()
}

fn scene_magnifiers(
    engine: &Engine,
    viewport: ViewportId,
) -> Vec<snow_draw_engine_display::MagnifierDisplayItem> {
    engine
        .acquire_patch(viewport, None)
        .unwrap()
        .scene
        .ops
        .iter()
        .flat_map(|op| &op.insert_items)
        .filter_map(|item| match item {
            SceneDisplayItem::Magnifier(value) => Some(value.clone()),
            _ => None,
        })
        .collect()
}

fn wheel(engine: &mut Engine, viewport: ViewportId, delta: f64, kind: WheelDeltaKind) {
    assert!(
        engine
            .process_input(
                viewport,
                InputEvent::Wheel(WheelEvent {
                    position: Point::new(400.0, 300.0),
                    delta: snow_draw_engine_core::Vector2 { x: 0.0, y: delta },
                    delta_kind: kind,
                    modifiers: Modifiers::default(),
                }),
            )
            .unwrap()
            .consumed
    );
}

#[test]
fn magnifier_marquee_matches_either_region_without_matching_the_empty_gap() {
    for shape in [
        HighlightShape::Rectangle,
        HighlightShape::Ellipse,
        HighlightShape::Diamond,
    ] {
        for rotation in [0.0, 0.7] {
            for direction in [-1.0, 1.0] {
                let (mut engine, viewport, id) = fixture(shape, 2.0);
                let mut value = *engine.model.magnifier(id).unwrap();
                value.source_center = Point::new(-1000.0, 0.0);
                value.rotation = rotation;
                value.connection_type = snow_draw_engine_core::MagnifierConnectionType::None;
                let mut transaction = Transaction::new("separate offscreen source");
                transaction.update_magnifier(id, value);
                engine.model.apply_transaction(transaction).unwrap();
                engine
                    .reset_editing_state_with_viewport_changes(viewport)
                    .unwrap();
                drag(
                    &mut engine,
                    viewport,
                    Point::new(-170.0 * direction, -170.0 * direction),
                    Point::new(170.0 * direction, 170.0 * direction),
                );
                assert_eq!(engine.editor.selected_ids(), vec![id]);
                assert_eq!(
                    engine.model.element_rect_proxy(id).unwrap(),
                    value.source_rect()
                );

                engine
                    .reset_editing_state_with_viewport_changes(viewport)
                    .unwrap();
                drag(
                    &mut engine,
                    viewport,
                    Point::new(-530.0, -30.0),
                    Point::new(-470.0, 30.0),
                );
                assert!(engine.editor.selected_ids().is_empty());

                drag(
                    &mut engine,
                    viewport,
                    Point::new(-1100.0, -100.0),
                    Point::new(-900.0, 100.0),
                );
                assert_eq!(engine.editor.selected_ids(), vec![id]);
            }
        }
    }
}

#[test]
fn magnifier_wheel_accumulates_pixel_and_fractional_angle_deltas() {
    for selected in [false, true] {
        for (kind, delta, count) in [
            (WheelDeltaKind::Pixel, 10.0, 10),
            (WheelDeltaKind::Angle, 20.0, 6),
        ] {
            let (mut engine, viewport, id) = fixture(HighlightShape::Rectangle, 2.0);
            if selected {
                select(&mut engine, viewport, id);
            }
            let camera = engine.viewport_slot(viewport).unwrap().view;
            for _ in 0..count {
                wheel(&mut engine, viewport, delta, kind);
            }
            let style = engine.viewport_style_toolbar_state(viewport).unwrap();
            assert_eq!(style.magnifier_style.factor, 2.1);
            assert_eq!(engine.viewport_slot(viewport).unwrap().view, camera);
            if selected {
                assert_eq!(engine.model.magnifier(id).unwrap().factor, 2.1);
                assert!(engine.undo().unwrap());
                assert_eq!(engine.model.magnifier(id).unwrap().factor, 2.0);
            } else {
                assert_eq!(engine.model.magnifier(id).unwrap().factor, 2.0);
            }
        }
    }
}

#[test]
fn magnifier_wheel_remainder_resets_for_direction_kind_and_factor_changes() {
    let (mut engine, viewport, id) = fixture(HighlightShape::Rectangle, 2.0);
    select(&mut engine, viewport, id);
    wheel(&mut engine, viewport, 40.0, WheelDeltaKind::Pixel);
    wheel(&mut engine, viewport, -60.0, WheelDeltaKind::Pixel);
    assert_eq!(engine.model.magnifier(id).unwrap().factor, 2.0);
    wheel(&mut engine, viewport, -40.0, WheelDeltaKind::Pixel);
    assert_eq!(engine.model.magnifier(id).unwrap().factor, 1.9);

    wheel(&mut engine, viewport, 40.0, WheelDeltaKind::Pixel);
    wheel(&mut engine, viewport, 60.0, WheelDeltaKind::Angle);
    assert_eq!(engine.model.magnifier(id).unwrap().factor, 1.9);
    wheel(&mut engine, viewport, 60.0, WheelDeltaKind::Angle);
    assert_eq!(engine.model.magnifier(id).unwrap().factor, 2.0);

    wheel(&mut engine, viewport, 40.0, WheelDeltaKind::Pixel);
    engine
        .set_viewport_magnifier_style_patch(
            viewport,
            MagnifierStyle {
                factor: 3.0,
                ..Default::default()
            },
            MAGNIFIER_STYLE_PROPERTY_FACTOR,
            false,
        )
        .unwrap();
    wheel(&mut engine, viewport, 60.0, WheelDeltaKind::Pixel);
    assert_eq!(engine.model.magnifier(id).unwrap().factor, 3.0);
    wheel(&mut engine, viewport, 40.0, WheelDeltaKind::Pixel);
    assert_eq!(engine.model.magnifier(id).unwrap().factor, 3.1);
}

#[test]
fn magnifier_wheel_preserves_large_delta_remainders_and_discards_saturated_input() {
    let (mut engine, viewport, id) = fixture(HighlightShape::Rectangle, 2.0);
    select(&mut engine, viewport, id);
    wheel(&mut engine, viewport, 450.0, WheelDeltaKind::Pixel);
    assert_eq!(engine.model.magnifier(id).unwrap().factor, 2.4);
    wheel(&mut engine, viewport, 50.0, WheelDeltaKind::Pixel);
    assert_eq!(engine.model.magnifier(id).unwrap().factor, 2.5);
    for (start, outward, limit) in [(9.9, 250.0, 10.0), (1.1, -250.0, 1.0)] {
        engine
            .set_viewport_magnifier_style_patch(
                viewport,
                MagnifierStyle {
                    factor: start,
                    ..Default::default()
                },
                MAGNIFIER_STYLE_PROPERTY_FACTOR,
                false,
            )
            .unwrap();
        wheel(&mut engine, viewport, outward, WheelDeltaKind::Pixel);
        assert_eq!(engine.model.magnifier(id).unwrap().factor, limit);
        for _ in 0..5 {
            wheel(
                &mut engine,
                viewport,
                -outward.signum() * 20.0,
                WheelDeltaKind::Pixel,
            );
        }
        assert_eq!(engine.model.magnifier(id).unwrap().factor, start);
    }
}

#[test]
fn magnifier_wheel_remainder_resets_for_selection_tool_and_focus_changes() {
    for transition in [
        "selection",
        "defaults",
        "tool",
        "focus",
        "modified wheel",
        "history",
        "reset",
    ] {
        let (mut engine, viewport, id) = fixture(HighlightShape::Rectangle, 2.0);
        select(&mut engine, viewport, id);
        wheel(&mut engine, viewport, 40.0, WheelDeltaKind::Pixel);
        match transition {
            "selection" => {
                let other = engine.model.peek_next_element_id();
                let mut transaction = Transaction::new("another magnifier");
                transaction.insert_magnifier(
                    other,
                    Default::default(),
                    *engine.model.magnifier(id).unwrap(),
                );
                engine.model.apply_transaction(transaction).unwrap();
                select(&mut engine, viewport, other);
                select(&mut engine, viewport, id);
            }
            "defaults" => {
                engine
                    .set_viewport_active_tool(viewport, ActiveTool::Magnifier)
                    .unwrap();
            }
            "tool" => {
                engine
                    .set_viewport_active_tool(viewport, ActiveTool::Watermark)
                    .unwrap();
                engine
                    .set_viewport_active_tool(viewport, ActiveTool::Select)
                    .unwrap();
            }
            "focus" => {
                engine
                    .process_input(viewport, InputEvent::FocusLost)
                    .unwrap();
            }
            "modified wheel" => {
                engine
                    .process_input(
                        viewport,
                        InputEvent::Wheel(WheelEvent {
                            position: Point::new(400.0, 300.0),
                            delta: snow_draw_engine_core::Vector2 { x: 0.0, y: 10.0 },
                            delta_kind: WheelDeltaKind::Pixel,
                            modifiers: Modifiers {
                                ctrl: true,
                                ..Default::default()
                            },
                        }),
                    )
                    .unwrap();
            }
            "history" => {
                engine
                    .set_viewport_magnifier_style_patch(
                        viewport,
                        MagnifierStyle {
                            stroke_width: 4.0,
                            ..Default::default()
                        },
                        snow_draw_engine_editor::MAGNIFIER_STYLE_PROPERTY_STROKE_WIDTH,
                        false,
                    )
                    .unwrap();
                assert!(engine.undo().unwrap());
                assert!(engine.redo().unwrap());
            }
            "reset" => {
                engine
                    .reset_editing_state_with_viewport_changes(viewport)
                    .unwrap();
                select(&mut engine, viewport, id);
            }
            _ => unreachable!(),
        }
        wheel(&mut engine, viewport, 60.0, WheelDeltaKind::Pixel);
        assert_eq!(
            engine
                .viewport_style_toolbar_state(viewport)
                .unwrap()
                .magnifier_style
                .factor,
            2.0,
            "{transition}"
        );
        wheel(&mut engine, viewport, 40.0, WheelDeltaKind::Pixel);
        assert_eq!(
            engine
                .viewport_style_toolbar_state(viewport)
                .unwrap()
                .magnifier_style
                .factor,
            2.1,
            "{transition}"
        );
    }
}

#[test]
fn magnifier_wheel_remainder_is_not_snapshot_persisted_or_cloned_session_state() {
    let (mut engine, viewport, id) = fixture(HighlightShape::Rectangle, 2.0);
    select(&mut engine, viewport, id);
    let snapshot = engine.editor.snapshot();
    let serialized = engine.serialize_document_session().unwrap();
    wheel(&mut engine, viewport, 40.0, WheelDeltaKind::Pixel);
    assert_eq!(engine.editor.snapshot(), snapshot);
    assert_eq!(engine.serialize_document_session().unwrap(), serialized);

    let mut clone = engine.clone_document_session();
    wheel(&mut clone, viewport, 60.0, WheelDeltaKind::Pixel);
    assert_eq!(clone.model.magnifier(id).unwrap().factor, 2.0);
    wheel(&mut clone, viewport, 40.0, WheelDeltaKind::Pixel);
    assert_eq!(clone.model.magnifier(id).unwrap().factor, 2.1);
    assert_eq!(engine.model.magnifier(id).unwrap().factor, 2.0);

    let mut restored =
        Engine::from_serialized_document_session_with_config(&serialized, Default::default())
            .unwrap();
    let restored_viewport = restored.create_viewport(Default::default()).unwrap();
    restored
        .set_viewport_surface_size(restored_viewport, 800, 600)
        .unwrap();
    select(&mut restored, restored_viewport, id);
    wheel(
        &mut restored,
        restored_viewport,
        60.0,
        WheelDeltaKind::Pixel,
    );
    assert_eq!(restored.model.magnifier(id).unwrap().factor, 2.0);
    wheel(
        &mut restored,
        restored_viewport,
        40.0,
        WheelDeltaKind::Pixel,
    );
    assert_eq!(restored.model.magnifier(id).unwrap().factor, 2.1);
    wheel(&mut engine, viewport, 60.0, WheelDeltaKind::Pixel);
    assert_eq!(engine.model.magnifier(id).unwrap().factor, 2.1);
}

#[test]
fn magnifier_lens_drag_ignores_other_button_releases() {
    for (button, release) in [
        (PointerButton::Secondary, Some(PointerButton::Primary)),
        (PointerButton::Middle, Some(PointerButton::Primary)),
        (PointerButton::Secondary, None),
        (PointerButton::Middle, None),
    ] {
        let (mut engine, viewport, id) = fixture(HighlightShape::Rectangle, 2.0);
        select(&mut engine, viewport, id);
        let original = *engine.model.magnifier(id).unwrap();
        pointer(
            &mut engine,
            viewport,
            PointerEventType::Down,
            475.0,
            300.0,
            false,
        );
        pointer(
            &mut engine,
            viewport,
            PointerEventType::Move,
            495.0,
            300.0,
            false,
        );
        let output = engine
            .process_input(
                viewport,
                InputEvent::Pointer(PointerEvent {
                    pressure: None,
                    timestamp_ms: 0,
                    pointer_id: 1,
                    event_type: PointerEventType::Up,
                    device: PointerDevice::Mouse,
                    position: Point::new(495.0, 300.0),
                    button: Some(button),
                    buttons: PointerButtons(PointerButtons::PRIMARY),
                    modifiers: Modifiers::default(),
                }),
            )
            .unwrap();
        assert!(!output.consumed);
        assert_eq!(output.capture, PointerCaptureCommand::NoChange);
        assert_eq!(*engine.model.magnifier(id).unwrap(), original);
        pointer(
            &mut engine,
            viewport,
            PointerEventType::Move,
            515.0,
            300.0,
            false,
        );
        let output = engine
            .process_input(
                viewport,
                InputEvent::Pointer(PointerEvent {
                    pressure: None,
                    timestamp_ms: 0,
                    pointer_id: 1,
                    event_type: PointerEventType::Up,
                    device: PointerDevice::Mouse,
                    position: Point::new(515.0, 300.0),
                    button: release,
                    buttons: PointerButtons::default(),
                    modifiers: Modifiers::default(),
                }),
            )
            .unwrap();
        assert_eq!(output.capture, PointerCaptureCommand::Release);
        assert_eq!(
            engine.model.magnifier(id).unwrap().magnified_center,
            Point::new(40.0, 0.0)
        );
        assert!(engine.undo().unwrap());
        assert_eq!(*engine.model.magnifier(id).unwrap(), original);
    }
}

#[test]
fn magnifier_creates_all_shapes_as_one_pristine_image_item() {
    for shape in [
        HighlightShape::Rectangle,
        HighlightShape::Ellipse,
        HighlightShape::Diamond,
    ] {
        let (mut engine, viewport, id) = fixture(shape, 2.0);
        let value = *engine.model.magnifier(id).unwrap();
        assert_eq!(value.source_center, value.magnified_center);
        assert_eq!((value.width, value.height), (100.0, 80.0));
        assert_eq!(value.shape, shape);
        assert_eq!(value.corner_radii, CornerRadii::splat(6.0));
        assert!(
            value.connection_type == snow_draw_engine_core::MagnifierConnectionType::Gradient
                && value.leader().is_none()
        );
        let patch = engine.acquire_patch(viewport, None).unwrap();
        let items = patch
            .scene
            .ops
            .iter()
            .flat_map(|op| &op.insert_items)
            .collect::<Vec<_>>();
        assert_eq!(items.len(), 1);
        let SceneDisplayItem::Magnifier(item) = items[0] else {
            panic!("one logical lens")
        };
        assert_eq!((item.lens.width, item.lens.height), (200.0, 160.0));
        assert_eq!(item.lens.corner_radii, CornerRadii::splat(6.0));
        assert_eq!((item.source_width, item.source_height), (100.0, 80.0));
        select(&mut engine, viewport, id);
        let patch = engine.acquire_patch(viewport, None).unwrap();
        assert!(patch.overlay.ops.iter().flat_map(|op|&op.insert_items).any(|item|matches!(item,OverlayDisplayItem::FocusConnection(item) if item.stroke_style==snow_draw_engine_core::arrow::StrokeStyle::Dashed)));
    }
}

#[test]
fn magnifier_source_wins_overlap_and_lens_moves_independently_with_history() {
    for shape in [
        HighlightShape::Rectangle,
        HighlightShape::Ellipse,
        HighlightShape::Diamond,
    ] {
        let (mut engine, viewport, id) = fixture(shape, 2.0);
        select(&mut engine, viewport, id);
        let before = *engine.model.magnifier(id).unwrap();
        drag(
            &mut engine,
            viewport,
            Point::new(0.0, 0.0),
            Point::new(20.0, 15.0),
        );
        let moved = *engine.model.magnifier(id).unwrap();
        assert_eq!(moved.source_center, Point::new(20.0, 15.0));
        assert_eq!(moved.magnified_center, moved.source_center);
        assert!(engine.undo().unwrap());
        assert_eq!(*engine.model.magnifier(id).unwrap(), before);
        assert!(engine.redo().unwrap());
        // The lens's exposed horizontal interior remains outside the source.
        let lens_from = Point::new(95.0, 15.0);
        drag(&mut engine, viewport, lens_from, Point::new(185.0, 55.0));
        let lens = *engine.model.magnifier(id).unwrap();
        assert_eq!(lens.source_center, moved.source_center);
        assert_eq!(lens.magnified_center, Point::new(110.0, 55.0));
        assert!(engine.undo().unwrap());
        assert_eq!(*engine.model.magnifier(id).unwrap(), moved);
    }
}

#[test]
fn magnifier_corner_handle_edits_geometry_with_undo_and_redo() {
    let (mut engine, viewport, id) = fixture(HighlightShape::Rectangle, 2.0);
    select(&mut engine, viewport, id);
    let original = *engine.model.magnifier(id).unwrap();
    let from = handle(
        &mut engine,
        viewport,
        UiShapeKind::SelectionCornerRadiusHandle,
    );
    drag(
        &mut engine,
        viewport,
        from,
        Point::new(from.x + 15.0, from.y + 15.0),
    );
    let changed = *engine.model.magnifier(id).unwrap();
    assert_ne!(changed.corner_radii, original.corner_radii);
    assert_eq!(
        (changed.width, changed.height),
        (original.width, original.height)
    );
    assert_eq!(changed.source_center, original.source_center);
    assert_eq!(changed.magnified_center, original.magnified_center);
    assert!(engine.undo().unwrap());
    assert_eq!(*engine.model.magnifier(id).unwrap(), original);
    assert!(engine.redo().unwrap());
    assert_eq!(*engine.model.magnifier(id).unwrap(), changed);
}

#[test]
fn magnifier_mixed_corner_patch_remembers_corners_and_preserves_unedited_factors() {
    let (mut engine, viewport, first_id) = fixture(HighlightShape::Rectangle, 2.0);
    let first = *engine.model.magnifier(first_id).unwrap();
    let second_id = engine.model.peek_next_element_id();
    let second = snow_draw_engine_document::MagnifierData {
        source_center: Point::new(400.0, 0.0),
        magnified_center: Point::new(400.0, 0.0),
        factor: 3.0,
        corner_radii: CornerRadii::splat(12.0),
        ..first
    };
    let mut transaction = Transaction::new("second magnifier");
    transaction.insert_magnifier(second_id, Default::default(), second);
    engine
        .apply_editor_command(
            viewport,
            EditorCommand::ApplyTransaction(ApplyTransactionCommand {
                transaction,
                history_undo_snapshot: None,
            }),
        )
        .unwrap();
    let snapshot = serde_json::from_value(serde_json::json!({
        "selectedIds": [first_id, second_id], "primaryId": second_id
    }))
    .unwrap();
    engine
        .editor
        .restore_history_selection(&engine.model, &snapshot);
    engine.refresh_all_viewports().unwrap();
    let state = engine.viewport_style_toolbar_state(viewport).unwrap();
    assert_eq!(state.magnifier_style.factor, 3.0);
    assert_eq!(
        state.magnifier_style_mixed,
        MAGNIFIER_STYLE_PROPERTY_CORNER_RADII | MAGNIFIER_STYLE_PROPERTY_FACTOR
    );
    let style = MagnifierStyle {
        corner_radii: CornerRadii::splat(8.0),
        factor: 9.0,
        ..Default::default()
    };
    engine
        .set_viewport_magnifier_style_patch(
            viewport,
            style,
            MAGNIFIER_STYLE_PROPERTY_CORNER_RADII,
            false,
        )
        .unwrap();
    assert_eq!(
        engine.model.magnifier(first_id).unwrap().corner_radii,
        CornerRadii::splat(8.0)
    );
    assert_eq!(
        engine.model.magnifier(second_id).unwrap().corner_radii,
        CornerRadii::splat(8.0)
    );
    assert_eq!(engine.model.magnifier(first_id).unwrap().factor, 2.0);
    assert_eq!(engine.model.magnifier(second_id).unwrap().factor, 3.0);
    assert_eq!(
        engine
            .viewport_style_toolbar_state(viewport)
            .unwrap()
            .magnifier_style_mixed,
        MAGNIFIER_STYLE_PROPERTY_FACTOR
    );
    assert!(engine.undo().unwrap());
    assert_eq!(*engine.model.magnifier(first_id).unwrap(), first);
    assert_eq!(*engine.model.magnifier(second_id).unwrap(), second);
    engine
        .reset_editing_state_with_viewport_changes(viewport)
        .unwrap();
    assert_eq!(
        engine
            .viewport_style_toolbar_state(viewport)
            .unwrap()
            .magnifier_style
            .corner_radii,
        CornerRadii::splat(8.0)
    );
    engine
        .set_viewport_active_tool(viewport, ActiveTool::Magnifier)
        .unwrap();
    drag(
        &mut engine,
        viewport,
        Point::new(-250.0, 100.0),
        Point::new(-150.0, 180.0),
    );
    let next = engine
        .model
        .paint_order()
        .iter()
        .copied()
        .rev()
        .find(|id| engine.model.magnifier(*id).is_ok())
        .unwrap();
    let next = engine.model.magnifier(next).unwrap();
    assert_eq!(next.corner_radii, CornerRadii::splat(8.0));
    assert_eq!(next.factor, 2.0);
}

#[test]
fn magnifier_mixed_arrow_ratio_patch_preserves_other_properties_and_history() {
    let (mut engine, viewport, first_id) = fixture(HighlightShape::Rectangle, 2.0);
    let first = *engine.model.magnifier(first_id).unwrap();
    let second_id = engine.model.peek_next_element_id();
    let second = snow_draw_engine_document::MagnifierData {
        source_center: Point::new(400.0, 0.0),
        magnified_center: Point::new(400.0, 0.0),
        factor: 3.0,
        arrow_ratio: 2.3,
        ..first
    };
    let mut transaction = Transaction::new("second magnifier");
    transaction.insert_magnifier(second_id, Default::default(), second);
    engine
        .apply_editor_command(
            viewport,
            EditorCommand::ApplyTransaction(ApplyTransactionCommand {
                transaction,
                history_undo_snapshot: None,
            }),
        )
        .unwrap();
    let snapshot = serde_json::from_value(serde_json::json!({
        "selectedIds": [first_id, second_id], "primaryId": first_id
    }))
    .unwrap();
    engine
        .editor
        .restore_history_selection(&engine.model, &snapshot);
    engine.refresh_all_viewports().unwrap();
    assert_eq!(
        engine
            .viewport_style_toolbar_state(viewport)
            .unwrap()
            .magnifier_style_mixed,
        MAGNIFIER_STYLE_PROPERTY_ARROW_RATIO | MAGNIFIER_STYLE_PROPERTY_FACTOR
    );
    engine
        .set_viewport_magnifier_style_patch(
            viewport,
            MagnifierStyle {
                arrow_ratio: 0.5,
                factor: 9.0,
                ..Default::default()
            },
            MAGNIFIER_STYLE_PROPERTY_ARROW_RATIO,
            false,
        )
        .unwrap();
    assert_eq!(
        *engine.model.magnifier(first_id).unwrap(),
        snow_draw_engine_document::MagnifierData {
            arrow_ratio: 0.5,
            ..first
        }
    );
    assert_eq!(
        *engine.model.magnifier(second_id).unwrap(),
        snow_draw_engine_document::MagnifierData {
            arrow_ratio: 0.5,
            ..second
        }
    );
    assert_eq!(
        engine
            .viewport_style_toolbar_state(viewport)
            .unwrap()
            .magnifier_style_mixed,
        MAGNIFIER_STYLE_PROPERTY_FACTOR
    );
    assert!(engine.history_state().can_undo);
    engine.undo().unwrap();
    assert_eq!(*engine.model.magnifier(first_id).unwrap(), first);
    assert_eq!(*engine.model.magnifier(second_id).unwrap(), second);
    assert!(engine.history_state().can_redo);
    engine.redo().unwrap();
    assert_eq!(engine.model.magnifier(first_id).unwrap().arrow_ratio, 0.5);
    assert_eq!(engine.model.magnifier(second_id).unwrap().arrow_ratio, 0.5);
    engine
        .reset_editing_state_with_viewport_changes(viewport)
        .unwrap();
    assert_eq!(
        engine
            .viewport_style_toolbar_state(viewport)
            .unwrap()
            .magnifier_style
            .arrow_ratio,
        0.5
    );
}

#[test]
fn magnifier_factor_one_external_grip_can_separate_coincident_regions() {
    let (mut engine, viewport, id) = fixture(HighlightShape::Ellipse, 1.0);
    select(&mut engine, viewport, id);
    let from = handle(&mut engine, viewport, UiShapeKind::MagnifierMoveHandle);
    drag(
        &mut engine,
        viewport,
        from,
        Point::new(from.x + 140.0, from.y + 20.0),
    );
    let value = engine.model.magnifier(id).unwrap();
    assert_eq!(value.source_center, Point::new(0.0, 0.0));
    assert_eq!(value.magnified_center, Point::new(140.0, 20.0));
    assert!(value.leader().is_none());
    assert_eq!(
        value.connection_type,
        snow_draw_engine_core::MagnifierConnectionType::Gradient
    );
}

#[test]
fn magnifier_single_resize_and_rotation_edit_source_with_fixed_lens_center() {
    let (mut engine, viewport, id) = fixture(HighlightShape::Rectangle, 2.0);
    select(&mut engine, viewport, id);
    let grip = handle(&mut engine, viewport, UiShapeKind::MagnifierMoveHandle);
    drag(
        &mut engine,
        viewport,
        grip,
        Point::new(grip.x + 180.0, grip.y),
    );
    let before = *engine.model.magnifier(id).unwrap();
    let from = handle(&mut engine, viewport, UiShapeKind::SelectionResizeHandle);
    drag(
        &mut engine,
        viewport,
        from,
        Point::new(from.x - 30.0, from.y - 20.0),
    );
    let resized = *engine.model.magnifier(id).unwrap();
    assert_eq!(resized.magnified_center, before.magnified_center);
    assert_eq!(resized.factor, before.factor);
    assert!(resized.width > before.width && resized.height > before.height);
    let rotation = handle(&mut engine, viewport, UiShapeKind::SelectionRotationHandle);
    let relative = Point::new(
        rotation.x - resized.source_center.x,
        rotation.y - resized.source_center.y,
    );
    let to = Point::new(
        resized.source_center.x - relative.y,
        resized.source_center.y + relative.x,
    );
    drag(&mut engine, viewport, rotation, to);
    let rotated = *engine.model.magnifier(id).unwrap();
    assert_eq!(rotated.source_center, resized.source_center);
    assert_eq!(rotated.magnified_center, resized.magnified_center);
    assert!((rotated.rotation - std::f64::consts::FRAC_PI_2).abs() < 1e-9);
    assert!(engine.undo().unwrap());
    assert_eq!(*engine.model.magnifier(id).unwrap(), resized);
}

#[test]
fn magnifier_lens_cancel_does_not_commit_and_z_order_remains_intact() {
    let (mut engine, viewport, id) = fixture(HighlightShape::Rectangle, 2.0);
    select(&mut engine, viewport, id);
    let before = *engine.model.magnifier(id).unwrap();
    let from = handle(&mut engine, viewport, UiShapeKind::MagnifierMoveHandle);
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Down,
        from.x + 400.0,
        from.y + 300.0,
        false,
    );
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Move,
        from.x + 500.0,
        from.y + 330.0,
        false,
    );
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Cancel,
        from.x + 500.0,
        from.y + 330.0,
        false,
    );
    assert_eq!(*engine.model.magnifier(id).unwrap(), before);
    let top = engine.model.peek_next_element_id();
    let mut rect = before.source_rect();
    rect.width = 20.0;
    rect.height = 20.0;
    let mut transaction = Transaction::new("top annotation");
    transaction.insert_rectangle(top, Default::default(), rect);
    engine
        .apply_editor_command(
            viewport,
            EditorCommand::ApplyTransaction(ApplyTransactionCommand {
                transaction,
                history_undo_snapshot: None,
            }),
        )
        .unwrap();
    engine
        .reset_editing_state_with_viewport_changes(viewport)
        .unwrap();
    assert_eq!(
        engine
            .hit_quick_selection_at(
                viewport,
                Point::new(0.0, 0.0),
                snow_draw_engine_interaction::PointerButton::Primary
            )
            .unwrap(),
        Some(top)
    );
}

#[test]
fn magnifier_style_payload_requires_current_fields_and_rejects_obsolete_toggle() {
    let style = MagnifierStyle::default();
    assert_eq!(
        style.connection_type,
        snow_draw_engine_core::MagnifierConnectionType::Gradient
    );
    assert!(style.source_outline);
    let json = serde_json::to_value(style).unwrap();
    assert_eq!(
        serde_json::from_value::<MagnifierStyle>(json.clone()).unwrap(),
        style
    );
    for key in ["connection_type", "source_outline"] {
        let mut missing = json.clone();
        missing.as_object_mut().unwrap().remove(key);
        assert!(serde_json::from_value::<MagnifierStyle>(missing).is_err());
    }
    let mut obsolete = json;
    obsolete["show_leader"] = false.into();
    assert!(serde_json::from_value::<MagnifierStyle>(obsolete).is_err());
}

#[test]
fn magnifier_connections_and_source_outlines_patch_independently_with_history() {
    use snow_draw_engine_core::MagnifierConnectionType;
    for connection_type in [
        MagnifierConnectionType::None,
        MagnifierConnectionType::LeaderLine,
        MagnifierConnectionType::Gradient,
    ] {
        for source_outline in [false, true] {
            let (mut engine, viewport, id) = fixture(HighlightShape::Rectangle, 2.0);
            select(&mut engine, viewport, id);
            let original = *engine.model.magnifier(id).unwrap();
            let style = MagnifierStyle {
                connection_type,
                source_outline,
                factor: 9.0,
                ..Default::default()
            };
            engine
                .set_viewport_magnifier_style_patch(
                    viewport,
                    style,
                    MAGNIFIER_STYLE_PROPERTY_CONNECTION_TYPE
                        | MAGNIFIER_STYLE_PROPERTY_SOURCE_OUTLINE,
                    false,
                )
                .unwrap();
            let changed = *engine.model.magnifier(id).unwrap();
            assert_eq!(changed.connection_type, connection_type);
            assert_eq!(changed.source_outline, source_outline);
            assert_eq!(changed.factor, original.factor);
            let display = scene_magnifiers(&engine, viewport).remove(0);
            assert_eq!(display.connection_type, connection_type);
            assert_eq!(display.source_outline, source_outline);
            if changed != original {
                assert!(engine.undo().unwrap());
                assert_eq!(*engine.model.magnifier(id).unwrap(), original);
                assert!(engine.redo().unwrap());
                assert_eq!(*engine.model.magnifier(id).unwrap(), changed);
            }
            engine
                .reset_editing_state_with_viewport_changes(viewport)
                .unwrap();
            let defaults = engine
                .viewport_style_toolbar_state(viewport)
                .unwrap()
                .magnifier_style;
            assert_eq!(defaults.connection_type, connection_type);
            assert_eq!(defaults.source_outline, source_outline);
            assert_eq!(defaults.factor, original.factor);
        }
    }
}

#[test]
fn magnifier_mixed_connections_and_outlines_resolve_one_property_at_a_time() {
    use snow_draw_engine_core::MagnifierConnectionType;
    let (mut engine, viewport, first_id) = fixture(HighlightShape::Rectangle, 2.0);
    let original = *engine.model.magnifier(first_id).unwrap();
    let second_id = engine.model.peek_next_element_id();
    let second = snow_draw_engine_document::MagnifierData {
        source_center: Point::new(400.0, 0.0),
        magnified_center: Point::new(400.0, 0.0),
        connection_type: MagnifierConnectionType::None,
        source_outline: false,
        ..original
    };
    let mut transaction = Transaction::new("second magnifier");
    transaction.insert_magnifier(second_id, Default::default(), second);
    engine
        .apply_editor_command(
            viewport,
            EditorCommand::ApplyTransaction(ApplyTransactionCommand {
                transaction,
                history_undo_snapshot: None,
            }),
        )
        .unwrap();
    let selection = serde_json::from_value(
        serde_json::json!({"selectedIds":[first_id,second_id], "primaryId":first_id}),
    )
    .unwrap();
    engine
        .editor
        .restore_history_selection(&engine.model, &selection);
    engine.refresh_all_viewports().unwrap();
    assert_eq!(
        engine
            .viewport_style_toolbar_state(viewport)
            .unwrap()
            .magnifier_style_mixed,
        MAGNIFIER_STYLE_PROPERTY_CONNECTION_TYPE | MAGNIFIER_STYLE_PROPERTY_SOURCE_OUTLINE
    );
    engine
        .set_viewport_magnifier_style_patch(
            viewport,
            MagnifierStyle {
                connection_type: MagnifierConnectionType::LeaderLine,
                ..Default::default()
            },
            MAGNIFIER_STYLE_PROPERTY_CONNECTION_TYPE,
            false,
        )
        .unwrap();
    assert_eq!(
        engine
            .viewport_style_toolbar_state(viewport)
            .unwrap()
            .magnifier_style_mixed,
        MAGNIFIER_STYLE_PROPERTY_SOURCE_OUTLINE
    );
    assert!(engine.model.magnifier(first_id).unwrap().source_outline);
    assert!(!engine.model.magnifier(second_id).unwrap().source_outline);
    engine
        .set_viewport_magnifier_style_patch(
            viewport,
            MagnifierStyle::default(),
            MAGNIFIER_STYLE_PROPERTY_SOURCE_OUTLINE,
            false,
        )
        .unwrap();
    assert_eq!(
        engine
            .viewport_style_toolbar_state(viewport)
            .unwrap()
            .magnifier_style_mixed,
        0
    );
    assert!(engine.model.magnifier(second_id).unwrap().source_outline);
    assert_eq!(
        engine.model.magnifier(second_id).unwrap().connection_type,
        MagnifierConnectionType::LeaderLine
    );
    assert!(engine.undo().unwrap());
    assert!(!engine.model.magnifier(second_id).unwrap().source_outline);
    assert!(engine.undo().unwrap());
    assert_eq!(*engine.model.magnifier(first_id).unwrap(), original);
    assert_eq!(*engine.model.magnifier(second_id).unwrap(), second);
}

#[test]
fn magnifier_creation_only_patches_preserve_selection_and_sessions_remember_styles() {
    let (mut engine, viewport, id) = fixture(HighlightShape::Diamond, 2.0);
    select(&mut engine, viewport, id);
    let style = MagnifierStyle {
        factor: 3.5,
        connection_type: snow_draw_engine_core::MagnifierConnectionType::None,
        source_outline: false,
        stroke_width: 0.0,
        corner_radii: CornerRadii::splat(12.0),
        arrow_ratio: 2.3,
        ..Default::default()
    };
    engine
        .set_viewport_magnifier_style_patch(viewport, style, MAGNIFIER_STYLE_PROPERTY_ALL, false)
        .unwrap();
    assert_eq!(engine.model.magnifier(id).unwrap().factor, 3.5);
    let defaults = MagnifierStyle {
        factor: 8.0,
        corner_radii: CornerRadii::splat(3.0),
        arrow_ratio: 0.5,
        ..Default::default()
    };
    engine
        .set_viewport_magnifier_style_patch(
            viewport,
            defaults,
            MAGNIFIER_STYLE_PROPERTY_FACTOR
                | MAGNIFIER_STYLE_PROPERTY_CORNER_RADII
                | MAGNIFIER_STYLE_PROPERTY_ARROW_RATIO,
            true,
        )
        .unwrap();
    assert_eq!(engine.model.magnifier(id).unwrap().factor, 3.5);
    assert_eq!(
        engine.model.magnifier(id).unwrap().corner_radii,
        CornerRadii::splat(12.0)
    );
    let bytes = engine.serialize_document_session().unwrap();
    assert_eq!(engine.model.magnifier(id).unwrap().arrow_ratio, 2.3);
    assert_eq!(
        serde_json::from_slice::<serde_json::Value>(&bytes).unwrap()["schemaVersion"],
        12
    );
    let mut restored =
        Engine::from_serialized_document_session_with_config(&bytes, EngineConfig::default())
            .unwrap();
    assert_eq!(
        *restored.model.magnifier(id).unwrap(),
        *engine.model.magnifier(id).unwrap()
    );
    let rv = restored.create_viewport(ViewportConfig::default()).unwrap();
    restored
        .set_viewport_active_tool(rv, ActiveTool::Magnifier)
        .unwrap();
    assert_eq!(
        restored
            .viewport_style_toolbar_state(rv)
            .unwrap()
            .magnifier_style,
        MagnifierStyle {
            factor: 8.0,
            corner_radii: CornerRadii::splat(3.0),
            arrow_ratio: 0.5,
            ..style
        }
    );
    assert_eq!(
        restored
            .viewport_style_toolbar_state(rv)
            .unwrap()
            .magnifier_style
            .arrow_ratio,
        0.5
    );
    assert_eq!(
        restored
            .viewport_style_toolbar_state(rv)
            .unwrap()
            .magnifier_style
            .corner_radii,
        CornerRadii::splat(3.0)
    );
    assert_eq!(
        restored
            .viewport_style_toolbar_state(rv)
            .unwrap()
            .magnifier_style
            .factor,
        8.0
    );
    let history = engine.serialize_document_history().unwrap();
    assert_eq!(
        serde_json::from_slice::<serde_json::Value>(&history).unwrap()["schemaVersion"],
        8
    );
    let restored =
        Engine::from_serialized_document_history_with_config(&history, EngineConfig::default())
            .unwrap();
    assert_eq!(
        *restored.model.magnifier(id).unwrap(),
        *engine.model.magnifier(id).unwrap()
    );
    let template = engine.serialize_selected_draw_template().unwrap();
    assert_eq!(
        serde_json::from_slice::<serde_json::Value>(&template).unwrap()["schemaVersion"],
        3
    );
    engine
        .insert_draw_template_with_viewport_changes(viewport, &template, Point::new(150.0, 120.0))
        .unwrap();
    assert_eq!(engine.model.paint_order().len(), 2);
    let copy = engine
        .model
        .magnifier(engine.model.paint_order()[1])
        .unwrap();
    assert_eq!((copy.width, copy.height, copy.factor), (100.0, 80.0, 3.5));
    assert_eq!(copy.arrow_ratio, 2.3);
    assert_eq!(copy.connection_type, style.connection_type);
    assert_eq!(copy.source_outline, style.source_outline);
}

#[test]
fn magnifier_legacy_session_without_arrow_ratios_uses_identity_scale() {
    fn remove_ratios(value: &mut serde_json::Value) {
        match value {
            serde_json::Value::Object(object) => {
                object.remove("arrow_ratio");
                for value in object.values_mut() {
                    remove_ratios(value);
                }
            }
            serde_json::Value::Array(values) => {
                for value in values {
                    remove_ratios(value);
                }
            }
            _ => {}
        }
    }
    let (engine, _, id) = fixture(HighlightShape::Rectangle, 2.0);
    let mut json: serde_json::Value =
        serde_json::from_slice(&engine.serialize_document_session().unwrap()).unwrap();
    remove_ratios(&mut json);
    let mut restored = Engine::from_serialized_document_session_with_config(
        &serde_json::to_vec(&json).unwrap(),
        Default::default(),
    )
    .unwrap();
    assert_eq!(restored.model.magnifier(id).unwrap().arrow_ratio, 1.0);
    let viewport = restored.create_viewport(ViewportConfig::default()).unwrap();
    restored
        .set_viewport_active_tool(viewport, ActiveTool::Magnifier)
        .unwrap();
    assert_eq!(
        restored
            .viewport_style_toolbar_state(viewport)
            .unwrap()
            .magnifier_style
            .arrow_ratio,
        1.0
    );
}

#[test]
fn magnifier_legacy_session_without_defaults_loads_and_invalid_defaults_fail() {
    let engine = Engine::default();
    let mut json: serde_json::Value =
        serde_json::from_slice(&engine.serialize_document_session().unwrap()).unwrap();
    json["schemaVersion"] = 10.into();
    json["editor"].as_object_mut().unwrap().remove("magnifier");
    let mut legacy = Engine::from_serialized_document_session_with_config(
        &serde_json::to_vec(&json).unwrap(),
        EngineConfig::default(),
    )
    .unwrap();
    let v = legacy.create_viewport(ViewportConfig::default()).unwrap();
    assert_eq!(
        legacy
            .viewport_style_toolbar_state(v)
            .unwrap()
            .magnifier_style
            .factor,
        2.0
    );
    json["editor"]["magnifier"] = serde_json::to_value(MagnifierStyle {
        factor: 11.0,
        ..Default::default()
    })
    .unwrap();
    assert!(
        Engine::from_serialized_document_session_with_config(
            &serde_json::to_vec(&json).unwrap(),
            EngineConfig::default()
        )
        .is_err()
    );
}

#[test]
fn magnifier_mixed_selection_resizes_and_rotates_both_region_centers_coherently() {
    let (mut engine, viewport, id) = fixture(HighlightShape::Rectangle, 2.0);
    select(&mut engine, viewport, id);
    let grip = handle(&mut engine, viewport, UiShapeKind::MagnifierMoveHandle);
    drag(
        &mut engine,
        viewport,
        grip,
        Point::new(grip.x + 180.0, grip.y),
    );
    let rectangle_id = engine.model.peek_next_element_id();
    let mut rectangle = engine.model.magnifier(id).unwrap().source_rect();
    rectangle.center = Point::new(400.0, 0.0);
    rectangle.width = 20.0;
    rectangle.height = 20.0;
    let mut transaction = Transaction::new("mixed selection member");
    transaction.insert_rectangle(rectangle_id, Default::default(), rectangle);
    engine
        .apply_editor_command(
            viewport,
            EditorCommand::ApplyTransaction(ApplyTransactionCommand {
                transaction,
                history_undo_snapshot: None,
            }),
        )
        .unwrap();
    let snapshot = serde_json::from_value(serde_json::json!({
        "selectedIds": [id, rectangle_id], "primaryId": id
    }))
    .unwrap();
    engine
        .editor
        .restore_history_selection(&engine.model, &snapshot);
    engine.refresh_all_viewports().unwrap();
    let from = handle(&mut engine, viewport, UiShapeKind::SelectionResizeHandle);
    drag(
        &mut engine,
        viewport,
        from,
        Point::new(from.x - 100.0, from.y - 60.0),
    );
    let resized = *engine.model.magnifier(id).unwrap();
    let sx = resized.width / 100.0;
    assert!(sx > 1.0 && resized.height > 80.0);
    assert!((resized.magnified_center.x - resized.source_center.x - 180.0 * sx).abs() < 1e-8);
    assert!(
        (engine.model.rectangle(rectangle_id).unwrap().center.x
            - resized.source_center.x
            - 400.0 * sx)
            .abs()
            < 1e-8
    );
    assert_eq!(resized.factor, 2.0);
    let bounds = engine
        .editor
        .presentation_state(&engine.model, &engine.viewport_slot(viewport).unwrap().view)
        .selection_bounds
        .unwrap();
    let from = handle(&mut engine, viewport, UiShapeKind::SelectionRotationHandle);
    let dx = from.x - bounds.center.x;
    let dy = from.y - bounds.center.y;
    drag(
        &mut engine,
        viewport,
        from,
        Point::new(bounds.center.x - dy, bounds.center.y + dx),
    );
    let rotated = *engine.model.magnifier(id).unwrap();
    assert!((rotated.rotation - std::f64::consts::FRAC_PI_2).abs() < 1e-8);
    assert!((rotated.magnified_center.x - rotated.source_center.x).abs() < 1e-8);
    assert!((rotated.magnified_center.y - rotated.source_center.y - 180.0 * sx).abs() < 1e-8);
    assert_eq!(
        (rotated.width, rotated.height, rotated.factor),
        (resized.width, resized.height, 2.0)
    );
}

#[derive(Clone, Copy, Debug)]
enum ResizeCompanion {
    Rectangle,
    Arrow,
    Magnifier,
}

fn magnifier_resize_fixture(
    shape: HighlightShape,
    companion: Option<ResizeCompanion>,
) -> (Engine, ViewportId, Vec<ElementId>) {
    let mut engine = Engine::default();
    let viewport = engine.create_viewport(ViewportConfig::default()).unwrap();
    engine
        .set_viewport_surface_size(viewport, 800, 600)
        .unwrap();
    engine
        .set_viewport_active_tool(viewport, ActiveTool::Select)
        .unwrap();
    let id = engine.model.peek_next_element_id();
    let value = snow_draw_engine_document::MagnifierData {
        source_center: Point::new(-20.0, -10.0),
        magnified_center: Point::new(150.0, 90.0),
        width: 80.0,
        height: 60.0,
        rotation: 0.4,
        shape,
        corner_radii: CornerRadii {
            top_left: 4.0,
            top_right: 8.0,
            bottom_right: 12.0,
            bottom_left: 16.0,
        },
        ..Default::default()
    };
    let mut transaction = Transaction::new("resize regression fixture");
    transaction.insert_magnifier(id, Default::default(), value);
    let mut ids = vec![id];
    if let Some(companion) = companion {
        let other_id = ElementId {
            index: id.index + 1,
            generation: id.generation,
        };
        match companion {
            ResizeCompanion::Rectangle => {
                let mut rectangle = value.source_rect();
                rectangle.center = Point::new(300.0, 200.0);
                rectangle.width = 40.0;
                rectangle.height = 30.0;
                rectangle.rotation = 0.0;
                transaction.insert_rectangle(other_id, Default::default(), rectangle);
            }
            ResizeCompanion::Arrow => {
                let arrow = snow_draw_engine_document::ArrowData::from_global_points(
                    &[Point::new(260.0, 180.0), Point::new(310.0, 220.0)],
                    value.stroke,
                    2.0,
                    StrokeStyle::Solid,
                    ArrowType::Straight,
                    None,
                    None,
                )
                .unwrap();
                transaction.insert_arrow(other_id, Default::default(), arrow);
            }
            ResizeCompanion::Magnifier => {
                transaction.insert_magnifier(
                    other_id,
                    Default::default(),
                    snow_draw_engine_document::MagnifierData {
                        source_center: Point::new(250.0, 170.0),
                        magnified_center: Point::new(300.0, 210.0),
                        width: 30.0,
                        height: 20.0,
                        factor: 1.5,
                        rotation: -0.2,
                        ..value
                    },
                );
            }
        }
        ids.push(other_id);
    }
    engine
        .apply_editor_command(
            viewport,
            EditorCommand::ApplyTransaction(ApplyTransactionCommand {
                transaction,
                history_undo_snapshot: None,
            }),
        )
        .unwrap();
    let snapshot = serde_json::from_value(serde_json::json!({
        "selectedIds": ids, "primaryId": id
    }))
    .unwrap();
    engine
        .editor
        .restore_history_selection(&engine.model, &snapshot);
    engine.refresh_all_viewports().unwrap();
    (engine, viewport, ids)
}

fn resize_selection_by_scales(
    engine: &mut Engine,
    viewport: ViewportId,
    sx: f64,
    sy: f64,
    from_center: bool,
) -> (snow_draw_engine_editor::SelectionBounds, Point<f64>) {
    let bounds = engine
        .editor
        .presentation_state(&engine.model, &engine.viewport_slot(viewport).unwrap().view)
        .selection_bounds
        .unwrap();
    let local_anchor = if from_center {
        Point::default()
    } else {
        Point::new(-bounds.width / 2.0, -bounds.height / 2.0)
    };
    let (sin, cos) = bounds.rotation.sin_cos();
    let map = |point: Point<f64>| {
        Point::new(
            bounds.center.x + point.x * cos - point.y * sin,
            bounds.center.y + point.x * sin + point.y * cos,
        )
    };
    let from = map(Point::new(
        bounds.width / 2.0 + 4.0,
        bounds.height / 2.0 + 4.0,
    ));
    let reference = if from_center { 0.5 } else { 1.0 };
    let to = map(Point::new(
        local_anchor.x + bounds.width * reference * sx + 4.0,
        local_anchor.y + bounds.height * reference * sy + 4.0,
    ));
    pointer(
        engine,
        viewport,
        PointerEventType::Down,
        from.x + 400.0,
        from.y + 300.0,
        from_center,
    );
    pointer(
        engine,
        viewport,
        PointerEventType::Move,
        to.x + 400.0,
        to.y + 300.0,
        from_center,
    );
    (bounds, to)
}

fn resized_point(
    point: Point<f64>,
    bounds: snow_draw_engine_editor::SelectionBounds,
    sx: f64,
    sy: f64,
    from_center: bool,
) -> Point<f64> {
    let (sin, cos) = bounds.rotation.sin_cos();
    let dx = point.x - bounds.center.x;
    let dy = point.y - bounds.center.y;
    let local = Point::new(dx * cos + dy * sin, -dx * sin + dy * cos);
    let anchor = if from_center {
        Point::default()
    } else {
        Point::new(-bounds.width / 2.0, -bounds.height / 2.0)
    };
    let local = Point::new(
        anchor.x + (local.x - anchor.x) * sx,
        anchor.y + (local.y - anchor.y) * sy,
    );
    Point::new(
        bounds.center.x + local.x * cos - local.y * sin,
        bounds.center.y + local.x * sin + local.y * cos,
    )
}

fn reflected_radii(shape: HighlightShape, radii: CornerRadii, sx: f64, sy: f64) -> CornerRadii {
    let mut values = [
        radii.top_left,
        radii.top_right,
        radii.bottom_right,
        radii.bottom_left,
    ];
    if sx < 0.0 {
        if shape == HighlightShape::Diamond {
            values.swap(1, 3);
        } else {
            values.swap(0, 1);
            values.swap(2, 3);
        }
    }
    if sy < 0.0 {
        if shape == HighlightShape::Diamond {
            values.swap(0, 2);
        } else {
            values.swap(0, 3);
            values.swap(1, 2);
        }
    }
    CornerRadii {
        top_left: values[0],
        top_right: values[1],
        bottom_right: values[2],
        bottom_left: values[3],
    }
}

fn assert_resized_magnifier(
    actual: snow_draw_engine_document::MagnifierData,
    original: snow_draw_engine_document::MagnifierData,
    bounds: snow_draw_engine_editor::SelectionBounds,
    sx: f64,
    sy: f64,
    from_center: bool,
    grouped: bool,
) {
    let expected_source = resized_point(original.source_center, bounds, sx, sy, from_center);
    let expected_lens = if grouped {
        resized_point(original.magnified_center, bounds, sx, sy, from_center)
    } else {
        original.magnified_center
    };
    for (actual, expected) in [
        (actual.source_center.x, expected_source.x),
        (actual.source_center.y, expected_source.y),
        (actual.magnified_center.x, expected_lens.x),
        (actual.magnified_center.y, expected_lens.y),
        (actual.width, original.width * sx.abs()),
        (actual.height, original.height * sy.abs()),
    ] {
        assert!((actual - expected).abs() < 1e-8, "{actual} != {expected}");
    }
    assert_eq!(actual.rotation, original.rotation);
    assert_eq!(actual.factor, original.factor);
    assert_eq!(actual.stroke_width, original.stroke_width);
    assert_eq!(
        actual.corner_radii,
        reflected_radii(original.shape, original.corner_radii, sx, sy)
    );
}

#[test]
fn magnifier_group_resize_preserves_signed_transform_in_preview_commit_and_history() {
    for shape in [HighlightShape::Rectangle, HighlightShape::Diamond] {
        for companion in [
            ResizeCompanion::Rectangle,
            ResizeCompanion::Arrow,
            ResizeCompanion::Magnifier,
        ] {
            for (sx, sy) in [(-1.0, 1.0), (1.0, -1.0), (-1.0, -1.0), (1.5, 0.75)] {
                for from_center in [false, true] {
                    let (mut engine, viewport, ids) =
                        magnifier_resize_fixture(shape, Some(companion));
                    let originals: Vec<_> = ids
                        .iter()
                        .filter_map(|id| {
                            engine.model.magnifier(*id).ok().map(|value| (*id, *value))
                        })
                        .collect();
                    let original_arrow = engine.model.arrow(ids[1]).ok().cloned();
                    let original_rectangle = engine.model.rectangle(ids[1]).ok().copied();
                    let (bounds, to) =
                        resize_selection_by_scales(&mut engine, viewport, sx, sy, from_center);
                    let presentation = engine.editor.presentation_state(
                        &engine.model,
                        &engine.viewport_slot(viewport).unwrap().view,
                    );
                    for (id, original) in &originals {
                        let preview = presentation
                            .preview_magnifiers
                            .iter()
                            .find(|preview| preview.id == *id)
                            .unwrap();
                        assert_resized_magnifier(
                            preview.magnifier,
                            *original,
                            bounds,
                            sx,
                            sy,
                            from_center,
                            true,
                        );
                    }
                    pointer(
                        &mut engine,
                        viewport,
                        PointerEventType::Up,
                        to.x + 400.0,
                        to.y + 300.0,
                        from_center,
                    );
                    let committed: Vec<_> = originals
                        .iter()
                        .map(|(id, original)| {
                            let value = *engine.model.magnifier(*id).unwrap();
                            assert_resized_magnifier(
                                value,
                                *original,
                                bounds,
                                sx,
                                sy,
                                from_center,
                                true,
                            );
                            (*id, value)
                        })
                        .collect();
                    if let Some(original) = original_rectangle {
                        let actual = engine.model.rectangle(ids[1]).unwrap().center;
                        let expected = resized_point(original.center, bounds, sx, sy, from_center);
                        assert!((actual.x - expected.x).abs() < 1e-8);
                        assert!((actual.y - expected.y).abs() < 1e-8);
                    }
                    if let Some(original) = original_arrow {
                        let actual = engine.model.arrow(ids[1]).unwrap().global_points();
                        for (actual, original) in actual.iter().zip(original.global_points()) {
                            let expected = resized_point(original, bounds, sx, sy, from_center);
                            assert!((actual.x - expected.x).abs() < 1e-8);
                            assert!((actual.y - expected.y).abs() < 1e-8);
                        }
                    }
                    assert!(engine.undo().unwrap());
                    for (id, original) in &originals {
                        assert_eq!(engine.model.magnifier(*id).unwrap(), original);
                    }
                    assert!(engine.redo().unwrap());
                    for (id, value) in &committed {
                        assert_eq!(engine.model.magnifier(*id).unwrap(), value);
                    }
                }
            }
        }
    }
}

#[test]
fn magnifier_centered_reflection_commits_when_group_proxies_are_unchanged() {
    let (mut engine, viewport, ids) =
        magnifier_resize_fixture(HighlightShape::Rectangle, Some(ResizeCompanion::Magnifier));
    let mut transaction = Transaction::new("symmetric magnifiers");
    let values = [
        (Point::new(-60.0, -40.0), Point::new(60.0, 40.0)),
        (Point::new(60.0, -40.0), Point::new(-60.0, 40.0)),
    ];
    for (id, (source_center, magnified_center)) in ids.iter().zip(values) {
        transaction.update_magnifier(
            *id,
            snow_draw_engine_document::MagnifierData {
                source_center,
                magnified_center,
                width: 20.0,
                height: 20.0,
                factor: 1.0,
                corner_radii: CornerRadii::default(),
                ..Default::default()
            },
        );
    }
    engine
        .apply_editor_command(
            viewport,
            EditorCommand::ApplyTransaction(ApplyTransactionCommand {
                transaction,
                history_undo_snapshot: None,
            }),
        )
        .unwrap();
    let original_presentation = engine
        .editor
        .presentation_state(&engine.model, &engine.viewport_slot(viewport).unwrap().view);
    let originals: Vec<_> = ids
        .iter()
        .map(|id| (*id, *engine.model.magnifier(*id).unwrap()))
        .collect();
    let (bounds, to) = resize_selection_by_scales(&mut engine, viewport, -1.0, -1.0, true);
    let preview = engine
        .editor
        .presentation_state(&engine.model, &engine.viewport_slot(viewport).unwrap().view);
    assert_eq!(
        preview.preview_elements,
        original_presentation.selection_elements
    );
    assert_eq!(
        preview.selection_bounds,
        original_presentation.selection_bounds
    );
    for (id, original) in &originals {
        let value = preview
            .preview_magnifiers
            .iter()
            .find(|value| value.id == *id)
            .unwrap();
        assert_resized_magnifier(value.magnifier, *original, bounds, -1.0, -1.0, true, true);
    }
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Up,
        to.x + 400.0,
        to.y + 300.0,
        true,
    );
    for (id, original) in &originals {
        assert_resized_magnifier(
            *engine.model.magnifier(*id).unwrap(),
            *original,
            bounds,
            -1.0,
            -1.0,
            true,
            true,
        );
    }
    assert!(engine.undo().unwrap());
    for (id, original) in &originals {
        assert_eq!(engine.model.magnifier(*id).unwrap(), original);
    }
    assert!(engine.redo().unwrap());
    for (id, original) in &originals {
        assert_resized_magnifier(
            *engine.model.magnifier(*id).unwrap(),
            *original,
            bounds,
            -1.0,
            -1.0,
            true,
            true,
        );
    }
}

#[test]
fn magnifier_single_resize_reflects_shape_corners_with_fixed_lens_center() {
    for shape in [HighlightShape::Rectangle, HighlightShape::Diamond] {
        for (sx, sy) in [(-1.0, 1.0), (1.0, -1.0), (-1.0, -1.0)] {
            let (mut engine, viewport, ids) = magnifier_resize_fixture(shape, None);
            let original = *engine.model.magnifier(ids[0]).unwrap();
            let (bounds, to) = resize_selection_by_scales(&mut engine, viewport, sx, sy, false);
            pointer(
                &mut engine,
                viewport,
                PointerEventType::Up,
                to.x + 400.0,
                to.y + 300.0,
                false,
            );
            assert_resized_magnifier(
                *engine.model.magnifier(ids[0]).unwrap(),
                original,
                bounds,
                sx,
                sy,
                false,
                false,
            );
        }
    }
}

#[test]
fn magnifier_wheel_clamps_and_remembers_factor_without_changing_border_or_camera() {
    let (mut engine, viewport, id) = fixture(HighlightShape::Ellipse, 2.0);
    select(&mut engine, viewport, id);
    let before = *engine.model.magnifier(id).unwrap();
    let camera = engine.viewport_slot(viewport).unwrap().view;
    for (delta, expected) in [(360.0, 2.3), (24000.0, 10.0), (-24000.0, 1.0)] {
        assert!(
            engine
                .process_input(
                    viewport,
                    InputEvent::Wheel(WheelEvent {
                        position: Point::new(400.0, 300.0),
                        delta: snow_draw_engine_core::Vector2 { x: 0.0, y: delta },
                        delta_kind: WheelDeltaKind::Angle,
                        modifiers: Modifiers::default(),
                    })
                )
                .unwrap()
                .consumed
        );
        assert_eq!(engine.model.magnifier(id).unwrap().factor, expected);
    }
    let after = *engine.model.magnifier(id).unwrap();
    assert_eq!(
        (after.stroke, after.stroke_width),
        (before.stroke, before.stroke_width)
    );
    assert_eq!(
        (after.source_center, after.magnified_center),
        (before.source_center, before.magnified_center)
    );
    assert_eq!(engine.viewport_slot(viewport).unwrap().view, camera);
    engine
        .reset_editing_state_with_viewport_changes(viewport)
        .unwrap();
    engine
        .set_viewport_active_tool(viewport, ActiveTool::Magnifier)
        .unwrap();
    assert_eq!(
        engine
            .viewport_style_toolbar_state(viewport)
            .unwrap()
            .magnifier_style
            .factor,
        1.0
    );
    assert_eq!(
        engine.set_viewport_magnifier_style_patch(
            viewport,
            MagnifierStyle {
                factor: 8.0,
                ..Default::default()
            },
            MAGNIFIER_STYLE_PROPERTY_FACTOR,
            false
        ),
        Err(ErrorCode::InvalidState)
    );
    assert_eq!(
        engine
            .viewport_style_toolbar_state(viewport)
            .unwrap()
            .magnifier_style
            .factor,
        1.0
    );
}

#[test]
fn magnifier_selection_preview_and_commit_use_full_pointer_down_snapshot() {
    let (mut engine, viewport, id) = fixture(HighlightShape::Rectangle, 2.0);
    select(&mut engine, viewport, id);
    let original = *engine.model.magnifier(id).unwrap();
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Down,
        400.0,
        300.0,
        false,
    );
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Move,
        420.0,
        315.0,
        false,
    );
    let mut changed = original;
    changed.factor = 6.0;
    changed.magnified_center = Point::new(200.0, 100.0);
    let mut transaction = Transaction::new("concurrent style update");
    transaction.update_magnifier(id, changed);
    engine.model.apply_transaction(transaction).unwrap();
    let patch = engine.acquire_patch(viewport, None).unwrap();
    let lens = patch
        .scene
        .ops
        .iter()
        .flat_map(|op| &op.insert_items)
        .find_map(|item| {
            if let SceneDisplayItem::Magnifier(value) = item {
                Some(value)
            } else {
                None
            }
        })
        .unwrap();
    assert_eq!(lens.factor, original.factor);
    assert_eq!((lens.lens.center_x, lens.lens.center_y), (20.0, 15.0));
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Up,
        420.0,
        315.0,
        false,
    );
    assert_eq!(
        *engine.model.magnifier(id).unwrap(),
        original.translated(Point::new(20.0, 15.0))
    );
}

#[test]
fn magnifier_actual_shape_source_hit_precedes_lens_inside_generic_selection_frame() {
    for (shape, point) in [
        (HighlightShape::Ellipse, Point::new(40.0, 30.0)),
        (HighlightShape::Diamond, Point::new(30.0, 25.0)),
    ] {
        let (mut engine, viewport, id) = fixture(shape, 2.0);
        select(&mut engine, viewport, id);
        let before = *engine.model.magnifier(id).unwrap();
        drag(
            &mut engine,
            viewport,
            point,
            Point::new(point.x + 100.0, point.y),
        );
        let moved = *engine.model.magnifier(id).unwrap();
        assert_eq!(moved.source_center, before.source_center);
        assert_eq!(moved.magnified_center, Point::new(100.0, 0.0));
    }
}

#[test]
fn magnifier_source_alt_drag_duplicates_both_regions_as_one_history_entry() {
    let (mut engine, viewport, id) = fixture(HighlightShape::Rectangle, 2.0);
    select(&mut engine, viewport, id);
    let original = *engine.model.magnifier(id).unwrap();
    for (kind, x, y) in [
        (PointerEventType::Down, 400.0, 300.0),
        (PointerEventType::Move, 430.0, 320.0),
        (PointerEventType::Up, 430.0, 320.0),
    ] {
        pointer(&mut engine, viewport, kind, x, y, true);
    }
    assert_eq!(engine.model.paint_order().len(), 2);
    assert_eq!(*engine.model.magnifier(id).unwrap(), original);
    let copy = engine.model.paint_order()[1];
    assert_eq!(
        *engine.model.magnifier(copy).unwrap(),
        original.translated(Point::new(30.0, 20.0))
    );
    assert!(engine.undo().unwrap());
    assert_eq!(engine.model.paint_order(), &[id]);
}

#[test]
fn magnifier_source_alt_drag_preview_keeps_original_stationary() {
    for shape in [
        HighlightShape::Rectangle,
        HighlightShape::Ellipse,
        HighlightShape::Diamond,
    ] {
        let (mut engine, viewport, id) = fixture(shape, 2.0);
        select(&mut engine, viewport, id);
        let original = scene_magnifiers(&engine, viewport).remove(0);
        pointer(
            &mut engine,
            viewport,
            PointerEventType::Down,
            400.0,
            300.0,
            true,
        );
        pointer(
            &mut engine,
            viewport,
            PointerEventType::Move,
            430.0,
            320.0,
            true,
        );
        let preview = scene_magnifiers(&engine, viewport);
        assert_eq!(preview.len(), 2);
        assert_eq!(
            preview
                .iter()
                .find(|value| value.lens.id == original.lens.id),
            Some(&original),
            "copying must keep the committed original in place"
        );
        let copy = preview
            .iter()
            .find(|value| value.lens.id != original.lens.id)
            .unwrap();
        assert_eq!(copy.source_center, Point::new(30.0, 20.0));
        assert_eq!((copy.lens.center_x, copy.lens.center_y), (30.0, 20.0));
        pointer(
            &mut engine,
            viewport,
            PointerEventType::Up,
            430.0,
            320.0,
            true,
        );
        assert_eq!(scene_magnifiers(&engine, viewport), preview);
    }
}

#[test]
fn magnifier_pending_eraser_preview_fades_lens_and_leader_without_changing_geometry() {
    for shape in [
        HighlightShape::Rectangle,
        HighlightShape::Ellipse,
        HighlightShape::Diamond,
    ] {
        let (mut engine, viewport, id) = fixture(shape, 2.0);
        select(&mut engine, viewport, id);
        engine
            .set_viewport_magnifier_style_patch(
                viewport,
                MagnifierStyle {
                    connection_type: snow_draw_engine_core::MagnifierConnectionType::LeaderLine,
                    ..Default::default()
                },
                MAGNIFIER_STYLE_PROPERTY_CONNECTION_TYPE,
                false,
            )
            .unwrap();
        let grip = handle(&mut engine, viewport, UiShapeKind::MagnifierMoveHandle);
        drag(
            &mut engine,
            viewport,
            grip,
            Point::new(grip.x + 250.0, grip.y),
        );
        engine
            .set_selected_opacity_with_viewport_changes(viewport, 0.6)
            .unwrap();
        let original = scene_magnifiers(&engine, viewport).remove(0);
        assert!(original.leader.is_some());
        engine
            .set_viewport_active_tool(viewport, ActiveTool::Eraser)
            .unwrap();
        pointer(
            &mut engine,
            viewport,
            PointerEventType::Down,
            650.0,
            300.0,
            false,
        );
        let mut expected = original.clone();
        expected.lens.opacity *= 0.5;
        expected.leader.as_mut().unwrap().opacity *= 0.5;
        assert_eq!(scene_magnifiers(&engine, viewport), vec![expected]);
        pointer(
            &mut engine,
            viewport,
            PointerEventType::Up,
            650.0,
            300.0,
            false,
        );
        assert!(scene_magnifiers(&engine, viewport).is_empty());
        assert!(engine.undo().unwrap());
        assert_eq!(scene_magnifiers(&engine, viewport), vec![original]);
    }
}

#[test]
fn magnifier_alignment_history_restores_compound_bounds_and_single_source_editing() {
    let (mut engine, viewport, id) = fixture(HighlightShape::Diamond, 2.0);
    let mut original = *engine.model.magnifier(id).unwrap();
    original.magnified_center = Point::new(200.0, 100.0);
    original.rotation = std::f64::consts::FRAC_PI_3;
    let companion_id = engine.model.peek_next_element_id();
    let mut companion = original.source_rect();
    companion.center = Point::new(500.0, 400.0);
    companion.rotation = 0.0;
    let mut transaction = Transaction::new("alignment fixture");
    transaction.update_magnifier(id, original);
    transaction.insert_rectangle(companion_id, Default::default(), companion);
    engine
        .apply_editor_command(
            viewport,
            EditorCommand::ApplyTransaction(ApplyTransactionCommand {
                transaction,
                history_undo_snapshot: None,
            }),
        )
        .unwrap();
    let snapshot = serde_json::from_value(serde_json::json!({
        "selectedIds": [id, companion_id], "primaryId": id
    }))
    .unwrap();
    engine
        .editor
        .restore_history_selection(&engine.model, &snapshot);
    engine.refresh_all_viewports().unwrap();
    let selection_bounds = |engine: &Engine| {
        engine
            .editor
            .presentation_state(&engine.model, &engine.viewport_slot(viewport).unwrap().view)
            .selection_bounds
            .unwrap()
    };
    let before_bounds = selection_bounds(&engine);
    engine
        .align_selected_with_viewport_changes(viewport, 2)
        .unwrap();
    let aligned = *engine.model.magnifier(id).unwrap();
    let aligned_companion = *engine.model.rectangle(companion_id).unwrap();
    let aligned_bounds = selection_bounds(&engine);
    assert_ne!(aligned, original);
    assert!(
        (aligned.group_rect().center.x + aligned.group_rect().width / 2.0
            - (aligned_companion.center.x + aligned_companion.width / 2.0))
            .abs()
            < 1e-9
    );
    assert!(engine.undo().unwrap());
    assert_eq!(*engine.model.magnifier(id).unwrap(), original);
    assert_eq!(*engine.model.rectangle(companion_id).unwrap(), companion);
    assert_eq!(engine.selected_ids(), vec![id, companion_id]);
    assert_eq!(selection_bounds(&engine), before_bounds);
    assert!(engine.redo().unwrap());
    assert_eq!(*engine.model.magnifier(id).unwrap(), aligned);
    assert_eq!(
        *engine.model.rectangle(companion_id).unwrap(),
        aligned_companion
    );
    assert_eq!(selection_bounds(&engine), aligned_bounds);

    select(&mut engine, viewport, id);
    let single = selection_bounds(&engine);
    assert_eq!(single.center, aligned.source_center);
    assert_eq!(single.width, aligned.width);
    assert_eq!(single.height, aligned.height);
    assert_eq!(single.rotation, aligned.rotation);
}
