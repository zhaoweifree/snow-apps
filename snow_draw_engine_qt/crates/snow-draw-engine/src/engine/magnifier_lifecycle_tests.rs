use super::*;
use snow_draw_engine_display::SceneDisplayItem;
use snow_draw_engine_document::{ElementMeta, MagnifierData, RectangleData, Transaction};
use snow_draw_engine_interaction::{
    Modifiers, PointerButton, PointerButtons, PointerCaptureCommand, PointerDevice, PointerEvent,
    PointerEventType,
};

fn pointer(
    engine: &mut Engine,
    viewport: ViewportId,
    event_type: PointerEventType,
    position: Point<f64>,
) -> InteractionOutput {
    engine
        .process_input(
            viewport,
            InputEvent::Pointer(PointerEvent {
                pressure: None,
                timestamp_ms: 0,
                pointer_id: 1,
                event_type,
                device: PointerDevice::Mouse,
                position,
                button: Some(PointerButton::Primary),
                buttons: PointerButtons(PointerButtons::PRIMARY),
                modifiers: Modifiers::default(),
            }),
        )
        .unwrap()
}

fn fixture() -> (Engine, ViewportId, ElementId) {
    let mut engine = Engine::default();
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
        Point::new(350.0, 260.0),
    );
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Up,
        Point::new(450.0, 340.0),
    );
    let id = engine.model.paint_order()[0];
    engine
        .set_viewport_active_tool(viewport, ActiveTool::Select)
        .unwrap();
    engine
        .select_element_with_viewport_changes(viewport, id)
        .unwrap();
    (engine, viewport, id)
}

fn preview_magnifiers(engine: &Engine, viewport: ViewportId) -> Vec<MagnifierData> {
    engine
        .editor
        .presentation_state(&engine.model, &engine.viewport_slot(viewport).unwrap().view)
        .preview_magnifiers
        .into_iter()
        .map(|value| value.magnifier)
        .collect()
}

fn scene_magnifier_count(engine: &Engine, viewport: ViewportId) -> usize {
    engine
        .acquire_patch(viewport, None)
        .unwrap()
        .scene
        .ops
        .iter()
        .flat_map(|op| &op.insert_items)
        .filter(|item| matches!(item, SceneDisplayItem::Magnifier(_)))
        .count()
}

#[test]
fn magnifier_lifecycle_target_removal_cancels_drag_preview_and_completion() {
    for removal in [
        "undo creation",
        "transaction",
        "delete",
        "delete all",
        "clear document",
    ] {
        for completion in [PointerEventType::Up, PointerEventType::Cancel] {
            let (mut engine, viewport, id) = fixture();
            assert_eq!(
                pointer(
                    &mut engine,
                    viewport,
                    PointerEventType::Down,
                    Point::new(475.0, 300.0),
                )
                .capture,
                PointerCaptureCommand::Capture(1)
            );
            pointer(
                &mut engine,
                viewport,
                PointerEventType::Move,
                Point::new(495.0, 310.0),
            );
            assert!(engine.editor.has_pointer_capture());
            assert_eq!(preview_magnifiers(&engine, viewport).len(), 1);
            match removal {
                "undo creation" => assert!(engine.undo().unwrap()),
                "transaction" => {
                    let mut transaction = Transaction::new("remove active magnifier");
                    transaction.remove_element(id);
                    engine
                        .apply_editor_command(
                            viewport,
                            EditorCommand::ApplyTransaction(ApplyTransactionCommand {
                                transaction,
                                history_undo_snapshot: None,
                            }),
                        )
                        .unwrap();
                }
                "delete" => {
                    engine
                        .delete_selected_with_viewport_changes(viewport)
                        .unwrap();
                }
                "delete all" => {
                    engine
                        .delete_all_elements_with_viewport_changes(viewport)
                        .unwrap();
                }
                "clear document" => {
                    engine.clear_document_preserving_viewports().unwrap();
                }
                _ => unreachable!(),
            }
            assert!(engine.model.paint_order().is_empty(), "{removal}");
            assert!(!engine.editor.has_pointer_capture(), "{removal}");
            assert!(
                preview_magnifiers(&engine, viewport).is_empty(),
                "{removal}"
            );
            assert_eq!(scene_magnifier_count(&engine, viewport), 0, "{removal}");
            assert_eq!(
                pointer(&mut engine, viewport, completion, Point::new(505.0, 310.0),).capture,
                PointerCaptureCommand::Release,
                "{removal}: {completion:?} must release the cancelled gesture"
            );
            assert!(engine.model.paint_order().is_empty(), "{removal}");
            assert_eq!(scene_magnifier_count(&engine, viewport), 0, "{removal}");
            if removal == "undo creation" {
                assert!(engine.redo().unwrap());
                assert_eq!(engine.model.paint_order(), &[id]);
                assert_eq!(
                    engine.model.magnifier(id).unwrap().magnified_center,
                    Point::default()
                );
            }
        }
    }
}

