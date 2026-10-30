use super::*;
use snow_draw_engine_document::{
    ElementMeta, HighlightShape, RectangleData, TextLayoutSize, Transaction,
};
use snow_draw_engine_editor::EditorPresentationState;
use snow_draw_engine_interaction::{
    KeyCode, KeyEvent, KeyEventType, Modifiers, PointerButton, PointerButtons, PointerDevice,
    PointerEvent, PointerEventType,
};

fn commit(engine: &mut Engine, viewport: ViewportId, transaction: Transaction) {
    engine
        .commit_transaction(
            viewport,
            ApplyTransactionCommand {
                transaction,
                history_undo_snapshot: None,
            },
        )
        .unwrap();
}

fn setup(shape: HighlightShape, rotation: f64, zoom: f64) -> (Engine, ViewportId, ElementId) {
    let mut engine = Engine::default();
    let viewport = engine.create_viewport(ViewportConfig::default()).unwrap();
    engine
        .set_viewport_surface_size(viewport, 800, 600)
        .unwrap();
    engine
        .set_viewport_camera(
            viewport,
            Camera {
                center: Point::default(),
                zoom,
            },
        )
        .unwrap();
    let owner = engine.model.peek_next_element_id();
    let mut transaction = Transaction::new("shape fixture");
    transaction.insert_rectangle(
        owner,
        ElementMeta::default(),
        RectangleData {
            center: Point::new(-30.0, -20.0),
            width: 150.0,
            height: 90.0,
            rotation,
            highlight_shape: shape,
            text_element_id: None,
            rectangle_kind: snow_draw_engine_document::RectangleElementKind::Rectangle,
            fill: snow_draw_engine_core::ColorRgba8::default(),
            fill_style: Default::default(),
            stroke: snow_draw_engine_core::ColorRgba8 {
                r: 80,
                g: 120,
                b: 220,
                a: 255,
            },
            stroke_width: 2.0,
            stroke_style: Default::default(),
            stroke_rendering_style: Default::default(),
            stroke_rendering_seed: 0,
            corner_radii: Default::default(),
            opacity: 1.0,
        },
    );
    commit(&mut engine, viewport, transaction);
    engine
        .set_viewport_active_tool(viewport, ActiveTool::Shape)
        .unwrap();
    engine
        .select_element_with_viewport_changes(viewport, owner)
        .unwrap();
    (engine, viewport, owner)
}

fn presentation(engine: &Engine, viewport: ViewportId) -> EditorPresentationState {
    engine
        .editor
        .presentation_state(&engine.model, &engine.viewport_slot(viewport).unwrap().view)
}

fn send(
    engine: &mut Engine,
    viewport: ViewportId,
    kind: PointerEventType,
    point: Point<f64>,
    pointer_id: u32,
    button: PointerButton,
) {
    let zoom = engine.viewport_slot(viewport).unwrap().view.camera.zoom;
    engine
        .process_input_with_viewport_changes(
            viewport,
            InputEvent::Pointer(PointerEvent {
                pressure: None,
                timestamp_ms: 0,
                pointer_id,
                event_type: kind,
                device: PointerDevice::Mouse,
                position: Point::new(point.x * zoom + 400.0, point.y * zoom + 300.0),
                button: Some(button),
                buttons: PointerButtons(PointerButtons::PRIMARY),
                modifiers: Modifiers::default(),
            }),
        )
        .unwrap();
}

fn pointer(engine: &mut Engine, viewport: ViewportId, kind: PointerEventType, point: Point<f64>) {
    send(engine, viewport, kind, point, 1, PointerButton::Primary);
}

fn begin(engine: &mut Engine, viewport: ViewportId) -> Point<f64> {
    let handle = presentation(engine, viewport).shape_text_handle.unwrap();
    pointer(engine, viewport, PointerEventType::Down, handle);
    handle
}

