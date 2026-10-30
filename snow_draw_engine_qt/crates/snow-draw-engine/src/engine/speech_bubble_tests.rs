use super::*;
use snow_draw_engine_core::CornerRadii;
use snow_draw_engine_display::{OverlayDisplayItem, UiShapeKind};
use snow_draw_engine_document::{
    ElementData, ElementMeta, HighlightShape, SpeechBubbleData, TextData, TextHorizontalAlign,
    TextLayoutSize, TextVerticalAlign, Transaction, speech_bubble_local_to_canvas,
    speech_bubble_tail_base_points,
};
use snow_draw_engine_editor::{
    ActiveTextDraftPresentation, ActiveTextDraftTarget, TextCommitTarget, TextDraftCommit,
    speech_bubble_text_constraints,
};
use snow_draw_engine_interaction::{
    Modifiers, PointerButton, PointerButtons, PointerDevice, PointerEvent, PointerEventType,
};

#[path = "speech_bubble_layout_origin_tests.rs"]
mod layout_origin_tests;

#[path = "speech_bubble_selection_geometry_tests.rs"]
mod selection_geometry_tests;

#[path = "speech_bubble_hit_tests.rs"]
mod hit_tests;

#[path = "speech_bubble_draft_lifecycle_tests.rs"]
mod draft_lifecycle_tests;

#[path = "speech_bubble_tail_drag_tests.rs"]
mod tail_drag_tests;

fn pointer(
    engine: &mut Engine,
    viewport: ViewportId,
    event_type: PointerEventType,
    point: Point<f64>,
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
                modifiers: Modifiers::default(),
            }),
        )
        .unwrap();
}

fn create(shape: HighlightShape, width: f64, height: f64) -> (Engine, ViewportId, ElementId) {
    let mut config = EngineConfig::default();
    config.style_defaults.editor.speech_bubble.shape = shape;
    config.style_defaults.editor.speech_bubble.corner_radii = CornerRadii::splat(8.0);
    let mut engine = Engine::new(config);
    let viewport = engine.create_viewport(ViewportConfig::default()).unwrap();
    engine
        .set_viewport_surface_size(viewport, 800, 600)
        .unwrap();
    engine
        .set_viewport_active_tool(viewport, ActiveTool::SpeechBubble)
        .unwrap();
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Down,
        Point::new(-width / 2.0, -height / 2.0),
    );
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Move,
        Point::new(width / 2.0, height / 2.0),
    );
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Up,
        Point::new(width / 2.0, height / 2.0),
    );
    let owner = engine.model.paint_order()[0];
    (engine, viewport, owner)
}

fn body(engine: &Engine, owner: ElementId) -> SpeechBubbleData {
    engine.model.speech_bubble(owner).unwrap().clone()
}

fn handle(engine: &mut Engine, viewport: ViewportId, kind: UiShapeKind) -> Point<f64> {
    let patch = engine.acquire_patch(viewport, None).unwrap();
    patch
        .overlay
        .ops
        .iter()
        .flat_map(|op| &op.insert_items)
        .find_map(|item| match item {
            OverlayDisplayItem::Rectangle(rect) if rect.kind == kind => {
                Some(Point::new(rect.center_x, rect.center_y))
            }
            _ => None,
        })
        .unwrap()
}

fn bind(
    engine: &mut Engine,
    viewport: ViewportId,
    owner: ElementId,
    height: f64,
    minimum: f64,
    reserve: f64,
) -> ElementId {
    let (info, style) = engine
        .speech_bubble_text_target(viewport, None)
        .unwrap()
        .unwrap();
    let c =
        speech_bubble_text_constraints(body(engine, owner).body, minimum, reserve, height).unwrap();
    let mut draft = TextDraftCommit::new(
        TextCommitTarget::NewSpeechBubble(owner),
        info.center,
        "speech bubble text",
        TextLayoutSize::with_content(c.content_width, height, c.content_width - 1.0, height),
        style,
        true,
        false,
    );
    draft.minimum_grapheme_width = minimum;
    draft.text_ink_reserve = reserve;
    engine
        .commit_text_draft_with_viewport_changes(viewport, draft)
        .unwrap();
    engine
        .model
        .speech_bubble(owner)
        .unwrap()
        .text_element_id
        .unwrap()
}

#[test]
fn speech_bubble_requested_radii_survive_tiny_creation_style_and_manual_resize() {
    use snow_draw_engine_editor::{
        SHAPE_STYLE_MIXED_CORNER_RADII, ShapeKind, ShapeStyle, ShapeStylePatch,
    };
    for shape in [
        HighlightShape::Rectangle,
        HighlightShape::Ellipse,
        HighlightShape::Diamond,
    ] {
        let (mut tiny, viewport, owner) = create(shape, 1.0, 1.0);
        assert_eq!(
            body(&tiny, owner).body.corner_radii,
            CornerRadii::splat(8.0)
        );
        let mut style = ShapeStyle::from_rectangle_shape_style(
            EngineConfig::default().style_defaults.editor.speech_bubble,
        );
        style.corner_radii = CornerRadii::splat(100.0);
        tiny.set_viewport_shape_style_patch(
            viewport,
            ShapeStylePatch {
                kind: ShapeKind::SpeechBubble,
                style,
                properties: SHAPE_STYLE_MIXED_CORNER_RADII,
            },
        )
        .unwrap();
        assert_eq!(
            body(&tiny, owner).body.corner_radii,
            CornerRadii::splat(100.0)
        );

        let (mut engine, viewport, owner) = create(shape, 160.0, 90.0);
        let before = body(&engine, owner);
        let from = handle(&mut engine, viewport, UiShapeKind::SelectionResizeHandle);
        let to = Point::new(75.0, 40.0);
        pointer(&mut engine, viewport, PointerEventType::Down, from);
        pointer(&mut engine, viewport, PointerEventType::Move, to);
        pointer(&mut engine, viewport, PointerEventType::Up, to);
        let resized = body(&engine, owner);
        assert!(resized.body.width <= 2.0 && resized.body.height <= 2.0);
        assert_eq!(resized.body.corner_radii, before.body.corner_radii);
        snow_draw_engine_document::validate_speech_bubble(&resized).unwrap();
        let saved = engine.serialize_document_session().unwrap();
        let restored =
            Engine::from_serialized_document_session_with_config(&saved, EngineConfig::default())
                .unwrap();
        assert_eq!(body(&restored, owner), resized);
        engine.undo_with_viewport_changes().unwrap();
        assert_eq!(body(&engine, owner), before);
        engine.redo_with_viewport_changes().unwrap();
        assert_eq!(body(&engine, owner), resized);
    }
}

