use super::*;
use snow_draw_engine_document::speech_bubble_body_proxy;
use snow_draw_engine_editor::{EditorPresentationState, SpeechBubbleTextMeasurement};

fn presentation(engine: &Engine, viewport: ViewportId) -> EditorPresentationState {
    engine
        .editor
        .presentation_state(&engine.model, &engine.viewport_slot(viewport).unwrap().view)
}

fn bind_text(engine: &mut Engine, viewport: ViewportId, owner: ElementId) {
    bind(engine, viewport, owner, 30.0, 55.0, 3.0);
}

fn pointer_with_modifiers(
    engine: &mut Engine,
    viewport: ViewportId,
    event_type: PointerEventType,
    point: Point<f64>,
    modifiers: Modifiers,
) {
    engine
        .process_input_with_viewport_changes(
            viewport,
            InputEvent::Pointer(PointerEvent {
                pressure: None,
                timestamp_ms: 0,
                pointer_id: 1,
                event_type,
                device: PointerDevice::Mouse,
                position: Point::new(point.x + 400.0, point.y + 300.0),
                button: Some(PointerButton::Primary),
                buttons: PointerButtons(PointerButtons::PRIMARY),
                modifiers,
            }),
        )
        .unwrap();
}

fn corner_handles(engine: &mut Engine, viewport: ViewportId) -> Vec<Point<f64>> {
    engine
        .acquire_patch(viewport, None)
        .unwrap()
        .overlay
        .ops
        .iter()
        .flat_map(|op| &op.insert_items)
        .filter_map(|item| match item {
            OverlayDisplayItem::Rectangle(rect)
                if rect.kind == UiShapeKind::SelectionCornerRadiusHandle =>
            {
                Some(Point::new(rect.center_x, rect.center_y))
            }
            _ => None,
        })
        .collect()
}

fn assert_resolved_selection(
    engine: &Engine,
    viewport: ViewportId,
    owner: ElementId,
) -> SpeechBubbleData {
    let state = presentation(engine, viewport);
    let bubble = state
        .speech_bubble_previews
        .iter()
        .find(|preview| preview.0 == owner)
        .map_or_else(|| body(engine, owner), |preview| preview.1.clone());
    let rect = speech_bubble_body_proxy(&bubble);
    assert_eq!(
        state.selected_single_rect,
        Some(rect),
        "corner controls must use the rendered bubble geometry"
    );
    assert_eq!(state.selection_elements[0].rect, rect);
    assert_bounds_close(
        state.selection_bounds.unwrap(),
        rect.center,
        rect.width,
        rect.height,
    );
    bubble
}

fn measure(engine: &mut Engine, viewport: ViewportId, owner: ElementId, height: f64) {
    let request = engine
        .speech_bubble_text_layout_requests(viewport)
        .unwrap()
        .into_iter()
        .find(|request| request.owner_id == owner)
        .unwrap();
    let constraints = speech_bubble_text_constraints(request.body, 55.0, 3.0, height).unwrap();
    engine
        .apply_speech_bubble_text_layouts(
            viewport,
            &[(
                request.text_id,
                SpeechBubbleTextMeasurement {
                    key: request.key,
                    size: TextLayoutSize::with_content(
                        constraints.content_width,
                        height,
                        constraints.content_width - 1.0,
                        height,
                    ),
                    minimum_grapheme_width: 55.0,
                    text_ink_reserve: 3.0,
                },
            )],
        )
        .unwrap();
}