#[test]
fn shape_text_click_previews_then_commits_one_binding_and_requests_edit_at_all_zooms() {
    for shape in [
        HighlightShape::Rectangle,
        HighlightShape::Ellipse,
        HighlightShape::Diamond,
    ] {
        for zoom in [0.5, 1.0, 3.0] {
            let (mut engine, viewport, owner) = setup(shape, 0.65, zoom);
            let original = *engine.model.rectangle(owner).unwrap();
            let handle = begin(&mut engine, viewport);
            assert_eq!(
                engine.model.paint_order(),
                &[owner],
                "press is preview-only"
            );
            assert_eq!(*engine.model.rectangle(owner).unwrap(), original);
            assert_eq!(engine.take_text_edit_request(viewport).unwrap(), None);
            let preview = presentation(&engine, viewport)
                .shape_bound_text_preview
                .unwrap();
            assert_eq!(preview.owner_id, owner);
            assert_eq!(preview.text.center, handle);
            assert!(engine.model.text(preview.text_id).is_err());
            let request = engine
                .bound_label_layout_request(viewport)
                .unwrap()
                .unwrap();
            assert_eq!(request.text_id, preview.text_id);
            let result = engine
                .apply_bound_label_layout(
                    viewport,
                    request.text_id,
                    TextLayoutSize::new(32.0, 40.0),
                )
                .unwrap();
            assert_eq!(result.changed_viewports, vec![viewport]);
            assert!(
                engine
                    .bound_label_layout_request(viewport)
                    .unwrap()
                    .is_none()
            );
            pointer(&mut engine, viewport, PointerEventType::Up, handle);
            let text_id = engine.model.bound_text_id_for_shape(owner).unwrap();
            let text = engine.model.text(text_id).unwrap();
            assert_eq!(text.center, handle);
            assert_eq!(text.width(), 32.0);
            assert_eq!(text.height(), 40.0);
            let mut bound_shape = original;
            bound_shape.text_element_id = Some(text_id);
            assert_eq!(*engine.model.rectangle(owner).unwrap(), bound_shape);
            assert_eq!(
                engine.take_text_edit_request(viewport).unwrap(),
                Some(text_id)
            );
            assert_eq!(engine.take_text_edit_request(viewport).unwrap(), None);
            assert!(
                presentation(&engine, viewport)
                    .shape_bound_text_preview
                    .is_none()
            );
            assert!(engine.undo().unwrap());
            assert_eq!(engine.model.paint_order(), &[owner]);
            assert_eq!(*engine.model.rectangle(owner).unwrap(), original);
            assert!(engine.redo().unwrap());
            assert_eq!(engine.model.bound_text_id_for_shape(owner), Some(text_id));
            assert_eq!(engine.model.text(text_id).unwrap().center, handle);
        }
    }
}

#[test]
fn shape_text_drag_uses_release_position_and_reuses_existing_text() {
    for moves in [false, true] {
        let (mut engine, viewport, owner) = setup(HighlightShape::Diamond, 0.5, 1.5);
        begin(&mut engine, viewport);
        if moves {
            pointer(
                &mut engine,
                viewport,
                PointerEventType::Move,
                Point::new(120.0, 60.0),
            );
            assert_eq!(engine.model.paint_order(), &[owner]);
        }
        let end = Point::new(160.0, 125.0);
        pointer(&mut engine, viewport, PointerEventType::Up, end);
        let text_id = engine.model.bound_text_id_for_shape(owner).unwrap();
        assert_eq!(engine.model.text(text_id).unwrap().center, end);
        let mut original_text = engine.model.text(text_id).unwrap().clone();
        original_text.text = "Existing label".into();
        original_text.layout = TextLayoutSize::new(200.0, 45.0);
        original_text.font_size = 36.0;
        let mut tx = Transaction::new("label content");
        tx.update_text(text_id, original_text.clone());
        commit(&mut engine, viewport, tx);
        begin(&mut engine, viewport);
        assert!(
            engine
                .bound_label_layout_request(viewport)
                .unwrap()
                .is_none()
        );
        assert_eq!(engine.model.text(text_id).unwrap(), &original_text);
        let next = Point::new(-180.0, 140.0);
        pointer(&mut engine, viewport, PointerEventType::Move, next);
        pointer(&mut engine, viewport, PointerEventType::Up, next);
        let mut expected = original_text.clone();
        expected.center = next;
        assert_eq!(engine.model.text(text_id).unwrap(), &expected);
        assert_eq!(engine.model.paint_order().len(), 2);
        assert_eq!(engine.model.bound_text_id_for_shape(owner), Some(text_id));
        assert_eq!(
            engine.take_text_edit_request(viewport).unwrap(),
            Some(text_id)
        );
        engine.undo().unwrap();
        assert_eq!(engine.model.text(text_id).unwrap(), &original_text);
    }
}