#[test]
fn speech_bubble_template_remaps_bound_text_and_accepts_legacy_three_rectangle() {
    use snow_draw_engine_document::ElementRecord;
    use snow_draw_engine_editor::DrawTemplate;
    let (mut source, viewport, owner) = create(HighlightShape::Diamond, 160.0, 90.0);
    let label = bind(&mut source, viewport, owner, 40.0, 20.0, 3.0);
    let add = handle(
        &mut source,
        viewport,
        UiShapeKind::SpeechBubbleAddTailHandle,
    );
    pointer(&mut source, viewport, PointerEventType::Down, add);
    pointer(
        &mut source,
        viewport,
        PointerEventType::Up,
        Point::new(150.0, 200.0),
    );
    let before = body(&source, owner);
    let template: DrawTemplate =
        serde_json::from_slice(&source.serialize_selected_draw_template().unwrap()).unwrap();
    assert_eq!(template.schema_version, 7);
    assert_eq!(template.elements.len(), 2);
    source
        .insert_draw_template_with_viewport_changes(
            viewport,
            &serde_json::to_vec(&template).unwrap(),
            Point::new(400.0, 200.0),
        )
        .unwrap();
    let inserted = source.selected_ids()[0];
    let copied = body(&source, inserted);
    assert_ne!(inserted, owner);
    assert_eq!(copied.tails, before.tails);
    assert_eq!(copied.body.corner_radii, before.body.corner_radii);
    let copied_label = copied.text_element_id.unwrap();
    assert_ne!(copied_label, label);
    assert_eq!(
        source.model.text(copied_label).unwrap().text,
        source.model.text(label).unwrap().text
    );

    let legacy = DrawTemplate {
        schema_version: 3,
        source_center: before.body.center,
        selected_ids: vec![owner],
        elements: vec![ElementRecord {
            id: owner,
            meta: ElementMeta::default(),
            data: ElementData::Rectangle(before.body),
        }],
    };
    source
        .insert_draw_template_with_viewport_changes(
            viewport,
            &serde_json::to_vec(&legacy).unwrap(),
            Point::new(-200.0, -100.0),
        )
        .unwrap();
    let inserted = source.selected_ids()[0];
    assert_eq!(
        source.model.rectangle(inserted).unwrap().center,
        Point::new(-200.0, -100.0)
    );
}

#[test]
fn speech_bubble_creation_selects_owner_and_requests_edit_for_all_shapes() {
    for shape in [
        HighlightShape::Rectangle,
        HighlightShape::Ellipse,
        HighlightShape::Diamond,
    ] {
        let (mut engine, viewport, owner) = create(shape, 160.0, 80.0);
        assert_eq!(engine.selected_ids(), vec![owner]);
        assert_eq!(body(&engine, owner).body.highlight_shape, shape);
        assert_eq!(
            engine
                .take_speech_bubble_text_edit_request(viewport)
                .unwrap(),
            Some(owner)
        );
        assert_eq!(
            engine
                .take_speech_bubble_text_edit_request(viewport)
                .unwrap(),
            None
        );
        assert_eq!(
            engine
                .speech_bubble_text_target(viewport, None)
                .unwrap()
                .unwrap()
                .0
                .bubble_id,
            Some(owner)
        );
        let add = handle(
            &mut engine,
            viewport,
            UiShapeKind::SpeechBubbleAddTailHandle,
        );
        let rotate = handle(&mut engine, viewport, UiShapeKind::SelectionRotationHandle);
        let center = body(&engine, owner).body.center;
        assert!((add.x + rotate.x - 2.0 * center.x).abs() < 1e-9);
        assert!((add.y + rotate.y - 2.0 * center.y).abs() < 1e-9);
        engine.undo_with_viewport_changes().unwrap();
        assert!(engine.model.paint_order().is_empty());
        engine.redo_with_viewport_changes().unwrap();
        assert!(engine.model.speech_bubble(owner).is_ok());
    }
}

#[test]
fn speech_bubble_new_text_centers_without_changing_other_text_defaults() {
    for shape in [
        HighlightShape::Rectangle,
        HighlightShape::Ellipse,
        HighlightShape::Diamond,
    ] {
        for horizontal_align in [TextHorizontalAlign::Left, TextHorizontalAlign::Right] {
            for vertical_align in [TextVerticalAlign::Top, TextVerticalAlign::Bottom] {
                let (mut engine, viewport, owner) = create(shape, 160.0, 80.0);
                let mut defaults = engine.style_defaults().editor.text.clone();
                defaults.horizontal_align = horizontal_align;
                defaults.vertical_align = vertical_align;
                defaults.font_size = 23.0;
                defaults.bold = true;
                engine
                    .set_viewport_text_creation_style(
                        viewport,
                        defaults.clone(),
                        snow_draw_engine_editor::TEXT_STYLE_ALL_PROPERTIES,
                    )
                    .unwrap();
                let mut expected = defaults.clone();
                expected.horizontal_align = TextHorizontalAlign::Center;
                expected.vertical_align = TextVerticalAlign::Center;
                for point in [None, Some(body(&engine, owner).body.center)] {
                    let (info, style) = engine
                        .speech_bubble_text_target(viewport, point)
                        .unwrap()
                        .unwrap();
                    assert_eq!(info.horizontal_align, TextHorizontalAlign::Center);
                    assert_eq!(info.vertical_align, TextVerticalAlign::Center);
                    assert_eq!(style, expected);
                }
                let label = bind(&mut engine, viewport, owner, 24.0, 12.0, 2.0);
                let text = engine.model.text(label).unwrap();
                assert_eq!(text.horizontal_align, TextHorizontalAlign::Center);
                assert_eq!(text.vertical_align, TextVerticalAlign::Center);
                assert_eq!(
                    engine
                        .viewport_style_toolbar_state(viewport)
                        .unwrap()
                        .text_style,
                    defaults
                );
            }
        }
    }
}

#[test]
fn speech_bubble_existing_text_keeps_explicit_alignment_when_reopened() {
    let (mut engine, viewport, owner) = create(HighlightShape::Rectangle, 160.0, 80.0);
    let label = bind(&mut engine, viewport, owner, 24.0, 12.0, 2.0);
    let mut text = engine.model.text(label).unwrap().clone();
    text.horizontal_align = TextHorizontalAlign::Right;
    text.vertical_align = TextVerticalAlign::Bottom;
    let mut tx = Transaction::new("align bubble text");
    tx.update_text(label, text);
    engine
        .apply_editor_command(
            viewport,
            EditorCommand::ApplyTransaction(ApplyTransactionCommand {
                transaction: tx,
                history_undo_snapshot: None,
            }),
        )
        .unwrap();
    let (info, reopened) = engine
        .speech_bubble_text_target(viewport, None)
        .unwrap()
        .unwrap();
    assert_eq!(info.id, label);
    assert_eq!(info.horizontal_align, TextHorizontalAlign::Right);
    assert_eq!(info.vertical_align, TextVerticalAlign::Bottom);
    assert_eq!(reopened.horizontal_align, TextHorizontalAlign::Right);
    assert_eq!(reopened.vertical_align, TextVerticalAlign::Bottom);
}

