use super::*;
use snow_draw_engine_core::{Vector2, arrow::Arrowhead};
use snow_draw_engine_display::{OverlayDisplayItem, SceneDisplayItem, UiShapeKind};
use snow_draw_engine_document::{HighlightShape, MagnifierData, Transaction};
use snow_draw_engine_editor::{
    MAGNIFIER_STYLE_PROPERTY_ALL, MAGNIFIER_STYLE_PROPERTY_FACTOR, MagnifierStyle,
    magnifier_move_grip,
};
use snow_draw_engine_interaction::{
    Modifiers, PointerButton, PointerButtons, PointerDevice, PointerEvent, PointerEventType,
    WheelDeltaKind, WheelEvent,
};

fn fixture() -> (Engine, ViewportId, ElementId, MagnifierData) {
    let mut engine = Engine::default();
    let viewport = engine.create_viewport(ViewportConfig::default()).unwrap();
    engine
        .set_viewport_surface_size(viewport, 800, 600)
        .unwrap();
    let id = engine.model.allocate_element_id();
    let value = MagnifierData {
        source_center: Point::new(-120.0, 0.0),
        magnified_center: Point::new(120.0, 0.0),
        width: 80.0,
        height: 60.0,
        ..Default::default()
    };
    let mut transaction = Transaction::new("insert magnifier");
    transaction.insert_magnifier(id, Default::default(), value);
    engine
        .commit_transaction(
            viewport,
            ApplyTransactionCommand {
                transaction,
                history_undo_snapshot: None,
            },
        )
        .unwrap();
    engine
        .select_element_with_viewport_changes(viewport, id)
        .unwrap();
    (engine, viewport, id, value)
}

fn pointer(engine: &mut Engine, viewport: ViewportId, kind: PointerEventType, point: Point<f64>) {
    engine
        .process_input(
            viewport,
            InputEvent::Pointer(PointerEvent {
                pressure: None,
                timestamp_ms: 0,
                pointer_id: 1,
                event_type: kind,
                device: PointerDevice::Mouse,
                position: Point::new(point.x + 400.0, point.y + 300.0),
                button: Some(PointerButton::Primary),
                buttons: PointerButtons(PointerButtons::PRIMARY),
                modifiers: Modifiers::default(),
            }),
        )
        .unwrap();
}

fn wheel(engine: &mut Engine, viewport: ViewportId) {
    engine
        .process_input(
            viewport,
            InputEvent::Wheel(WheelEvent {
                position: Point::new(400.0, 300.0),
                delta: Vector2 { x: 0.0, y: 120.0 },
                delta_kind: WheelDeltaKind::Angle,
                modifiers: Modifiers::default(),
            }),
        )
        .unwrap();
}

fn scene_magnifier(
    engine: &Engine,
    viewport: ViewportId,
) -> snow_draw_engine_display::MagnifierDisplayItem {
    engine
        .acquire_patch(viewport, None)
        .unwrap()
        .scene
        .ops
        .iter()
        .flat_map(|op| &op.insert_items)
        .find_map(|item| match item {
            SceneDisplayItem::Magnifier(value) => Some(value.clone()),
            _ => None,
        })
        .unwrap()
}

#[test]
fn magnifier_wheel_during_drag_updates_preview_commit_and_history() {
    for lens in [false, true] {
        let (mut engine, viewport, id, original) = fixture();
        let start = if lens {
            magnifier_move_grip(original, 1.0)
        } else {
            original.source_center
        };
        let end = Point::new(start.x + 30.0, start.y + 15.0);
        pointer(&mut engine, viewport, PointerEventType::Down, start);
        pointer(&mut engine, viewport, PointerEventType::Move, end);
        wheel(&mut engine, viewport);
        assert_eq!(
            engine
                .viewport_style_toolbar_state(viewport)
                .unwrap()
                .magnifier_style
                .factor,
            2.1
        );
        assert_eq!(scene_magnifier(&engine, viewport).factor, 2.1);
        assert_eq!(
            engine.model.magnifier(id).unwrap().source_center,
            original.source_center
        );
        assert_eq!(
            engine.model.magnifier(id).unwrap().magnified_center,
            original.magnified_center
        );
        pointer(&mut engine, viewport, PointerEventType::Up, end);
        let mut expected = if lens {
            original
        } else {
            original.translated(Point::new(30.0, 15.0))
        };
        if lens {
            expected.magnified_center = Point::new(150.0, 15.0);
        }
        expected.factor = 2.1;
        assert_eq!(*engine.model.magnifier(id).unwrap(), expected);
        engine.undo().unwrap();
        assert_eq!(
            *engine.model.magnifier(id).unwrap(),
            MagnifierData {
                factor: 2.1,
                ..original
            }
        );
        engine.undo().unwrap();
        assert_eq!(*engine.model.magnifier(id).unwrap(), original);
        engine.redo().unwrap();
        engine.redo().unwrap();
        assert_eq!(*engine.model.magnifier(id).unwrap(), expected);
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
            2.1
        );
    }
}