#[test]
fn magnifier_lifecycle_source_removal_cancels_pending_and_active_group_drag() {
    for moved in [false, true] {
        for grouped in [false, true] {
            let (mut engine, viewport, id) = fixture();
            let rectangle = RectangleData {
                center: Point::new(180.0, 0.0),
                width: 30.0,
                height: 30.0,
                ..engine.model.magnifier(id).unwrap().source_rect()
            };
            let companion = grouped.then(|| {
                let companion = engine.model.peek_next_element_id();
                let mut transaction = Transaction::new("group companion");
                transaction.insert_rectangle(companion, ElementMeta::default(), rectangle);
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
                pointer(
                    &mut engine,
                    viewport,
                    PointerEventType::Down,
                    Point::new(280.0, 200.0),
                );
                pointer(
                    &mut engine,
                    viewport,
                    PointerEventType::Up,
                    Point::new(620.0, 400.0),
                );
                assert_eq!(engine.selected_ids(), vec![id, companion]);
                companion
            });
            assert_eq!(
                pointer(
                    &mut engine,
                    viewport,
                    PointerEventType::Down,
                    Point::new(400.0, 300.0),
                )
                .capture,
                PointerCaptureCommand::Capture(1)
            );
            if let Some(companion) = companion {
                assert_eq!(engine.selected_ids(), vec![id, companion]);
            }
            if moved {
                pointer(
                    &mut engine,
                    viewport,
                    PointerEventType::Move,
                    Point::new(425.0, 310.0),
                );
                assert_eq!(preview_magnifiers(&engine, viewport).len(), 1);
            }
            let mut transaction = Transaction::new("remove dragged source");
            transaction.remove_element(id);
            engine
                .apply_editor_command(
                    viewport,
                    EditorCommand::ApplyTransaction(ApplyTransactionCommand {
                        transaction,
                        history_undo_snapshot: None,
                    }),
                )
                .unwrap();
            assert!(preview_magnifiers(&engine, viewport).is_empty());
            assert!(!engine.editor.has_pointer_capture());
            assert_eq!(scene_magnifier_count(&engine, viewport), 0);
            assert_eq!(
                pointer(
                    &mut engine,
                    viewport,
                    PointerEventType::Up,
                    Point::new(425.0, 310.0),
                )
                .capture,
                PointerCaptureCommand::Release
            );
            if let Some(companion) = companion {
                assert_eq!(engine.model.paint_order(), &[companion]);
                assert_eq!(*engine.model.rectangle(companion).unwrap(), rectangle);
            } else {
                assert!(engine.model.paint_order().is_empty());
            }
        }
    }
}

#[test]
fn magnifier_lifecycle_editor_reset_discards_geometry_and_releases_capture() {
    let (mut engine, viewport, id) = fixture();
    let original = *engine.model.magnifier(id).unwrap();
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Down,
        Point::new(475.0, 300.0),
    );
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Move,
        Point::new(495.0, 310.0),
    );
    engine
        .reset_editing_state_with_viewport_changes(viewport)
        .unwrap();
    assert!(!engine.editor.has_pointer_capture());
    assert!(preview_magnifiers(&engine, viewport).is_empty());
    assert_eq!(
        pointer(
            &mut engine,
            viewport,
            PointerEventType::Up,
            Point::new(505.0, 310.0),
        )
        .capture,
        PointerCaptureCommand::Release
    );
    assert_eq!(*engine.model.magnifier(id).unwrap(), original);
}

#[test]
fn magnifier_lifecycle_cancelled_capture_does_not_override_a_new_gesture() {
    let (mut engine, viewport, id) = fixture();
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Down,
        Point::new(475.0, 300.0),
    );
    engine
        .reset_editing_state_with_viewport_changes(viewport)
        .unwrap();
    engine
        .select_element_with_viewport_changes(viewport, id)
        .unwrap();
    assert_eq!(
        pointer(
            &mut engine,
            viewport,
            PointerEventType::Down,
            Point::new(475.0, 300.0),
        )
        .capture,
        PointerCaptureCommand::Capture(1)
    );
    assert!(engine.editor.has_pointer_capture());
    assert_eq!(
        pointer(
            &mut engine,
            viewport,
            PointerEventType::Cancel,
            Point::new(475.0, 300.0),
        )
        .capture,
        PointerCaptureCommand::Release
    );
    assert!(!engine.editor.has_pointer_capture());
    assert_eq!(
        pointer(
            &mut engine,
            viewport,
            PointerEventType::Up,
            Point::new(475.0, 300.0),
        )
        .capture,
        PointerCaptureCommand::NoChange
    );
}