#[test]
fn speech_bubble_text_commit_fits_wrap_box_grows_without_shrink_and_restores_history() {
    for shape in [
        HighlightShape::Rectangle,
        HighlightShape::Ellipse,
        HighlightShape::Diamond,
    ] {
        let (mut engine, viewport, owner) = create(shape, 90.0, 40.0);
        let before = body(&engine, owner);
        let label = bind(&mut engine, viewport, owner, 110.0, 45.0, 4.0);
        let after = body(&engine, owner);
        let text = engine.model.text(label).unwrap();
        let c = speech_bubble_text_constraints(before.body, 45.0, 4.0, 110.0).unwrap();
        assert!((after.body.width - c.body_width).abs() < 1e-9);
        assert!((after.body.height - c.body_height).abs() < 1e-9);
        assert!((text.width() - c.content_width).abs() < 1e-9);
        assert!(text.height() >= 110.0 - 1e-9);
        assert_eq!(text.text_ink_reserve, 4.0);
        assert_eq!(text.center, after.body.center);
        assert!(
            (after.body.center.y
                - after.body.height / 2.0
                - (before.body.center.y - before.body.height / 2.0))
                .abs()
                < 1e-9
        );
        assert!(
            (after.body.center.x
                - after.body.width / 2.0
                - (before.body.center.x - before.body.width / 2.0))
                .abs()
                < 1e-9
        );
        engine.undo_with_viewport_changes().unwrap();
        assert_eq!(body(&engine, owner), before);
        assert!(engine.model.text(label).is_err());
        engine.redo_with_viewport_changes().unwrap();
        assert_eq!(body(&engine, owner), after);
        let (info, style) = engine
            .speech_bubble_text_target(viewport, None)
            .unwrap()
            .unwrap();
        let mut draft = TextDraftCommit::new(
            TextCommitTarget::Existing(label),
            info.center,
            "short",
            TextLayoutSize::new(c.content_width, 20.0),
            style,
            true,
            false,
        );
        draft.minimum_grapheme_width = 45.0;
        draft.text_ink_reserve = 4.0;
        engine
            .commit_text_draft_with_viewport_changes(viewport, draft)
            .unwrap();
        assert_eq!(body(&engine, owner).body.height, after.body.height);
        assert_eq!(body(&engine, owner).body.width, after.body.width);
    }
}

#[test]
fn speech_bubble_draft_cancel_and_empty_commit_keep_body_and_history() {
    let (mut engine, viewport, owner) = create(HighlightShape::Ellipse, 120.0, 50.0);
    let before = body(&engine, owner);
    let (info, style) = engine
        .speech_bubble_text_target(viewport, None)
        .unwrap()
        .unwrap();
    let draft = ActiveTextDraftPresentation {
        target: ActiveTextDraftTarget::NewSpeechBubble(owner),
        revision: 1,
        text: TextData {
            text: "draft".to_owned(),
            layout: TextLayoutSize::new(80.0, 200.0),
            text_ink_reserve: 3.0,
            ..TextData::default()
        },
    };
    engine
        .set_speech_bubble_aware_text_draft(viewport, draft, 35.0)
        .unwrap();
    assert_eq!(body(&engine, owner), before);
    engine
        .clear_active_text_draft_presentation_with_viewport_changes(viewport)
        .unwrap();
    assert_eq!(body(&engine, owner), before);
    let empty = TextDraftCommit::new(
        TextCommitTarget::NewSpeechBubble(owner),
        info.center,
        "   ",
        TextLayoutSize::new(80.0, 20.0),
        style,
        true,
        false,
    );
    engine
        .commit_text_draft_with_viewport_changes(viewport, empty)
        .unwrap();
    assert_eq!(body(&engine, owner), before);
    assert_eq!(engine.model.paint_order(), &[owner]);
    engine.undo_with_viewport_changes().unwrap();
    assert!(engine.model.paint_order().is_empty());
}

#[test]
fn speech_bubble_add_tail_drag_delete_cancel_and_undo_are_single_gestures() {
    let (mut engine, viewport, owner) = create(HighlightShape::Rectangle, 160.0, 90.0);
    let before = body(&engine, owner);
    let add = handle(
        &mut engine,
        viewport,
        UiShapeKind::SpeechBubbleAddTailHandle,
    );
    let tip = Point::new(add.x + 55.0, add.y + 75.0);
    pointer(&mut engine, viewport, PointerEventType::Down, add);
    assert!(engine.has_pointer_capture());
    pointer(&mut engine, viewport, PointerEventType::Move, tip);
    assert_eq!(body(&engine, owner), before);
    pointer(&mut engine, viewport, PointerEventType::Up, tip);
    assert!(!engine.has_pointer_capture());
    let tailed = body(&engine, owner);
    assert_eq!(tailed.tails.len(), 1);
    let actual = speech_bubble_local_to_canvas(&tailed, tailed.tails[0].tip);
    assert!((actual.x - tip.x).abs() < 1e-9 && (actual.y - tip.y).abs() < 1e-9);
    engine.undo_with_viewport_changes().unwrap();
    assert_eq!(body(&engine, owner), before);
    engine.redo_with_viewport_changes().unwrap();
    assert_eq!(body(&engine, owner), tailed);
    let from = handle(
        &mut engine,
        viewport,
        UiShapeKind::SpeechBubbleTailTipHandle,
    );
    pointer(&mut engine, viewport, PointerEventType::Down, from);
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Move,
        tailed.body.center,
    );
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Cancel,
        tailed.body.center,
    );
    assert_eq!(body(&engine, owner), tailed);
    assert!(!engine.has_pointer_capture());
    pointer(&mut engine, viewport, PointerEventType::Down, from);
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Move,
        tailed.body.center,
    );
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Up,
        tailed.body.center,
    );
    assert!(body(&engine, owner).tails.is_empty());
    engine.undo_with_viewport_changes().unwrap();
    assert_eq!(body(&engine, owner), tailed);
}