#[test]
fn magnifier_wheel_before_drag_threshold_survives_selection_transition() {
    let (mut engine, viewport, id, original) = fixture();
    let start = original.source_center;
    let end = Point::new(start.x + 30.0, start.y + 15.0);
    pointer(&mut engine, viewport, PointerEventType::Down, start);
    wheel(&mut engine, viewport);
    pointer(&mut engine, viewport, PointerEventType::Move, end);
    assert_eq!(scene_magnifier(&engine, viewport).factor, 2.1);
    pointer(&mut engine, viewport, PointerEventType::Up, end);
    assert_eq!(
        *engine.model.magnifier(id).unwrap(),
        MagnifierData {
            factor: 2.1,
            ..original.translated(Point::new(30.0, 15.0))
        }
    );
}

#[test]
fn magnifier_wheel_undo_and_cancel_during_lens_drag_preserve_committed_style() {
    let (mut engine, viewport, id, original) = fixture();
    let start = magnifier_move_grip(original, 1.0);
    let end = Point::new(start.x + 30.0, start.y + 15.0);
    pointer(&mut engine, viewport, PointerEventType::Down, start);
    pointer(&mut engine, viewport, PointerEventType::Move, end);
    wheel(&mut engine, viewport);
    engine.undo().unwrap();
    assert_eq!(scene_magnifier(&engine, viewport).factor, 2.0);
    engine.redo().unwrap();
    assert_eq!(scene_magnifier(&engine, viewport).factor, 2.1);
    pointer(&mut engine, viewport, PointerEventType::Cancel, end);
    assert_eq!(
        *engine.model.magnifier(id).unwrap(),
        MagnifierData {
            factor: 2.1,
            ..original
        }
    );
    engine.undo().unwrap();
    assert_eq!(*engine.model.magnifier(id).unwrap(), original);
}

#[test]
fn magnifier_style_api_during_lens_drag_rebases_appearance_without_committing_motion() {
    let (mut engine, viewport, id, original) = fixture();
    let start = magnifier_move_grip(original, 1.0);
    let end = Point::new(start.x + 30.0, start.y + 15.0);
    pointer(&mut engine, viewport, PointerEventType::Down, start);
    pointer(&mut engine, viewport, PointerEventType::Move, end);
    let style = MagnifierStyle {
        factor: 3.0,
        shape: HighlightShape::Ellipse,
        stroke_width: 4.0,
        connection_type: snow_draw_engine_core::MagnifierConnectionType::LeaderLine,
        source_outline: false,
        leader_arrowhead: Some(Arrowhead::Arrow),
        ..MagnifierStyle::from_data(&original)
    };
    engine
        .set_viewport_magnifier_style_patch(viewport, style, MAGNIFIER_STYLE_PROPERTY_ALL, false)
        .unwrap();
    let scene = scene_magnifier(&engine, viewport);
    assert_eq!(scene.factor, 3.0);
    assert_eq!(scene.connection_type, style.connection_type);
    assert!(!scene.source_outline);
    assert_eq!(scene.lens.stroke_width, 4.0);
    assert_eq!((scene.lens.center_x, scene.lens.center_y), (150.0, 15.0));
    assert_eq!(
        engine.model.magnifier(id).unwrap().magnified_center,
        original.magnified_center
    );
    pointer(&mut engine, viewport, PointerEventType::Up, end);
    let mut expected = original;
    style.apply(&mut expected);
    expected.magnified_center = Point::new(150.0, 15.0);
    assert_eq!(*engine.model.magnifier(id).unwrap(), expected);
}

