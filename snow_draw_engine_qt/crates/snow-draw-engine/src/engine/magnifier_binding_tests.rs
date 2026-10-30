use super::*;
use snow_draw_engine_core::{
    CornerRadii,
    arrow::{ArrowType, BindMode, EngineContext, StrokeStyle},
};
use snow_draw_engine_display::{OverlayDisplayItem, UiShapeKind};
use snow_draw_engine_document::{
    ArrowData, ArrowEndpointBinding, BindableElementState, DEFAULT_ARROW_MAX_COORDINATE,
    MagnifierData, Transaction, recompute_arrow_after_bindable_change,
};
use snow_draw_engine_editor::magnifier_move_grip;
use snow_draw_engine_interaction::{
    Modifiers, PointerButton, PointerButtons, PointerDevice, PointerEvent, PointerEventType,
};

struct Fixture {
    engine: Engine,
    viewport: ViewportId,
    magnifier_id: ElementId,
    companion_ids: [ElementId; 2],
    arrow_id: ElementId,
    original_magnifier: MagnifierData,
    original_arrow: ArrowData,
}

fn binding_context() -> EngineContext {
    EngineContext {
        zoom: 1.0,
        is_binding_enabled: true,
        bind_mode: BindMode::Orbit,
        max_coordinate: DEFAULT_ARROW_MAX_COORDINATE,
    }
}

fn fixture(mode: BindMode, group: bool) -> Fixture {
    let mut engine = Engine::default();
    let viewport = engine.create_viewport(ViewportConfig::default()).unwrap();
    engine
        .set_viewport_surface_size(viewport, 800, 600)
        .unwrap();
    engine
        .set_viewport_active_tool(viewport, ActiveTool::Select)
        .unwrap();
    let magnifier_id = engine.model.allocate_element_id();
    let companion_ids = [
        engine.model.allocate_element_id(),
        engine.model.allocate_element_id(),
    ];
    let arrow_id = engine.model.allocate_element_id();
    let original_magnifier = MagnifierData {
        source_center: Point::new(0.0, 0.0),
        magnified_center: Point::new(130.0, 70.0),
        width: 80.0,
        height: 60.0,
        rotation: 0.4,
        corner_radii: CornerRadii {
            top_left: 4.0,
            top_right: 8.0,
            bottom_right: 12.0,
            bottom_left: 16.0,
        },
        ..Default::default()
    };
    let other_endpoint = Point::new(-200.0, -150.0);
    let points = if mode == BindMode::Inside {
        [original_magnifier.source_center, other_endpoint]
    } else {
        [other_endpoint, original_magnifier.source_center]
    };
    let mut original_arrow = ArrowData::from_global_points(
        &points,
        original_magnifier.stroke,
        2.0,
        StrokeStyle::Solid,
        ArrowType::Straight,
        None,
        None,
    )
    .unwrap();
    let binding = Some(ArrowEndpointBinding {
        element_id: magnifier_id,
        fixed_point: [0.5, 0.5],
        mode,
    });
    if mode == BindMode::Inside {
        original_arrow.start_binding = binding;
    } else {
        original_arrow.end_binding = binding;
    }
    // Establish the original endpoint with the existing source binding contract.
    original_arrow = recompute_arrow_after_bindable_change(
        arrow_id,
        &original_arrow,
        &[BindableElementState::rectangle_proxy(
            magnifier_id,
            original_magnifier.source_rect(),
            0,
            false,
        )],
        &[magnifier_id],
        binding_context(),
    )
    .arrow;
    let mut transaction = Transaction::new("magnifier source binding fixture");
    transaction.insert_magnifier(magnifier_id, Default::default(), original_magnifier);
    for (id, center) in companion_ids
        .into_iter()
        .zip([Point::new(300.0, 180.0), Point::new(-240.0, -180.0)])
    {
        let mut rectangle = original_magnifier.source_rect();
        rectangle.center = center;
        rectangle.width = 40.0;
        rectangle.height = 30.0;
        rectangle.rotation = 0.0;
        transaction.insert_rectangle(id, Default::default(), rectangle);
    }
    transaction.insert_arrow(arrow_id, Default::default(), original_arrow.clone());
    engine
        .apply_editor_command(
            viewport,
            EditorCommand::ApplyTransaction(ApplyTransactionCommand {
                transaction,
                history_undo_snapshot: None,
            }),
        )
        .unwrap();
    let mut fixture = Fixture {
        engine,
        viewport,
        magnifier_id,
        companion_ids,
        arrow_id,
        original_magnifier,
        original_arrow,
    };
    fixture.select(if group {
        vec![magnifier_id, companion_ids[0]]
    } else {
        vec![magnifier_id]
    });
    fixture
}