#[test]
fn speech_bubble_symmetric_base_drag_updates_both_contour_controls() {
    let (mut engine, viewport, owner) = create(HighlightShape::Diamond, 160.0, 90.0);
    let add = handle(
        &mut engine,
        viewport,
        UiShapeKind::SpeechBubbleAddTailHandle,
    );
    let tip = Point::new(add.x + 80.0, add.y + 60.0);
    pointer(&mut engine, viewport, PointerEventType::Down, add);
    pointer(&mut engine, viewport, PointerEventType::Up, tip);
    let before = body(&engine, owner);
    let from = handle(
        &mut engine,
        viewport,
        UiShapeKind::SpeechBubbleTailBaseHandle,
    );
    let to = Point::new(from.x + 30.0, from.y - 30.0);
    pointer(&mut engine, viewport, PointerEventType::Down, from);
    pointer(&mut engine, viewport, PointerEventType::Move, to);
    pointer(&mut engine, viewport, PointerEventType::Up, to);
    let after = body(&engine, owner);
    assert_eq!(after.tails[0].tip, before.tails[0].tip);
    assert!((after.tails[0].base_width - before.tails[0].base_width).abs() > 1e-4);
    let bases = speech_bubble_tail_base_points(&after, &after.tails[0]);
    let axis = after.tails[0].tip.y.atan2(after.tails[0].tip.x);
    for base in bases {
        assert!(
            ((base.y.atan2(base.x) - axis).abs()
                - snow_draw_engine_document::speech_bubble_tail_effective_half_angle(
                    &after,
                    &after.tails[0]
                ))
            .abs()
                < 1e-9
        );
    }
}

#[test]
fn speech_bubble_duplicate_delete_and_serialization_keep_owned_labels_and_tails() {
    let (mut engine, viewport, owner) = create(HighlightShape::Ellipse, 160.0, 90.0);
    let label = bind(&mut engine, viewport, owner, 30.0, 15.0, 2.0);
    let add = handle(
        &mut engine,
        viewport,
        UiShapeKind::SpeechBubbleAddTailHandle,
    );
    pointer(&mut engine, viewport, PointerEventType::Down, add);
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Up,
        Point::new(add.x - 50.0, add.y + 80.0),
    );
    let original = body(&engine, owner);
    engine
        .duplicate_selected_with_viewport_changes(viewport, Point::new(25.0, 35.0))
        .unwrap();
    let duplicate = engine.selected_ids()[0];
    assert_ne!(duplicate, owner);
    let cloned = body(&engine, duplicate);
    assert_ne!(cloned.text_element_id, Some(label));
    assert_eq!(cloned.tails, original.tails);
    assert_eq!(
        engine
            .model
            .text(cloned.text_element_id.unwrap())
            .unwrap()
            .text,
        engine.model.text(label).unwrap().text
    );
    let saved = engine.serialize_document_session().unwrap();
    let restored =
        Engine::from_serialized_document_session_with_config(&saved, EngineConfig::default())
            .unwrap();
    assert_eq!(body(&restored, duplicate), cloned);
    assert_eq!(restored.model.speech_bubble_text_bindings().len(), 2);
    engine
        .delete_selected_with_viewport_changes(viewport)
        .unwrap();
    assert!(engine.model.speech_bubble(duplicate).is_err());
    assert!(engine.model.text(cloned.text_element_id.unwrap()).is_err());
    assert!(engine.model.speech_bubble(owner).is_ok());
    engine.undo_with_viewport_changes().unwrap();
    assert_eq!(body(&engine, duplicate), cloned);
}

#[test]
fn speech_bubble_rotated_growth_keeps_top_left_and_tail_canvas_positions() {
    use snow_draw_engine_document::Transaction;
    for shape in [
        HighlightShape::Rectangle,
        HighlightShape::Ellipse,
        HighlightShape::Diamond,
    ] {
        let (mut engine, viewport, owner) = create(shape, 160.0, 90.0);
        let mut rotated = body(&engine, owner);
        rotated.body.rotation = 0.57;
        let mut tx = Transaction::new("rotate bubble fixture");
        tx.update_speech_bubble(owner, rotated.clone());
        engine
            .commit_transaction(
                viewport,
                ApplyTransactionCommand {
                    transaction: tx,
                    history_undo_snapshot: None,
                },
            )
            .unwrap();
        let add = handle(
            &mut engine,
            viewport,
            UiShapeKind::SpeechBubbleAddTailHandle,
        );
        let tip = speech_bubble_local_to_canvas(&rotated, Point::new(180.0, 180.0));
        pointer(&mut engine, viewport, PointerEventType::Down, add);
        pointer(&mut engine, viewport, PointerEventType::Up, tip);
        let before = body(&engine, owner);
        let corner = speech_bubble_local_to_canvas(&before, Point::new(-80.0, -45.0));
        let label = bind(&mut engine, viewport, owner, 130.0, 20.0, 3.0);
        let after = body(&engine, owner);
        let grown_corner = speech_bubble_local_to_canvas(
            &after,
            Point::new(-after.body.width / 2.0, -after.body.height / 2.0),
        );
        assert!((corner.x - grown_corner.x).hypot(corner.y - grown_corner.y) < 1e-8);
        let grown_tip = speech_bubble_local_to_canvas(&after, after.tails[0].tip);
        assert!((tip.x - grown_tip.x).hypot(tip.y - grown_tip.y) < 1e-8);
        assert_eq!(
            engine.model.text(label).unwrap().rotation,
            after.body.rotation
        );
    }
}

#[test]
fn speech_bubble_empty_existing_text_unbinds_label_and_retains_body_and_tails() {
    let (mut engine, viewport, owner) = create(HighlightShape::Rectangle, 160.0, 90.0);
    let label = bind(&mut engine, viewport, owner, 40.0, 15.0, 2.0);
    let before = body(&engine, owner);
    let (info, style) = engine
        .speech_bubble_text_target(viewport, None)
        .unwrap()
        .unwrap();
    let draft = TextDraftCommit::new(
        TextCommitTarget::Existing(label),
        info.center,
        "",
        TextLayoutSize::new(info.width, 20.0),
        style,
        true,
        false,
    );
    engine
        .commit_text_draft_with_viewport_changes(viewport, draft)
        .unwrap();
    let after = body(&engine, owner);
    assert_eq!(after.body, before.body);
    assert_eq!(after.tails, before.tails);
    assert_eq!(after.text_element_id, None);
    assert!(engine.model.text(label).is_err());
    engine.undo_with_viewport_changes().unwrap();
    assert_eq!(body(&engine, owner), before);
}