#[test]
fn invalid_magnifier_style_during_drag_leaves_document_and_preview_unchanged() {
    let (mut engine, viewport, id, original) = fixture();
    let start = magnifier_move_grip(original, 1.0);
    let end = Point::new(start.x + 30.0, start.y + 15.0);
    pointer(&mut engine, viewport, PointerEventType::Down, start);
    pointer(&mut engine, viewport, PointerEventType::Move, end);
    let preview = scene_magnifier(&engine, viewport);
    let result = engine.set_viewport_magnifier_style_patch(
        viewport,
        MagnifierStyle {
            factor: 11.0,
            ..MagnifierStyle::from_data(&original)
        },
        MAGNIFIER_STYLE_PROPERTY_FACTOR,
        false,
    );
    assert_eq!(result, Err(ErrorCode::InvalidArgument));
    assert_eq!(*engine.model.magnifier(id).unwrap(), original);
    assert_eq!(scene_magnifier(&engine, viewport), preview);
    pointer(&mut engine, viewport, PointerEventType::Up, end);
    assert_eq!(engine.model.magnifier(id).unwrap().factor, 2.0);
}

fn preview_magnifier(engine: &Engine, viewport: ViewportId, id: ElementId) -> MagnifierData {
    engine
        .editor
        .presentation_state(&engine.model, &engine.viewport_slot(viewport).unwrap().view)
        .preview_magnifiers
        .into_iter()
        .find(|value| value.id == id)
        .unwrap()
        .magnifier
}

#[test]
fn magnifier_wheel_during_source_handle_drag_preserves_in_progress_geometry() {
    for kind in [
        UiShapeKind::SelectionResizeHandle,
        UiShapeKind::SelectionRotationHandle,
        UiShapeKind::SelectionCornerRadiusHandle,
    ] {
        let (mut engine, viewport, id, original) = fixture();
        let start = engine
            .acquire_patch(viewport, None)
            .unwrap()
            .overlay
            .ops
            .iter()
            .flat_map(|op| &op.insert_items)
            .find_map(|item| match item {
                OverlayDisplayItem::Rectangle(value) if value.kind == kind => {
                    Some(Point::new(value.center_x, value.center_y))
                }
                _ => None,
            })
            .unwrap();
        let end = match kind {
            UiShapeKind::SelectionResizeHandle => Point::new(start.x - 30.0, start.y - 20.0),
            UiShapeKind::SelectionRotationHandle => Point::new(
                original.source_center.x - (start.y - original.source_center.y),
                original.source_center.y + (start.x - original.source_center.x),
            ),
            _ => Point::new(start.x + 15.0, start.y + 15.0),
        };
        pointer(&mut engine, viewport, PointerEventType::Down, start);
        pointer(&mut engine, viewport, PointerEventType::Move, end);
        let mut expected = preview_magnifier(&engine, viewport, id);
        assert_ne!(expected, original, "the {kind:?} drag must change geometry");
        wheel(&mut engine, viewport);
        expected.factor = 2.1;
        assert_eq!(
            preview_magnifier(&engine, viewport, id),
            expected,
            "{kind:?}"
        );
        pointer(&mut engine, viewport, PointerEventType::Up, end);
        assert_eq!(*engine.model.magnifier(id).unwrap(), expected, "{kind:?}");
        engine.undo().unwrap();
        assert_eq!(
            *engine.model.magnifier(id).unwrap(),
            MagnifierData {
                factor: 2.1,
                ..original
            }
        );
    }
}

#[test]
fn magnifier_corner_drag_canonicalizes_oversized_radii_without_rescaling_other_corners() {
    let (mut engine, viewport, id, _) = fixture();
    engine
        .set_viewport_magnifier_style_patch(
            viewport,
            MagnifierStyle {
                corner_radii: snow_draw_engine_core::CornerRadii::splat(1000.0),
                ..Default::default()
            },
            snow_draw_engine_editor::MAGNIFIER_STYLE_PROPERTY_CORNER_RADII,
            false,
        )
        .unwrap();
    let before = *engine.model.magnifier(id).unwrap();
    let start = engine
        .acquire_patch(viewport, None)
        .unwrap()
        .overlay
        .ops
        .iter()
        .flat_map(|op| &op.insert_items)
        .find_map(|item| match item {
            OverlayDisplayItem::Rectangle(value)
                if value.kind == UiShapeKind::SelectionCornerRadiusHandle =>
            {
                Some(Point::new(value.center_x, value.center_y))
            }
            _ => None,
        })
        .unwrap();
    let end = Point::new(start.x - 5.0, start.y - 5.0);
    pointer(&mut engine, viewport, PointerEventType::Down, start);
    pointer(&mut engine, viewport, PointerEventType::Move, end);
    let after = preview_magnifier(&engine, viewport, id);
    let radii = after.magnified_rect().corner_radii;
    assert!(radii.top_left > 20.0 && radii.top_left < 60.0);
    assert_eq!(
        radii.top_right,
        before.magnified_rect().corner_radii.top_right
    );
    assert_eq!(
        radii.bottom_right,
        before.magnified_rect().corner_radii.bottom_right
    );
    assert_eq!(
        radii.bottom_left,
        before.magnified_rect().corner_radii.bottom_left
    );
    pointer(&mut engine, viewport, PointerEventType::Up, end);
    assert_eq!(*engine.model.magnifier(id).unwrap(), after);
    assert!(engine.undo().unwrap());
    assert_eq!(*engine.model.magnifier(id).unwrap(), before);
}