impl Fixture {
    fn select(&mut self, ids: Vec<ElementId>) {
        let snapshot = serde_json::from_value(serde_json::json!({
            "selectedIds": ids, "primaryId": self.magnifier_id,
        }))
        .unwrap();
        self.engine
            .editor
            .restore_history_selection(&self.engine.model, &snapshot);
        self.engine.refresh_all_viewports().unwrap();
        assert!(!self.engine.selected_ids().contains(&self.arrow_id));
    }

    fn pointer(&mut self, kind: PointerEventType, point: Point<f64>, from_center: bool) {
        self.engine
            .process_input(
                self.viewport,
                InputEvent::Pointer(PointerEvent {
                    pressure: None,
                    timestamp_ms: 0,
                    pointer_id: 1,
                    event_type: kind,
                    device: PointerDevice::Mouse,
                    position: Point::new(point.x + 400.0, point.y + 300.0),
                    button: Some(PointerButton::Primary),
                    buttons: PointerButtons(PointerButtons::PRIMARY),
                    modifiers: Modifiers {
                        ctrl: true,
                        alt: from_center,
                        ..Default::default()
                    },
                }),
            )
            .unwrap();
    }

    fn expected_arrow(&self, magnifier: MagnifierData) -> ArrowData {
        // Only the source rectangle is a binding target. The selection's compound
        // bounds and the destination lens must never enter endpoint recomputation.
        let bindables = self.engine.model.bindable_element_states_with_overrides(&[(
            self.magnifier_id,
            magnifier.source_rect(),
        )]);
        recompute_arrow_after_bindable_change(
            self.arrow_id,
            &self.original_arrow,
            &bindables,
            &[self.magnifier_id],
            binding_context(),
        )
        .arrow
    }

    fn assert_arrow(&self, arrow: &ArrowData, magnifier: MagnifierData) {
        let expected = self.expected_arrow(magnifier);
        assert_eq!(arrow.start_binding, expected.start_binding);
        assert_eq!(arrow.end_binding, expected.end_binding);
        assert_eq!(arrow.global_points().len(), expected.global_points().len());
        for (actual, expected) in arrow.global_points().iter().zip(expected.global_points()) {
            assert_point(*actual, expected);
        }
        if arrow
            .start_binding
            .as_ref()
            .is_some_and(|binding| binding.mode == BindMode::Inside)
        {
            // The binding engine offsets exact 0.5 ratios to 0.5001 to avoid
            // singular center rays; that offset must use the source dimensions.
            let (sin, cos) = magnifier.rotation.sin_cos();
            let x = magnifier.width * 0.0001;
            let y = magnifier.height * 0.0001;
            assert_point(
                arrow.global_points()[0],
                Point::new(
                    magnifier.source_center.x + x * cos - y * sin,
                    magnifier.source_center.y + x * sin + y * cos,
                ),
            );
        }
    }

    fn assert_preview_binding(&self) -> MagnifierData {
        let presentation = self.engine.editor.presentation_state(
            &self.engine.model,
            &self.engine.viewport_slot(self.viewport).unwrap().view,
        );
        let magnifier = presentation
            .preview_magnifiers
            .iter()
            .find(|preview| preview.id == self.magnifier_id)
            .unwrap()
            .magnifier;
        let arrow = presentation
            .preview_arrows
            .iter()
            .find(|preview| preview.id == self.arrow_id)
            .map(|preview| &preview.arrow)
            .unwrap_or_else(|| self.engine.model.arrow(self.arrow_id).unwrap());
        self.assert_arrow(arrow, magnifier);
        assert_eq!(
            *self.engine.model.arrow(self.arrow_id).unwrap(),
            self.original_arrow
        );
        assert_eq!(
            *self.engine.model.magnifier(self.magnifier_id).unwrap(),
            self.original_magnifier
        );
        magnifier
    }