#[test]
fn speech_bubble_focus_loss_cancels_captured_tail_preview() {
    let (mut engine, viewport, owner) = create(HighlightShape::Ellipse, 160.0, 90.0);
    let before = body(&engine, owner);
    let add = handle(
        &mut engine,
        viewport,
        UiShapeKind::SpeechBubbleAddTailHandle,
    );
    pointer(&mut engine, viewport, PointerEventType::Down, add);
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Move,
        Point::new(add.x + 60.0, add.y + 70.0),
    );
    engine
        .process_input_with_viewport_changes(viewport, InputEvent::FocusLost)
        .unwrap();
    assert!(!engine.has_pointer_capture());
    assert_eq!(body(&engine, owner), before);
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Up,
        Point::new(add.x + 60.0, add.y + 70.0),
    );
    assert_eq!(body(&engine, owner), before);
}

#[test]
fn speech_bubble_text_measurement_cache_reuses_warm_frames_and_rebuilds_only_touched_owner() {
    use snow_draw_engine_document::Transaction;
    let (mut engine, viewport, owner) = create(HighlightShape::Rectangle, 160.0, 90.0);
    bind(&mut engine, viewport, owner, 30.0, 15.0, 2.0);
    for index in 0..12 {
        engine
            .select_element_with_viewport_changes(viewport, owner)
            .unwrap();
        engine
            .duplicate_selected_with_viewport_changes(
                viewport,
                Point::new(20.0 * index as f64, 10.0),
            )
            .unwrap();
    }
    let requests = engine.speech_bubble_text_layout_requests(viewport).unwrap();
    assert_eq!(requests.len(), 13);
    let warm = engine.speech_bubble_text_request_build_count();
    for _ in 0..25 {
        engine.acquire_patch(viewport, None).unwrap();
        engine.speech_bubble_text_layout_requests(viewport).unwrap();
    }
    assert_eq!(engine.speech_bubble_text_request_build_count(), warm);
    let text_id = engine.model.bound_text_id_for_speech_bubble(owner).unwrap();
    let mut text = engine.model.text(text_id).unwrap().clone();
    text.text.push('!');
    let mut tx = Transaction::new("change one bubble label");
    tx.update_text(text_id, text);
    engine
        .commit_transaction(
            viewport,
            ApplyTransactionCommand {
                transaction: tx,
                history_undo_snapshot: None,
            },
        )
        .unwrap();
    engine.speech_bubble_text_layout_requests(viewport).unwrap();
    assert_eq!(engine.speech_bubble_text_request_build_count(), warm + 1);
}

#[test]
fn speech_bubble_manual_resize_enforces_accepted_grapheme_and_text_height_minimums() {
    use snow_draw_engine_editor::SpeechBubbleTextMeasurement;
    for shape in [
        HighlightShape::Rectangle,
        HighlightShape::Ellipse,
        HighlightShape::Diamond,
    ] {
        let (mut engine, viewport, owner) = create(shape, 160.0, 90.0);
        let label = bind(&mut engine, viewport, owner, 30.0, 55.0, 3.0);
        let original = body(&engine, owner);
        let request = engine
            .speech_bubble_text_layout_requests(viewport)
            .unwrap()
            .into_iter()
            .find(|request| request.owner_id == owner)
            .unwrap();
        let text = engine.model.text(label).unwrap();
        let measured = SpeechBubbleTextMeasurement {
            key: request.key,
            size: TextLayoutSize::with_content(text.width(), 30.0, text.width() - 1.0, 30.0),
            minimum_grapheme_width: 55.0,
            text_ink_reserve: 3.0,
        };
        engine
            .apply_speech_bubble_text_layouts(viewport, &[(label, measured)])
            .unwrap();
        let minimum = speech_bubble_text_constraints(
            snow_draw_engine_document::RectangleData {
                width: 1.0,
                height: 1.0,
                ..original.body
            },
            55.0,
            3.0,
            30.0,
        )
        .unwrap();
        let from = handle(&mut engine, viewport, UiShapeKind::SelectionResizeHandle);
        let to = Point::new(
            original.body.center.x + original.body.width / 2.0 - 5.0,
            original.body.center.y + original.body.height / 2.0 - 5.0,
        );
        pointer(&mut engine, viewport, PointerEventType::Down, from);
        pointer(&mut engine, viewport, PointerEventType::Move, to);
        pointer(&mut engine, viewport, PointerEventType::Up, to);
        let resized = body(&engine, owner);
        assert!(
            resized.body.width + 1e-8 >= minimum.body_width,
            "{shape:?} width {} below {}",
            resized.body.width,
            minimum.body_width
        );
        assert!(
            resized.body.height + 1e-8 >= minimum.body_height,
            "{shape:?} height {} below {}",
            resized.body.height,
            minimum.body_height
        );
    }
}

#[test]
fn speech_bubble_collision_offset_controls_do_not_jump_or_create_history_on_click() {
    let (mut engine, viewport, owner) = create(HighlightShape::Ellipse, 160.0, 90.0);
    let add = handle(
        &mut engine,
        viewport,
        UiShapeKind::SpeechBubbleAddTailHandle,
    );
    pointer(&mut engine, viewport, PointerEventType::Down, add);
    pointer(&mut engine, viewport, PointerEventType::Up, add);
    let before = body(&engine, owner);
    let tip_control = handle(
        &mut engine,
        viewport,
        UiShapeKind::SpeechBubbleTailTipHandle,
    );
    assert!((tip_control.x - add.x).hypot(tip_control.y - add.y) > 8.0);
    for kind in [
        UiShapeKind::SpeechBubbleTailTipHandle,
        UiShapeKind::SpeechBubbleTailBaseHandle,
    ] {
        let from = handle(&mut engine, viewport, kind);
        pointer(&mut engine, viewport, PointerEventType::Down, from);
        pointer(&mut engine, viewport, PointerEventType::Up, from);
        assert_eq!(body(&engine, owner), before);
    }
    engine.undo_with_viewport_changes().unwrap();
    assert!(body(&engine, owner).tails.is_empty());
}

#[test]
fn speech_bubble_manual_reflection_keeps_canvas_tip_and_replays_history() {
    let (mut engine, viewport, owner) = create(HighlightShape::Rectangle, 160.0, 90.0);
    let add = handle(
        &mut engine,
        viewport,
        UiShapeKind::SpeechBubbleAddTailHandle,
    );
    pointer(&mut engine, viewport, PointerEventType::Down, add);
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Up,
        Point::new(200.0, 150.0),
    );
    let before = body(&engine, owner);
    let from = handle(&mut engine, viewport, UiShapeKind::SelectionResizeHandle);
    pointer(&mut engine, viewport, PointerEventType::Down, from);
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Move,
        Point::new(140.0, 100.0),
    );
    pointer(
        &mut engine,
        viewport,
        PointerEventType::Up,
        Point::new(140.0, 100.0),
    );
    let after = body(&engine, owner);
    assert_eq!(after.tails.len(), 1);
    assert_canvas_tips_fixed(&before, &after);
    engine.undo_with_viewport_changes().unwrap();
    assert_eq!(body(&engine, owner), before);
    engine.redo_with_viewport_changes().unwrap();
    assert_eq!(body(&engine, owner), after);
}