#[test]
fn magnifier_corner_drag_continues_smoothly_after_factor_wheel_rebases_the_source() {
    for shape in [HighlightShape::Rectangle, HighlightShape::Diamond] {
        let (mut engine, viewport, id, _) = fixture();
        engine
            .set_viewport_magnifier_style_patch(
                viewport,
                MagnifierStyle {
                    shape,
                    ..Default::default()
                },
                snow_draw_engine_editor::MAGNIFIER_STYLE_PROPERTY_SHAPE,
                false,
            )
            .unwrap();
        let start = engine
            .acquire_patch(viewport, None)
            .unwrap()
            .overlay
            .ops
            .iter()
            .flat_map(|op| &op.insert_items)
            .find_map(|item| match item {
                OverlayDisplayItem::Rectangle(value)
                    if value.kind == UiShapeKind::SelectionCornerRadiusHandle =>
                {
                    Some(Point::new(value.center_x, value.center_y))
                }
                _ => None,
            })
            .unwrap();
        let end = Point::new(start.x + 1.0, start.y + 3.0);
        pointer(&mut engine, viewport, PointerEventType::Down, start);
        pointer(&mut engine, viewport, PointerEventType::Move, end);
        let before = preview_magnifier(&engine, viewport, id);
        engine
            .process_input(
                viewport,
                InputEvent::Wheel(WheelEvent {
                    position: Point::new(400.0, 300.0),
                    delta: Vector2 { x: 0.0, y: 2400.0 },
                    delta_kind: WheelDeltaKind::Angle,
                    modifiers: Modifiers::default(),
                }),
            )
            .unwrap();
        let rebased = preview_magnifier(&engine, viewport, id);
        assert_eq!(rebased.factor, 4.0);
        assert_eq!(rebased.corner_radii, before.corner_radii);
        let continued = if shape == HighlightShape::Rectangle {
            Point::new(end.x + 0.001, end.y)
        } else {
            Point::new(end.x, end.y + 0.001)
        };
        pointer(&mut engine, viewport, PointerEventType::Move, continued);
        let after = preview_magnifier(&engine, viewport, id);
        let delta = after.corner_radii.top_left - before.corner_radii.top_left;
        assert!(
            delta > 0.0 && delta < 0.1,
            "{shape:?}: radius jumped by {delta}"
        );
        assert_eq!(after.corner_radii.top_right, before.corner_radii.top_right);
        pointer(&mut engine, viewport, PointerEventType::Up, continued);
        assert_eq!(*engine.model.magnifier(id).unwrap(), after);
        assert!(engine.undo().unwrap());
        assert_eq!(engine.model.magnifier(id).unwrap().factor, 4.0);
    }
}

#[test]
fn magnifier_corner_drag_rebases_shape_and_radii_before_release_or_further_motion() {
    for shape in [HighlightShape::Rectangle, HighlightShape::Diamond] {
        for continue_drag in [false, true] {
            let (mut engine, viewport, id, original) = fixture();
            let start = engine
                .acquire_patch(viewport, None)
                .unwrap()
                .overlay
                .ops
                .iter()
                .flat_map(|op| &op.insert_items)
                .find_map(|item| match item {
                    OverlayDisplayItem::Rectangle(value)
                        if value.kind == UiShapeKind::SelectionCornerRadiusHandle =>
                    {
                        Some(Point::new(value.center_x, value.center_y))
                    }
                    _ => None,
                })
                .unwrap();
            let end = Point::new(start.x + 15.0, start.y + 15.0);
            pointer(&mut engine, viewport, PointerEventType::Down, start);
            pointer(&mut engine, viewport, PointerEventType::Move, end);
            let style = MagnifierStyle {
                shape,
                factor: 3.0,
                corner_radii: snow_draw_engine_core::CornerRadii::splat(18.0),
                source_outline: false,
                ..MagnifierStyle::from_data(&original)
            };
            engine
                .set_viewport_magnifier_style_patch(
                    viewport,
                    style,
                    MAGNIFIER_STYLE_PROPERTY_ALL,
                    false,
                )
                .unwrap();
            assert_eq!(
                MagnifierStyle::from_data(&preview_magnifier(&engine, viewport, id)),
                style
            );
            pointer(&mut engine, viewport, PointerEventType::Move, end);
            assert_eq!(
                MagnifierStyle::from_data(&preview_magnifier(&engine, viewport, id)),
                style
            );
            let release = if continue_drag {
                Point::new(end.x + 8.0, end.y + 8.0)
            } else {
                end
            };
            if continue_drag {
                pointer(&mut engine, viewport, PointerEventType::Move, release);
                assert_ne!(
                    preview_magnifier(&engine, viewport, id).corner_radii,
                    style.corner_radii
                );
            }
            let expected = preview_magnifier(&engine, viewport, id);
            pointer(&mut engine, viewport, PointerEventType::Up, release);
            assert_eq!(*engine.model.magnifier(id).unwrap(), expected);
            assert_eq!(expected.shape, shape);
            assert_eq!(
                expected.source_rect().corner_radii,
                expected
                    .magnified_rect()
                    .corner_radii
                    .scaled(1.0 / expected.factor)
            );
        }
    }
}