    fn assert_commit_and_history(&mut self, preview: MagnifierData) {
        let committed = *self.engine.model.magnifier(self.magnifier_id).unwrap();
        assert_eq!(committed, preview);
        let arrow = self.engine.model.arrow(self.arrow_id).unwrap().clone();
        self.assert_arrow(&arrow, committed);
        assert!(self.engine.undo().unwrap());
        assert_eq!(
            *self.engine.model.magnifier(self.magnifier_id).unwrap(),
            self.original_magnifier
        );
        assert_eq!(
            *self.engine.model.arrow(self.arrow_id).unwrap(),
            self.original_arrow
        );
        assert!(self.engine.redo().unwrap());
        assert_eq!(
            *self.engine.model.magnifier(self.magnifier_id).unwrap(),
            committed
        );
        assert_eq!(*self.engine.model.arrow(self.arrow_id).unwrap(), arrow);
    }

    fn bounds(&self) -> snow_draw_engine_editor::SelectionBounds {
        self.engine
            .editor
            .presentation_state(
                &self.engine.model,
                &self.engine.viewport_slot(self.viewport).unwrap().view,
            )
            .selection_bounds
            .unwrap()
    }

    fn handle(&self, kind: UiShapeKind) -> Point<f64> {
        let bounds = self.bounds();
        let (sin, cos) = bounds.rotation.sin_cos();
        self.engine
            .acquire_patch(self.viewport, None)
            .unwrap()
            .overlay
            .ops
            .iter()
            .flat_map(|op| &op.insert_items)
            .filter_map(|item| match item {
                OverlayDisplayItem::Rectangle(item) if item.kind == kind => {
                    Some(Point::new(item.center_x, item.center_y))
                }
                _ => None,
            })
            .max_by(|first, second| {
                let rank = |point: &Point<f64>| {
                    let dx = point.x - bounds.center.x;
                    let dy = point.y - bounds.center.y;
                    dx * cos + dy * sin - dx * sin + dy * cos
                };
                rank(first).total_cmp(&rank(second))
            })
            .unwrap()
    }
}

fn assert_point(actual: Point<f64>, expected: Point<f64>) {
    assert!(
        (actual.x - expected.x).abs() < 1e-8 && (actual.y - expected.y).abs() < 1e-8,
        "actual {actual:?}, expected source-bound point {expected:?}"
    );
}

#[test]
fn magnifier_binding_source_and_group_move_preview_commit_and_history() {
    for mode in [BindMode::Inside, BindMode::Orbit] {
        for group in [true, false] {
            let mut fixture = fixture(mode, group);
            let from = Point::new(0.0, 20.0);
            let to = Point::new(20.0, 40.0);
            fixture.pointer(PointerEventType::Down, from, false);
            fixture.pointer(PointerEventType::Move, to, false);
            let preview = fixture.assert_preview_binding();
            assert_point(preview.source_center, Point::new(20.0, 20.0));
            fixture.pointer(PointerEventType::Up, to, false);
            fixture.assert_commit_and_history(preview);
        }
    }
}