#[test]
fn speech_bubble_empty_draft_commit_preserves_grown_body_and_history_without_label() {
    let (mut engine, viewport, owner) = create(HighlightShape::Ellipse, 120.0, 50.0);
    let before = body(&engine, owner);
    let (info, style) = engine
        .speech_bubble_text_target(viewport, None)
        .unwrap()
        .unwrap();
    let draft = ActiveTextDraftPresentation {
        target: ActiveTextDraftTarget::NewSpeechBubble(owner),
        revision: 1,
        text: TextData {
            text: "temporary long text".to_owned(),
            layout: TextLayoutSize::new(80.0, 200.0),
            text_ink_reserve: 3.0,
            ..TextData::default()
        },
    };
    engine
        .set_speech_bubble_aware_text_draft(viewport, draft, 35.0)
        .unwrap();
    let empty = TextDraftCommit::new(
        TextCommitTarget::NewSpeechBubble(owner),
        info.center,
        "",
        TextLayoutSize::new(80.0, 20.0),
        style,
        true,
        false,
    );
    engine
        .commit_text_draft_with_viewport_changes(viewport, empty)
        .unwrap();
    let grown = body(&engine, owner);
    assert!(grown.body.height > before.body.height);
    assert_eq!(grown.text_element_id, None);
    assert_eq!(engine.model.paint_order(), &[owner]);
    let saved = engine.serialize_document_session().unwrap();
    let restored =
        Engine::from_serialized_document_session_with_config(&saved, EngineConfig::default())
            .unwrap();
    assert_eq!(body(&restored, owner), grown);
    engine.undo_with_viewport_changes().unwrap();
    assert_eq!(body(&engine, owner), before);
    engine.redo_with_viewport_changes().unwrap();
    assert_eq!(body(&engine, owner), grown);
}

fn bubble_group_bounds(
    engine: &Engine,
    viewport: ViewportId,
) -> snow_draw_engine_editor::SelectionBounds {
    engine
        .editor
        .presentation_state(&engine.model, &engine.viewport_slot(viewport).unwrap().view)
        .selection_bounds
        .unwrap()
}

fn select_bubble_group(engine: &mut Engine, viewport: ViewportId, ids: &[ElementId]) {
    let snapshot = serde_json::from_value(serde_json::json!({
        "selectedIds": ids, "primaryId": ids[0]
    }))
    .unwrap();
    engine
        .editor
        .restore_history_selection(&engine.model, &snapshot);
    engine.refresh_all_viewports().unwrap();
    assert_eq!(engine.selected_ids(), ids);
    assert!(engine.viewport_slot(viewport).is_ok());
}

fn bubble_group_fixture(
    shape: HighlightShape,
    rotation: f64,
) -> (Engine, ViewportId, [ElementId; 2]) {
    let (mut engine, viewport, owner) = create(shape, 160.0, 80.0);
    engine
        .set_viewport_active_tool(viewport, ActiveTool::Select)
        .unwrap();
    let mut bubble = body(&engine, owner);
    bubble.body.rotation = rotation;
    bubble
        .tails
        .push(snow_draw_engine_document::speech_bubble_default_tail(
            &bubble,
            Point::new(-150.0, 180.0),
            1.0,
        ));
    let other = engine.model.peek_next_element_id();
    let mut rect = bubble.body;
    rect.center = Point::new(180.0, 0.0);
    rect.width = 40.0;
    rect.height = 60.0;
    rect.rotation = 0.0;
    let mut transaction = Transaction::new("bubble group bounds fixture");
    transaction.update_speech_bubble(owner, bubble);
    transaction.insert_rectangle(other, Default::default(), rect);
    engine
        .apply_editor_command(
            viewport,
            EditorCommand::ApplyTransaction(ApplyTransactionCommand {
                transaction,
                history_undo_snapshot: None,
            }),
        )
        .unwrap();
    select_bubble_group(&mut engine, viewport, &[owner, other]);
    (engine, viewport, [owner, other])
}

fn assert_bounds_close(
    actual: snow_draw_engine_editor::SelectionBounds,
    center: Point<f64>,
    width: f64,
    height: f64,
) {
    assert!((actual.center.x - center.x).abs() < 1e-7, "{actual:?}");
    assert!((actual.center.y - center.y).abs() < 1e-7, "{actual:?}");
    assert!((actual.width - width).abs() < 1e-7, "{actual:?}");
    assert!((actual.height - height).abs() < 1e-7, "{actual:?}");
}

fn assert_canvas_tips_fixed(original: &SpeechBubbleData, actual: &SpeechBubbleData) {
    assert_eq!(actual.tails.len(), original.tails.len());
    for (before, after) in original.tails.iter().zip(&actual.tails) {
        let expected = speech_bubble_local_to_canvas(original, before.tip);
        let actual = speech_bubble_local_to_canvas(actual, after.tip);
        assert!(
            (expected.x - actual.x).hypot(expected.y - actual.y) < 1e-7,
            "tail {} moved from {expected:?} to {actual:?}",
            before.id
        );
        assert_eq!(after.id, before.id);
        assert_eq!(after.base_width, before.base_width);
    }
}