#[test]
fn shape_text_cancel_escape_and_focus_loss_discard_new_and_existing_previews() {
    for existing in [false, true] {
        for mode in 0..3 {
            let (mut engine, viewport, owner) = setup(HighlightShape::Rectangle, 0.0, 1.0);
            if existing {
                let at = begin(&mut engine, viewport);
                pointer(&mut engine, viewport, PointerEventType::Up, at);
                engine.take_text_edit_request(viewport).unwrap();
            }
            let original_shape = *engine.model.rectangle(owner).unwrap();
            let original_text = engine
                .model
                .bound_text_id_for_shape(owner)
                .map(|id| (id, engine.model.text(id).unwrap().clone()));
            let before_history = engine.history_state();
            begin(&mut engine, viewport);
            let end = Point::new(200.0, 200.0);
            pointer(&mut engine, viewport, PointerEventType::Move, end);
            match mode {
                0 => pointer(&mut engine, viewport, PointerEventType::Cancel, end),
                1 => {
                    engine
                        .process_input(
                            viewport,
                            InputEvent::Key(KeyEvent {
                                event_type: KeyEventType::KeyDown,
                                key_code: KeyCode::Escape,
                                repeat: false,
                                modifiers: Modifiers::default(),
                            }),
                        )
                        .unwrap();
                }
                _ => {
                    engine
                        .process_input(viewport, InputEvent::FocusLost)
                        .unwrap();
                }
            }
            pointer(&mut engine, viewport, PointerEventType::Up, end);
            assert_eq!(*engine.model.rectangle(owner).unwrap(), original_shape);
            if let Some((id, text)) = original_text {
                assert_eq!(engine.model.text(id).unwrap(), &text);
            } else {
                assert_eq!(engine.model.paint_order(), &[owner]);
            }
            assert_eq!(engine.history_state(), before_history);
            assert_eq!(engine.take_text_edit_request(viewport).unwrap(), None);
            assert!(
                presentation(&engine, viewport)
                    .shape_bound_text_preview
                    .is_none()
            );
            assert!(!engine.editor.has_pointer_capture());
        }
    }
}

#[test]
fn shape_text_drag_rejects_other_pointer_secondary_release_and_stale_measurement() {
    let (mut engine, viewport, owner) = setup(HighlightShape::Ellipse, 0.0, 1.0);
    let handle = begin(&mut engine, viewport);
    let old_request = engine
        .bound_label_layout_request(viewport)
        .unwrap()
        .unwrap();
    send(
        &mut engine,
        viewport,
        PointerEventType::Move,
        Point::new(200.0, 100.0),
        2,
        PointerButton::Primary,
    );
    send(
        &mut engine,
        viewport,
        PointerEventType::Up,
        Point::new(200.0, 100.0),
        1,
        PointerButton::Secondary,
    );
    assert_eq!(
        presentation(&engine, viewport)
            .shape_bound_text_preview
            .unwrap()
            .text
            .center,
        handle
    );
    assert_eq!(engine.model.paint_order(), &[owner]);
    pointer(&mut engine, viewport, PointerEventType::Cancel, handle);
    begin(&mut engine, viewport);
    let request = engine
        .bound_label_layout_request(viewport)
        .unwrap()
        .unwrap();
    assert_ne!(request.text_id, old_request.text_id);
    assert!(
        engine
            .apply_bound_label_layout(
                viewport,
                old_request.text_id,
                TextLayoutSize::new(90.0, 90.0)
            )
            .unwrap()
            .changed_viewports
            .is_empty()
    );
    assert_eq!(
        engine
            .bound_label_layout_request(viewport)
            .unwrap()
            .unwrap(),
        request
    );
}