#[test]
fn speech_bubble_clamped_resize_keeps_corner_controls_on_resolved_body() {
    for shape in [
        HighlightShape::Rectangle,
        HighlightShape::Ellipse,
        HighlightShape::Diamond,
    ] {
        for rotation in [0.0, 0.57] {
            for modifiers in [
                Modifiers::default(),
                Modifiers {
                    alt: true,
                    ..Default::default()
                },
                Modifiers {
                    shift: true,
                    ..Default::default()
                },
            ] {
                let (mut engine, viewport, owner) = create(shape, 160.0, 90.0);
                bind_text(&mut engine, viewport, owner);
                let mut original = body(&engine, owner);
                original.body.rotation = rotation;
                let mut tx = Transaction::new("rotated bubble resize fixture");
                tx.update_speech_bubble(owner, original.clone());
                engine
                    .apply_editor_command(
                        viewport,
                        EditorCommand::ApplyTransaction(ApplyTransactionCommand {
                            transaction: tx,
                            history_undo_snapshot: None,
                        }),
                    )
                    .unwrap();
                measure(&mut engine, viewport, owner, 30.0);
                let original = body(&engine, owner);
                let from = handle(&mut engine, viewport, UiShapeKind::SelectionResizeHandle);
                pointer(&mut engine, viewport, PointerEventType::Down, from);
                let anchor = if modifiers.alt {
                    Point::new(0.0, 0.0)
                } else {
                    Point::new(original.body.width / 2.0, original.body.height / 2.0)
                };
                let target = |distance| {
                    speech_bubble_local_to_canvas(
                        &original,
                        Point::new(anchor.x - distance, anchor.y - distance),
                    )
                };
                pointer_with_modifiers(
                    &mut engine,
                    viewport,
                    PointerEventType::Move,
                    target(12.0),
                    modifiers,
                );
                let clamped = assert_resolved_selection(&engine, viewport, owner);
                let handles = corner_handles(&mut engine, viewport);
                assert_eq!(
                    handles.len(),
                    if shape == HighlightShape::Ellipse {
                        0
                    } else {
                        4
                    }
                );
                pointer_with_modifiers(
                    &mut engine,
                    viewport,
                    PointerEventType::Move,
                    target(5.0),
                    modifiers,
                );
                assert_eq!(assert_resolved_selection(&engine, viewport, owner), clamped);
                assert_eq!(corner_handles(&mut engine, viewport), handles);
                assert_eq!(body(&engine, owner), original, "drag must remain a preview");
                pointer_with_modifiers(
                    &mut engine,
                    viewport,
                    PointerEventType::Up,
                    target(5.0),
                    modifiers,
                );
                assert_eq!(body(&engine, owner), clamped);
                assert_resolved_selection(&engine, viewport, owner);
                assert_eq!(corner_handles(&mut engine, viewport), handles);
                engine.undo_with_viewport_changes().unwrap();
                assert_eq!(body(&engine, owner), original);
                assert_resolved_selection(&engine, viewport, owner);
                engine.redo_with_viewport_changes().unwrap();
                assert_eq!(body(&engine, owner), clamped);
                assert_resolved_selection(&engine, viewport, owner);
            }
        }
    }
}

#[test]
fn speech_bubble_resize_corner_controls_follow_remeasured_text_growth_and_cancel() {
    let (mut engine, viewport, owner) = create(HighlightShape::Rectangle, 160.0, 90.0);
    bind_text(&mut engine, viewport, owner);
    measure(&mut engine, viewport, owner, 30.0);
    let original = body(&engine, owner);
    let from = handle(&mut engine, viewport, UiShapeKind::SelectionResizeHandle);
    let to = speech_bubble_local_to_canvas(
        &original,
        Point::new(
            original.body.width / 2.0 - 5.0,
            original.body.height / 2.0 - 5.0,
        ),
    );
    pointer(&mut engine, viewport, PointerEventType::Down, from);
    pointer(&mut engine, viewport, PointerEventType::Move, to);
    let before = assert_resolved_selection(&engine, viewport, owner);
    measure(&mut engine, viewport, owner, 120.0);
    let grown = assert_resolved_selection(&engine, viewport, owner);
    assert!(grown.body.height > before.body.height);
    assert_eq!(body(&engine, owner), original);
    pointer(&mut engine, viewport, PointerEventType::Cancel, to);
    assert_eq!(body(&engine, owner), original);
    assert_resolved_selection(&engine, viewport, owner);
}