#[test]
fn speech_bubble_move_rotate_and_resize_keep_canvas_tips_in_preview_commit_and_history() {
    for shape in [
        HighlightShape::Rectangle,
        HighlightShape::Ellipse,
        HighlightShape::Diamond,
    ] {
        for rotation in [0.0, 0.57] {
            for gesture in 0..3 {
                let (mut engine, viewport, owner) = create(shape, 160.0, 80.0);
                let label = bind(&mut engine, viewport, owner, 24.0, 12.0, 2.0);
                engine
                    .set_viewport_active_tool(viewport, ActiveTool::Select)
                    .unwrap();
                let mut original = body(&engine, owner);
                original.body.rotation = rotation;
                original.body.fill.a = 100;
                for tip in [Point::new(-240.0, 210.0), Point::new(270.0, -220.0)] {
                    original
                        .tails
                        .push(snow_draw_engine_document::speech_bubble_default_tail(
                            &original, tip, 1.0,
                        ));
                }
                let mut tx = Transaction::new("fixed tail gesture fixture");
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
                engine.speech_bubble_text_layout_requests(viewport).unwrap();
                assert_eq!(body(&engine, owner), original, "fixture must be committed");
                let from = match gesture {
                    0 => speech_bubble_local_to_canvas(
                        &original,
                        Point::new(0.0, -original.body.height * 0.35),
                    ),
                    1 => handle(&mut engine, viewport, UiShapeKind::SelectionRotationHandle),
                    _ => handle(&mut engine, viewport, UiShapeKind::SelectionResizeHandle),
                };
                let target = if gesture == 1 {
                    let center = original.body.center;
                    Point::new(
                        center.x - (from.y - center.y),
                        center.y + (from.x - center.x),
                    )
                } else {
                    Point::new(from.x - 35.0, from.y - 25.0)
                };
                for cancel in [true, false] {
                    pointer(&mut engine, viewport, PointerEventType::Down, from);
                    pointer(
                        &mut engine,
                        viewport,
                        PointerEventType::Move,
                        Point::new(
                            from.x + (target.x - from.x) * 0.2,
                            from.y + (target.y - from.y) * 0.2,
                        ),
                    );
                    for fraction in [0.4, 0.8] {
                        let point = Point::new(
                            from.x + (target.x - from.x) * fraction,
                            from.y + (target.y - from.y) * fraction,
                        );
                        pointer(&mut engine, viewport, PointerEventType::Move, point);
                        let presentation = engine.editor.presentation_state(
                            &engine.model,
                            &engine.viewport_slot(viewport).unwrap().view,
                        );
                        let preview = &presentation
                            .speech_bubble_previews
                            .iter()
                            .find(|preview| preview.0 == owner)
                            .unwrap()
                            .1;
                        assert_ne!(
                            preview.body,
                            original.body,
                            "{shape:?}, rotation {rotation}, gesture {gesture}, selected {:?}, bounds {:?}",
                            engine.selected_ids(),
                            presentation.selection_bounds
                        );
                        assert_canvas_tips_fixed(&original, preview);
                        assert_eq!(body(&engine, owner), original);
                    }
                    pointer(
                        &mut engine,
                        viewport,
                        if cancel {
                            PointerEventType::Cancel
                        } else {
                            PointerEventType::Up
                        },
                        target,
                    );
                    if cancel {
                        assert_eq!(body(&engine, owner), original);
                    }
                }
                let result = body(&engine, owner);
                assert_ne!(result.body, original.body);
                assert_canvas_tips_fixed(&original, &result);
                let text = engine.model.text(label).unwrap();
                assert_eq!(text.center, result.body.center);
                assert_eq!(text.rotation, result.body.rotation);
                engine.undo_with_viewport_changes().unwrap();
                assert_eq!(body(&engine, owner), original);
                engine.redo_with_viewport_changes().unwrap();
                assert_eq!(body(&engine, owner), result);
            }
        }
    }
}

#[test]
fn speech_bubble_duplicate_drag_carries_the_copy_tail_without_moving_original() {
    let (mut engine, viewport, ids) = bubble_group_fixture(HighlightShape::Rectangle, 0.37);
    select_bubble_group(&mut engine, viewport, &[ids[0]]);
    let original = body(&engine, ids[0]);
    let start = original.body.center;
    let target = Point::new(start.x - 35.0, start.y - 25.0);
    for (event, point) in [
        (PointerEventType::Down, start),
        (PointerEventType::Move, target),
    ] {
        super::duplicate_drag_tests::pointer(
            &mut engine,
            viewport,
            event,
            point.x + 400.0,
            point.y + 300.0,
            true,
        );
    }
    let presentation = engine
        .editor
        .presentation_state(&engine.model, &engine.viewport_slot(viewport).unwrap().view);
    assert!(presentation.duplicate_preview.is_some());
    assert!(presentation.speech_bubble_previews.is_empty());
    let transaction = presentation.duplicate_preview.as_ref().unwrap();
    let preview = transaction
        .operations()
        .iter()
        .find_map(|operation| match operation {
            snow_draw_engine_document::Operation::InsertElement {
                data: snow_draw_engine_document::ElementData::SpeechBubble(bubble),
                ..
            } => Some(bubble),
            _ => None,
        })
        .unwrap();
    assert_eq!(preview.tails, original.tails);
    assert_eq!(preview.body.center, target);
    assert_eq!(body(&engine, ids[0]), original);
    super::duplicate_drag_tests::pointer(
        &mut engine,
        viewport,
        PointerEventType::Up,
        target.x + 400.0,
        target.y + 300.0,
        true,
    );
    let copy_id = engine.selected_ids()[0];
    assert_ne!(copy_id, ids[0]);
    let copied = body(&engine, copy_id);
    assert_eq!(copied.tails, original.tails);
    assert_eq!(copied.body.center, target);
    assert_eq!(body(&engine, ids[0]), original);
    engine.undo_with_viewport_changes().unwrap();
    assert!(engine.model.speech_bubble(copy_id).is_err());
    assert_eq!(body(&engine, ids[0]), original);
    engine.redo_with_viewport_changes().unwrap();
    assert_eq!(body(&engine, copy_id), copied);
}

#[test]
fn speech_bubble_alignment_keeps_canvas_tail_tip_and_replays_history() {
    for shape in [
        HighlightShape::Rectangle,
        HighlightShape::Ellipse,
        HighlightShape::Diamond,
    ] {
        let (mut engine, viewport, ids) = bubble_group_fixture(shape, 0.37);
        let original = body(&engine, ids[0]);
        engine
            .align_selected_with_viewport_changes(viewport, 2)
            .unwrap();
        let result = body(&engine, ids[0]);
        assert_ne!(result.body.center, original.body.center);
        assert_canvas_tips_fixed(&original, &result);
        engine.undo_with_viewport_changes().unwrap();
        assert_eq!(body(&engine, ids[0]), original);
        engine.redo_with_viewport_changes().unwrap();
        assert_eq!(body(&engine, ids[0]), result);
    }
}