#[test]
fn magnifier_lifecycle_capture_is_runtime_only_and_respects_configuration() {
    let (mut engine, viewport, _) = fixture();
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Down,
        Point::new(475.0, 300.0),
    );
    assert!(engine.editor.has_pointer_capture());
    let clone = engine.clone_document_session();
    assert!(!clone.editor.has_pointer_capture());
    let restored = Engine::from_serialized_document_session_with_config(
        &engine.serialize_document_session().unwrap(),
        EngineConfig::default(),
    )
    .unwrap();
    assert!(!restored.editor.has_pointer_capture());

    let mut disabled = Engine::default();
    let viewport = disabled
        .create_viewport(ViewportConfig {
            engine: snow_draw_engine_core::EngineConfig {
                enable_pointer_capture: false,
                ..Default::default()
            },
        })
        .unwrap();
    disabled
        .set_viewport_surface_size(viewport, 800, 600)
        .unwrap();
    assert_eq!(
        pointer(
            &mut disabled,
            viewport,
            PointerEventType::Down,
            Point::new(350.0, 260.0),
        )
        .capture,
        PointerCaptureCommand::NoChange
    );
    assert!(!disabled.editor.has_pointer_capture());
    disabled
        .reset_editing_state_with_viewport_changes(viewport)
        .unwrap();
    assert_eq!(
        pointer(
            &mut disabled,
            viewport,
            PointerEventType::Cancel,
            Point::new(350.0, 260.0),
        )
        .capture,
        PointerCaptureCommand::NoChange
    );
}

#[test]
fn magnifier_lifecycle_cancelled_capture_release_survives_batched_moves() {
    let (mut engine, viewport, _) = fixture();
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Down,
        Point::new(475.0, 300.0),
    );
    assert!(engine.undo().unwrap());
    let events = [495.0, 505.0].map(|x| {
        InputEvent::Pointer(PointerEvent {
            pressure: None,
            timestamp_ms: 0,
            pointer_id: 1,
            event_type: PointerEventType::Move,
            device: PointerDevice::Mouse,
            position: Point::new(x, 300.0),
            button: None,
            buttons: PointerButtons(PointerButtons::PRIMARY),
            modifiers: Modifiers::default(),
        })
    });
    let output = engine
        .process_pointer_move_batch_with_viewport_changes(viewport, &events)
        .unwrap();
    assert_eq!(output.interaction.capture, PointerCaptureCommand::Release);
    assert!(!engine.editor.has_pointer_capture());
    assert_eq!(scene_magnifier_count(&engine, viewport), 0);
}

#[test]
fn magnifier_lifecycle_cancelled_capture_release_survives_a_disabled_viewport() {
    let (mut engine, viewport, _) = fixture();
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Down,
        Point::new(475.0, 300.0),
    );
    assert!(engine.undo().unwrap());
    let disabled = engine
        .create_viewport(ViewportConfig {
            engine: snow_draw_engine_core::EngineConfig {
                enable_pointer_capture: false,
                ..Default::default()
            },
        })
        .unwrap();
    assert_eq!(
        pointer(
            &mut engine,
            disabled,
            PointerEventType::Cancel,
            Point::new(475.0, 300.0),
        )
        .capture,
        PointerCaptureCommand::Release
    );
    assert!(!engine.editor.has_pointer_capture());
}

#[test]
fn magnifier_lifecycle_shared_target_validation_cancels_removed_arrow_drags() {
    for moved in [false, true] {
        let mut engine = Engine::default();
        let viewport = engine.create_viewport(ViewportConfig::default()).unwrap();
        engine
            .set_viewport_surface_size(viewport, 800, 600)
            .unwrap();
        engine
            .set_viewport_active_tool(viewport, ActiveTool::Arrow)
            .unwrap();
        for (event_type, position) in [
            (PointerEventType::Down, Point::new(350.0, 150.0)),
            (PointerEventType::Move, Point::new(450.0, 150.0)),
            (PointerEventType::Up, Point::new(450.0, 150.0)),
        ] {
            pointer(&mut engine, viewport, event_type, position);
        }
        let id = engine.model.paint_order()[0];
        engine
            .set_viewport_active_tool(viewport, ActiveTool::Select)
            .unwrap();
        engine
            .select_element_with_viewport_changes(viewport, id)
            .unwrap();
        assert_eq!(
            pointer(
                &mut engine,
                viewport,
                PointerEventType::Down,
                Point::new(400.0, 150.0),
            )
            .capture,
            PointerCaptureCommand::Capture(1)
        );
        if moved {
            pointer(
                &mut engine,
                viewport,
                PointerEventType::Move,
                Point::new(425.0, 160.0),
            );
        }
        let mut transaction = Transaction::new("remove dragged arrow");
        transaction.remove_element(id);
        engine
            .apply_editor_command(
                viewport,
                EditorCommand::ApplyTransaction(ApplyTransactionCommand {
                    transaction,
                    history_undo_snapshot: None,
                }),
            )
            .unwrap();
        assert!(!engine.editor.has_pointer_capture());
        let presentation = engine
            .editor
            .presentation_state(&engine.model, &engine.viewport_slot(viewport).unwrap().view);
        assert!(presentation.preview_arrows.is_empty());
        assert_eq!(
            pointer(
                &mut engine,
                viewport,
                PointerEventType::Up,
                Point::new(425.0, 160.0),
            )
            .capture,
            PointerCaptureCommand::Release
        );
        assert!(engine.model.paint_order().is_empty());
    }
}