#[test]
fn shape_text_handle_follows_rotation_and_owner_removal_cancels_the_gesture() {
    let (mut engine, viewport, owner) =
        setup(HighlightShape::Rectangle, std::f64::consts::FRAC_PI_2, 2.0);
    let shape = *engine.model.rectangle(owner).unwrap();
    let handle = presentation(&engine, viewport).shape_text_handle.unwrap();
    assert!((handle.x - (shape.center.x - shape.height / 2.0 - 12.0)).abs() < 1e-9);
    assert!((handle.y - shape.center.y).abs() < 1e-9);
    begin(&mut engine, viewport);
    let mut transaction = Transaction::new("remove shape during gesture");
    transaction.remove_element(owner);
    commit(&mut engine, viewport, transaction);
    assert!(
        presentation(&engine, viewport)
            .shape_bound_text_preview
            .is_none()
    );
    assert!(!engine.editor.has_pointer_capture());
    pointer(&mut engine, viewport, PointerEventType::Up, handle);
    assert_eq!(engine.take_text_edit_request(viewport).unwrap(), None);
    assert!(engine.model.paint_order().is_empty());
}

#[test]
fn shape_text_handle_requires_one_visible_unlocked_shape_and_a_compatible_tool() {
    for mode in 0..4 {
        let (mut engine, viewport, owner) = setup(HighlightShape::Rectangle, 0.0, 1.0);
        let mut tx = Transaction::new("shape handle guard");
        let mut selected = owner;
        if mode < 2 {
            tx.update_element_meta(
                owner,
                ElementMeta {
                    locked: mode == 0,
                    visible: mode != 1,
                },
            );
        } else {
            let shape = *engine.model.rectangle(owner).unwrap();
            let shape = if mode == 2 {
                shape.into_highlight(HighlightShape::Rectangle)
            } else {
                shape.into_spotlight()
            };
            selected = engine.model.peek_next_element_id();
            tx.insert_rectangle(selected, ElementMeta::default(), shape);
        }
        commit(&mut engine, viewport, tx);
        if selected != owner {
            engine
                .set_viewport_active_tool(viewport, ActiveTool::Select)
                .unwrap();
            engine
                .select_element_with_viewport_changes(viewport, selected)
                .unwrap();
        }
        assert!(presentation(&engine, viewport).shape_text_handle.is_none());
    }
    let (mut engine, viewport, owner) = setup(HighlightShape::Rectangle, 0.0, 1.0);
    let handle = begin(&mut engine, viewport);
    engine
        .set_viewport_active_tool(viewport, ActiveTool::Eraser)
        .unwrap();
    assert!(presentation(&engine, viewport).shape_text_handle.is_none());
    assert!(
        presentation(&engine, viewport)
            .shape_bound_text_preview
            .is_none()
    );
    assert_eq!(engine.model.paint_order(), &[owner]);
    pointer(&mut engine, viewport, PointerEventType::Up, handle);
    assert_eq!(engine.take_text_edit_request(viewport).unwrap(), None);

    engine
        .set_viewport_active_tool(viewport, ActiveTool::Select)
        .unwrap();
    let second = engine.model.peek_next_element_id();
    let mut shape = *engine.model.rectangle(owner).unwrap();
    shape.center = Point::new(200.0, 100.0);
    let mut tx = Transaction::new("second shape");
    tx.insert_rectangle(second, ElementMeta::default(), shape);
    commit(&mut engine, viewport, tx);
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Down,
        Point::new(-150.0, -100.0),
    );
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Up,
        Point::new(300.0, 200.0),
    );
    assert_eq!(engine.selected_ids().len(), 2);
    assert!(presentation(&engine, viewport).shape_text_handle.is_none());
}

#[test]
fn shape_text_existing_label_click_moves_below_shape_without_duplication() {
    let (mut engine, viewport, owner) = setup(HighlightShape::Rectangle, 0.0, 1.0);
    begin(&mut engine, viewport);
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Up,
        Point::new(170.0, 110.0),
    );
    let text_id = engine.model.bound_text_id_for_shape(owner).unwrap();
    let previous = engine.model.text(text_id).unwrap().clone();
    let handle = begin(&mut engine, viewport);
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Up,
        Point::new(handle.x + 1.0, handle.y),
    );
    assert_eq!(engine.model.text(text_id).unwrap().center, handle);
    assert_eq!(engine.model.paint_order().len(), 2);
    assert_eq!(
        engine.take_text_edit_request(viewport).unwrap(),
        Some(text_id)
    );
    engine.undo().unwrap();
    assert_eq!(engine.model.text(text_id).unwrap(), &previous);
}