#[test]
fn magnifier_binding_source_and_group_resize_including_centered_reflection() {
    for mode in [BindMode::Inside, BindMode::Orbit] {
        for group in [true, false] {
            for (sx, sy, from_center) in [(1.3, 1.4, false), (-1.0, -1.0, true)] {
                let mut fixture = fixture(mode, group);
                let bounds = fixture.bounds();
                let from = fixture.handle(UiShapeKind::SelectionResizeHandle);
                let (sin, cos) = bounds.rotation.sin_cos();
                let dx = from.x - bounds.center.x;
                let dy = from.y - bounds.center.y;
                let local_from = Point::new(dx * cos + dy * sin, -dx * sin + dy * cos);
                let anchor = if from_center {
                    Point::default()
                } else {
                    Point::new(-bounds.width / 2.0, -bounds.height / 2.0)
                };
                let half = if from_center { 0.5 } else { 1.0 };
                let local_to = Point::new(
                    anchor.x + bounds.width * half * sx + local_from.x - bounds.width / 2.0,
                    anchor.y + bounds.height * half * sy + local_from.y - bounds.height / 2.0,
                );
                let to = Point::new(
                    bounds.center.x + local_to.x * cos - local_to.y * sin,
                    bounds.center.y + local_to.x * sin + local_to.y * cos,
                );
                fixture.pointer(PointerEventType::Down, from, from_center);
                fixture.pointer(PointerEventType::Move, to, from_center);
                let preview = fixture.assert_preview_binding();
                fixture.pointer(PointerEventType::Up, to, from_center);
                fixture.assert_commit_and_history(preview);
            }
        }
    }
}

#[test]
fn magnifier_binding_source_and_group_rotation_preview_commit_and_history() {
    for mode in [BindMode::Inside, BindMode::Orbit] {
        for group in [true, false] {
            let mut fixture = fixture(mode, group);
            let center = fixture.bounds().center;
            let from = fixture.handle(UiShapeKind::SelectionRotationHandle);
            let to = Point::new(center.x - (from.y - center.y), center.y + from.x - center.x);
            fixture.pointer(PointerEventType::Down, from, false);
            fixture.pointer(PointerEventType::Move, to, false);
            let preview = fixture.assert_preview_binding();
            assert!(
                (preview.rotation
                    - fixture.original_magnifier.rotation
                    - std::f64::consts::FRAC_PI_2)
                    .abs()
                    < 1e-8
            );
            fixture.pointer(PointerEventType::Up, to, false);
            fixture.assert_commit_and_history(preview);
        }
    }
}

#[test]
fn magnifier_binding_alignment_and_distribution_use_source_geometry_with_history() {
    for mode in [BindMode::Inside, BindMode::Orbit] {
        for alignment in 0..=7 {
            let mut fixture = fixture(mode, true);
            if alignment >= 6 {
                fixture.select(vec![
                    fixture.magnifier_id,
                    fixture.companion_ids[0],
                    fixture.companion_ids[1],
                ]);
            }
            fixture
                .engine
                .align_selected_with_viewport_changes(fixture.viewport, alignment)
                .unwrap();
            let committed = *fixture
                .engine
                .model
                .magnifier(fixture.magnifier_id)
                .unwrap();
            fixture.assert_commit_and_history(committed);
        }
    }
}

#[test]
fn magnifier_binding_lens_only_move_preserves_source_and_attached_arrow() {
    for mode in [BindMode::Inside, BindMode::Orbit] {
        for group in [true, false] {
            let mut fixture = fixture(mode, group);
            let from = magnifier_move_grip(fixture.original_magnifier, 1.0);
            let to = Point::new(from.x + 25.0, from.y + 15.0);
            fixture.pointer(PointerEventType::Down, from, false);
            fixture.pointer(PointerEventType::Move, to, false);
            let presentation = fixture.engine.editor.presentation_state(
                &fixture.engine.model,
                &fixture.engine.viewport_slot(fixture.viewport).unwrap().view,
            );
            let preview = presentation
                .preview_magnifiers
                .iter()
                .find(|preview| preview.id == fixture.magnifier_id)
                .unwrap()
                .magnifier;
            assert_eq!(
                preview.source_rect(),
                fixture.original_magnifier.source_rect()
            );
            assert!(
                presentation
                    .preview_arrows
                    .iter()
                    .all(|arrow| arrow.id != fixture.arrow_id)
            );
            fixture.pointer(PointerEventType::Up, to, false);
            assert_eq!(
                *fixture.engine.model.arrow(fixture.arrow_id).unwrap(),
                fixture.original_arrow
            );
            fixture.assert_commit_and_history(preview);
        }
    }
}