#[test]
fn magnifier_group_wheel_during_drag_preserves_both_committed_styles() {
    let (mut engine, viewport, first, original) = fixture();
    let second = engine.model.allocate_element_id();
    let companion = MagnifierData {
        source_center: Point::new(240.0, 0.0),
        magnified_center: Point::new(240.0, 0.0),
        width: 40.0,
        height: 40.0,
        ..original
    };
    let mut transaction = Transaction::new("insert companion magnifier");
    transaction.insert_magnifier(second, Default::default(), companion);
    engine
        .commit_transaction(
            viewport,
            ApplyTransactionCommand {
                transaction,
                history_undo_snapshot: None,
            },
        )
        .unwrap();
    engine
        .set_viewport_active_tool(viewport, ActiveTool::Select)
        .unwrap();
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Down,
        Point::new(-300.0, -180.0),
    );
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Up,
        Point::new(370.0, 180.0),
    );
    assert_eq!(engine.selected_ids(), [first, second]);
    let end = Point::new(
        original.source_center.x + 30.0,
        original.source_center.y + 15.0,
    );
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Down,
        original.source_center,
    );
    pointer(&mut engine, viewport, PointerEventType::Move, end);
    wheel(&mut engine, viewport);
    for (id, value) in [(first, original), (second, companion)] {
        assert_eq!(
            *engine.model.magnifier(id).unwrap(),
            MagnifierData {
                factor: 2.1,
                ..value
            }
        );
        assert_eq!(
            preview_magnifier(&engine, viewport, id),
            MagnifierData {
                factor: 2.1,
                ..value.translated(Point::new(30.0, 15.0))
            }
        );
    }
    pointer(&mut engine, viewport, PointerEventType::Up, end);
    engine.undo().unwrap();
    for (id, value) in [(first, original), (second, companion)] {
        assert_eq!(
            *engine.model.magnifier(id).unwrap(),
            MagnifierData {
                factor: 2.1,
                ..value
            }
        );
    }
    engine.undo().unwrap();
    for (id, value) in [(first, original), (second, companion)] {
        assert_eq!(*engine.model.magnifier(id).unwrap(), value);
    }
}

#[test]
fn magnifier_opacity_edit_during_source_and_lens_drag_preserves_motion_and_history() {
    for lens in [false, true] {
        let (mut engine, viewport, id, original) = fixture();
        let start = if lens {
            magnifier_move_grip(original, 1.0)
        } else {
            original.source_center
        };
        let end = Point::new(start.x + 30.0, start.y + 15.0);
        pointer(&mut engine, viewport, PointerEventType::Down, start);
        pointer(&mut engine, viewport, PointerEventType::Move, end);
        let mut expected = preview_magnifier(&engine, viewport, id);
        engine
            .set_selected_opacity_with_viewport_changes(viewport, 0.4)
            .unwrap();
        expected.opacity = 0.4;
        assert_eq!(preview_magnifier(&engine, viewport, id), expected);
        pointer(&mut engine, viewport, PointerEventType::Up, end);
        assert_eq!(*engine.model.magnifier(id).unwrap(), expected);
        engine.undo().unwrap();
        assert_eq!(
            *engine.model.magnifier(id).unwrap(),
            MagnifierData {
                opacity: 0.4,
                ..original
            }
        );
        engine.undo().unwrap();
        assert_eq!(*engine.model.magnifier(id).unwrap(), original);
    }
}