#[test]
fn speech_bubble_group_frame_encloses_tails_single_frame_stays_on_body() {
    for shape in [
        HighlightShape::Rectangle,
        HighlightShape::Ellipse,
        HighlightShape::Diamond,
    ] {
        for rotation in [0.0, 0.37] {
            let (mut engine, viewport, ids) = bubble_group_fixture(shape, rotation);
            let bubble = body(&engine, ids[0]);
            let path = snow_draw_engine_document::speech_bubble_path_geometry(&bubble);
            let mut min_x = path.canvas_bounds[0].min(160.0);
            let mut min_y = path.canvas_bounds[1].min(-30.0);
            let mut max_x = path.canvas_bounds[2].max(200.0);
            let mut max_y = path.canvas_bounds[3].max(30.0);
            for (x, y) in [(-80.0, -40.0), (80.0, -40.0), (80.0, 40.0), (-80.0, 40.0)] {
                let point = speech_bubble_local_to_canvas(&bubble, Point::new(x, y));
                min_x = min_x.min(point.x);
                min_y = min_y.min(point.y);
                max_x = max_x.max(point.x);
                max_y = max_y.max(point.y);
            }
            let bounds = bubble_group_bounds(&engine, viewport);
            assert_bounds_close(
                bounds,
                Point::new(f64::midpoint(min_x, max_x), f64::midpoint(min_y, max_y)),
                max_x - min_x,
                max_y - min_y,
            );
            assert_eq!(bounds.rotation, 0.0);
            select_bubble_group(&mut engine, viewport, &[ids[0]]);
            let single = bubble_group_bounds(&engine, viewport);
            assert_bounds_close(single, bubble.body.center, 160.0, 80.0);
            assert_eq!(single.rotation, rotation);
            let add = handle(
                &mut engine,
                viewport,
                UiShapeKind::SpeechBubbleAddTailHandle,
            );
            let rotate = handle(&mut engine, viewport, UiShapeKind::SelectionRotationHandle);
            assert!((add.x + rotate.x - 2.0 * bubble.body.center.x).abs() < 1e-9);
            assert!((add.y + rotate.y - 2.0 * bubble.body.center.y).abs() < 1e-9);
        }
    }
}

#[test]
fn speech_bubble_group_rotates_body_around_full_silhouette_pivot_and_keeps_tip_fixed() {
    let (mut engine, viewport, ids) = bubble_group_fixture(HighlightShape::Rectangle, 0.0);
    let original = body(&engine, ids[0]);
    let other = *engine.model.rectangle(ids[1]).unwrap();
    let pivot = Point::new(25.0, 70.0);
    assert_bounds_close(bubble_group_bounds(&engine, viewport), pivot, 350.0, 220.0);
    let rotate = handle(&mut engine, viewport, UiShapeKind::SelectionRotationHandle);
    let target = Point::new(
        pivot.x - (rotate.y - pivot.y),
        pivot.y + (rotate.x - pivot.x),
    );
    pointer(&mut engine, viewport, PointerEventType::Down, rotate);
    pointer(&mut engine, viewport, PointerEventType::Move, target);
    let presentation = engine
        .editor
        .presentation_state(&engine.model, &engine.viewport_slot(viewport).unwrap().view);
    let preview = &presentation
        .speech_bubble_previews
        .iter()
        .find(|preview| preview.0 == ids[0])
        .unwrap()
        .1;
    assert_canvas_tips_fixed(&original, preview);
    let preview_bounds = presentation.selection_bounds.unwrap();
    assert_bounds_close(preview_bounds, Point::new(-7.5, 70.0), 350.0, 285.0);
    assert!((preview_bounds.rotation - std::f64::consts::FRAC_PI_2).abs() < 1e-7);
    assert_eq!(body(&engine, ids[0]), original);
    pointer(&mut engine, viewport, PointerEventType::Up, target);
    let result = body(&engine, ids[0]);
    assert!((result.body.center.x - 95.0).abs() < 1e-7);
    assert!((result.body.center.y - 45.0).abs() < 1e-7);
    assert_canvas_tips_fixed(&original, &result);
    assert_bounds_close(
        bubble_group_bounds(&engine, viewport),
        Point::new(-7.5, 105.0),
        285.0,
        280.0,
    );
    engine.undo_with_viewport_changes().unwrap();
    assert_eq!(body(&engine, ids[0]), original);
    assert_eq!(*engine.model.rectangle(ids[1]).unwrap(), other);
    assert_bounds_close(bubble_group_bounds(&engine, viewport), pivot, 350.0, 220.0);
    engine.redo_with_viewport_changes().unwrap();
    assert_eq!(body(&engine, ids[0]), result);
}

#[test]
fn speech_bubble_group_tail_only_update_refreshes_frame_and_history() {
    let (mut engine, viewport, ids) = bubble_group_fixture(HighlightShape::Diamond, 0.0);
    let before = body(&engine, ids[0]);
    let mut next = before.clone();
    next.tails.clear();
    let mut transaction = Transaction::new("remove group bubble tail");
    transaction.update_speech_bubble(ids[0], next);
    engine
        .apply_editor_command(
            viewport,
            EditorCommand::ApplyTransaction(ApplyTransactionCommand {
                transaction,
                history_undo_snapshot: None,
            }),
        )
        .unwrap();
    assert_bounds_close(
        bubble_group_bounds(&engine, viewport),
        Point::new(60.0, 0.0),
        280.0,
        80.0,
    );
    engine.undo_with_viewport_changes().unwrap();
    assert_eq!(body(&engine, ids[0]), before);
    assert_bounds_close(
        bubble_group_bounds(&engine, viewport),
        Point::new(25.0, 70.0),
        350.0,
        220.0,
    );
}

#[test]
fn speech_bubble_group_bounds_change_preserves_other_groups_rotated_preview_frames() {
    let (mut engine, viewport, ids) = bubble_group_fixture(HighlightShape::Rectangle, 0.0);
    let rectangle = body(&engine, ids[0]).body;
    let replacement = engine.model.peek_next_element_id();
    let mut transaction = Transaction::new("ordinary rectangle group fixture");
    transaction.remove_element(ids[0]);
    transaction.insert_rectangle(replacement, Default::default(), rectangle);
    engine
        .apply_editor_command(
            viewport,
            EditorCommand::ApplyTransaction(ApplyTransactionCommand {
                transaction,
                history_undo_snapshot: None,
            }),
        )
        .unwrap();
    select_bubble_group(&mut engine, viewport, &[replacement, ids[1]]);
    let original = bubble_group_bounds(&engine, viewport);
    assert_bounds_close(original, Point::new(60.0, 0.0), 280.0, 80.0);
    let rotate = handle(&mut engine, viewport, UiShapeKind::SelectionRotationHandle);
    let target = Point::new(
        original.center.x - (rotate.y - original.center.y),
        original.center.y + (rotate.x - original.center.x),
    );
    pointer(&mut engine, viewport, PointerEventType::Down, rotate);
    pointer(&mut engine, viewport, PointerEventType::Move, target);
    let preview = bubble_group_bounds(&engine, viewport);
    assert_bounds_close(preview, original.center, original.width, original.height);
    assert!((preview.rotation - std::f64::consts::FRAC_PI_2).abs() < 1e-7);
    pointer(&mut engine, viewport, PointerEventType::Cancel, target);
    assert_eq!(bubble_group_bounds(&engine, viewport), original);
}
