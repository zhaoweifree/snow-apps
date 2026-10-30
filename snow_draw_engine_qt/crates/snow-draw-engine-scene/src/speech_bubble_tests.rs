use super::*;
use snow_draw_engine_core::{Camera, SurfaceSize};
use snow_draw_engine_document::{
    ElementMeta, HighlightShape, RectangleElementKind, SpeechBubbleData, TextLayoutSize,
    Transaction, speech_bubble_default_tail, speech_bubble_grow_height,
};

fn bubble(shape: HighlightShape) -> SpeechBubbleData {
    let mut value = SpeechBubbleData::new(RectangleData {
        text_element_id: None,
        stroke_rendering_style: Default::default(),
        stroke_rendering_seed: 0,
        rectangle_kind: RectangleElementKind::Rectangle,
        highlight_shape: shape,
        center: Point::default(),
        width: 120.0,
        height: 80.0,
        rotation: 0.2,
        fill: ColorRgba8 {
            r: 240,
            g: 240,
            b: 240,
            a: 160,
        },
        fill_style: FillStyle::Solid,
        stroke: ColorRgba8 {
            r: 20,
            g: 30,
            b: 40,
            a: 180,
        },
        stroke_width: 4.0,
        stroke_style: StrokeStyle::Solid,
        corner_radii: CornerRadii::splat(9.0),
        opacity: 1.0,
    });
    let tail = speech_bubble_default_tail(&value, Point::new(40.0, 135.0), 1.0);
    value.tails.push(tail);
    value
}
fn frame() -> FrameView {
    FrameView {
        surface: SurfaceSize {
            width: 500,
            height: 500,
        },
        camera: Camera {
            center: Point::default(),
            zoom: 1.0,
        },
        clear_color: Default::default(),
    }
}
#[test]
fn speech_bubble_scene_tail_edits_publish_closed_geometry_and_complete_damage() {
    for shape in [
        HighlightShape::Rectangle,
        HighlightShape::Ellipse,
        HighlightShape::Diamond,
    ] {
        let owner = ElementId {
            index: 1,
            generation: 1,
        };
        let before = bubble(shape);
        let mut after = before.clone();
        after.tails[0].tip = Point::new(-110.0, 170.0);
        let before = scene_item_from_speech_bubble(owner, &before);
        let after = scene_item_from_speech_bubble(owner, &after);
        let (patch, _) = build_scene_layer_patch(
            std::slice::from_ref(&before),
            1,
            std::slice::from_ref(&after),
            frame(),
            false,
            false,
        );
        let paths = build_path_geometry_ops(std::slice::from_ref(&before), &patch);
        assert_eq!(paths.len(), 1);
        assert!(paths[0].closed);
        assert!(!paths[0].ranges.is_empty());
        assert!(paths[0].resulting_geometry_revision != paths[0].expected_geometry_revision);
        for item in [&before, &after] {
            let bounds = scene_display_item_bounds(item, frame()).unwrap();
            assert!(
                patch
                    .dirty_regions
                    .iter()
                    .any(|dirty| dirty.min_x <= bounds.min_x
                        && dirty.min_y <= bounds.min_y
                        && dirty.max_x >= bounds.max_x
                        && dirty.max_y >= bounds.max_y)
            );
        }
        let SceneDisplayItem::SpeechBubble(item) = after else {
            panic!("bubble display")
        };
        assert!(item.path.geometry.closed);
        assert!(item.path.path_commands.len() > 4);
    }
}
#[test]
fn speech_bubble_unchanged_scene_does_not_publish_geometry() {
    let item = scene_item_from_speech_bubble(
        ElementId {
            index: 0,
            generation: 1,
        },
        &bubble(HighlightShape::Diamond),
    );
    let (patch, _) = build_scene_layer_patch(
        std::slice::from_ref(&item),
        1,
        std::slice::from_ref(&item),
        frame(),
        false,
        false,
    );
    assert!(patch.ops.is_empty());
    assert!(patch.dirty_regions.is_empty());
    assert!(build_path_geometry_ops(&[item], &patch).is_empty());
}

#[test]
fn speech_bubble_offscreen_previews_enter_view_in_document_paint_order() {
    let owner = ElementId {
        index: 0,
        generation: 1,
    };
    let label = ElementId {
        index: 1,
        generation: 1,
    };
    let foreground = ElementId {
        index: 2,
        generation: 1,
    };
    let mut value = bubble(HighlightShape::Rectangle);
    value.body.center = Point::new(0.0, -180.0);
    value.body.width = 100.0;
    value.body.height = 40.0;
    value.body.rotation = 0.0;
    value.tails.clear();
    value.text_element_id = Some(label);
    let mut model = DocumentModel::new();
    let mut tx = Transaction::new("offscreen bubble preview");
    tx.insert_speech_bubble(owner, ElementMeta::default(), value.clone());
    tx.insert_text(
        label,
        ElementMeta::default(),
        TextData {
            text: "label".into(),
            layout: TextLayoutSize::new(60.0, 10.0),
            ..Default::default()
        },
    );
    let mut front = value.body;
    front.center = Point::default();
    tx.insert_rectangle(foreground, ElementMeta::default(), front);
    model.apply_transaction(tx).unwrap();
    let mut cache = DocumentSceneCache::new();
    cache.sync(&model, None);
    let view = FrameView {
        surface: SurfaceSize {
            width: 400,
            height: 300,
        },
        ..frame()
    };
    let ids = |items: Vec<SceneDisplayItem>| {
        items
            .into_iter()
            .map(|item| match item {
                SceneDisplayItem::SpeechBubble(item) => item.body.id,
                SceneDisplayItem::Text(item) => item.id,
                SceneDisplayItem::Rectangle(item) => item.id,
                _ => panic!("unexpected scene item"),
            })
            .collect::<Vec<_>>()
    };
    assert_eq!(
        ids(compose_scene_items(
            &cache,
            &model,
            &EditorPresentationState::default(),
            view
        )),
        vec![display_item_id(foreground)]
    );

    let mut tail_preview = value.clone();
    tail_preview.tails.push(speech_bubble_default_tail(
        &tail_preview,
        Point::new(0.0, 80.0),
        1.0,
    ));
    let presentation = EditorPresentationState {
        speech_bubble_previews: vec![(owner, tail_preview)],
        ..Default::default()
    };
    assert_eq!(
        ids(compose_scene_items(&cache, &model, &presentation, view)),
        vec![display_item_id(owner), display_item_id(foreground)]
    );

    let grown = speech_bubble_grow_height(&value, 200.0);
    let mut grown_text = model.text(label).unwrap().clone();
    grown_text.center = grown.body.center;
    grown_text.layout = TextLayoutSize::new(60.0, 100.0);
    let presentation = EditorPresentationState {
        speech_bubble_previews: vec![(owner, grown)],
        speech_bubble_text_previews: vec![(label, grown_text)],
        ..Default::default()
    };
    assert_eq!(
        ids(compose_scene_items(&cache, &model, &presentation, view)),
        vec![
            display_item_id(owner),
            display_item_id(label),
            display_item_id(foreground)
        ]
    );

    let presentation = EditorPresentationState {
        speech_bubble_previews: vec![(owner, value)],
        speech_bubble_text_previews: vec![(label, model.text(label).unwrap().clone())],
        ..Default::default()
    };
    assert_eq!(
        ids(compose_scene_items(&cache, &model, &presentation, view)),
        vec![display_item_id(foreground)]
    );
}

#[test]
fn speech_bubble_hidden_measured_previews_never_render_and_show_restores_them() {
    use snow_draw_engine_editor::{SpeechBubbleTextMeasurement, speech_bubble_text_constraints};
    let owner = ElementId {
        index: 0,
        generation: 1,
    };
    let label = ElementId {
        index: 1,
        generation: 1,
    };
    let mut value = bubble(HighlightShape::Rectangle);
    value.text_element_id = Some(label);
    let mut model = DocumentModel::new();
    let mut transaction = Transaction::new("hidden bubble measurements");
    transaction.insert_speech_bubble(
        owner,
        ElementMeta {
            visible: false,
            ..Default::default()
        },
        value,
    );
    transaction.insert_text(
        label,
        ElementMeta::default(),
        TextData {
            text: "remeasured label".into(),
            layout: TextLayoutSize::new(70.0, 20.0),
            ..Default::default()
        },
    );
    model.apply_transaction(transaction).unwrap();
    let mut session = EditorSession::new(Default::default()).unwrap();
    let request = session
        .speech_bubble_text_layout_requests(&model)
        .pop()
        .unwrap();
    let constraints = speech_bubble_text_constraints(request.body, 20.0, 3.0, 160.0).unwrap();
    session
        .apply_speech_bubble_text_layouts(
            &model,
            &[(
                label,
                SpeechBubbleTextMeasurement {
                    key: request.key,
                    size: TextLayoutSize::with_content(
                        constraints.content_width,
                        160.0,
                        60.0,
                        160.0,
                    ),
                    minimum_grapheme_width: 20.0,
                    text_ink_reserve: 3.0,
                },
            )],
        )
        .unwrap();
    let mut cache = DocumentSceneCache::new();
    cache.sync(&model, None);
    let view = EditorViewportState {
        surface: frame().surface,
        camera: frame().camera,
        ..Default::default()
    };
    for visible in [false, true, false] {
        let mut transaction = Transaction::new("change bubble visibility");
        transaction.update_element_meta(
            owner,
            ElementMeta {
                visible,
                ..Default::default()
            },
        );
        let delta = model.apply_transaction(transaction).unwrap();
        cache.sync(&model, Some(&delta.changes));
        assert_eq!(model.element(label).unwrap().meta.visible, visible);
        let presentation = session.presentation_state(&model, &view);
        assert_eq!(presentation.speech_bubble_previews.len(), 1);
        assert_eq!(presentation.speech_bubble_text_previews.len(), 1);
        let items = compose_scene_items(&cache, &model, &presentation, frame());
        assert_eq!(items.len(), if visible { 2 } else { 0 });
    }
}

#[test]
fn speech_bubble_new_label_preview_obeys_owner_visibility_and_paint_position() {
    use snow_draw_engine_editor::{ActiveTextDraftPresentation, ActiveTextDraftTarget};
    let owner = ElementId {
        index: 0,
        generation: 1,
    };
    let foreground = ElementId {
        index: 1,
        generation: 1,
    };
    let value = bubble(HighlightShape::Rectangle);
    let mut model = DocumentModel::new();
    let mut transaction = Transaction::new("new bubble label layering");
    transaction.insert_speech_bubble(owner, ElementMeta::default(), value.clone());
    transaction.insert_rectangle(foreground, ElementMeta::default(), value.body);
    model.apply_transaction(transaction).unwrap();
    let mut cache = DocumentSceneCache::new();
    cache.sync(&model, None);
    let draft = ActiveTextDraftPresentation {
        target: ActiveTextDraftTarget::NewSpeechBubble(owner),
        revision: 7,
        text: TextData {
            text: "draft".into(),
            layout: TextLayoutSize::new(60.0, 20.0),
            ..Default::default()
        },
    };
    let draft_id = display_item_id(draft.display_id());
    let presentation = EditorPresentationState {
        active_text_draft: Some(draft),
        ..Default::default()
    };
    let items = compose_scene_items(&cache, &model, &presentation, frame());
    assert!(
        matches!(&items[0], SceneDisplayItem::SpeechBubble(item) if item.body.id == display_item_id(owner))
    );
    assert!(matches!(&items[1], SceneDisplayItem::Text(item) if item.id == draft_id));
    assert!(
        matches!(&items[2], SceneDisplayItem::Rectangle(item) if item.id == display_item_id(foreground))
    );
    compose_scene_render_plan(&cache, &model, &presentation, &items);
    let mut transaction = Transaction::new("hide edited bubble");
    transaction.update_element_meta(
        owner,
        ElementMeta {
            visible: false,
            ..Default::default()
        },
    );
    let delta = model.apply_transaction(transaction).unwrap();
    cache.sync(&model, Some(&delta.changes));
    let items = compose_scene_items(&cache, &model, &presentation, frame());
    assert_eq!(items.len(), 1);
    assert!(
        matches!(&items[0], SceneDisplayItem::Rectangle(item) if item.id == display_item_id(foreground))
    );
    compose_scene_render_plan(&cache, &model, &presentation, &items);
}

#[test]
fn speech_bubble_duplicate_preview_remaps_owned_label_and_reuses_compact_scene() {
    let owner = ElementId {
        index: 100,
        generation: 3,
    };
    let label = ElementId {
        index: 101,
        generation: 3,
    };
    let mut value = bubble(HighlightShape::Ellipse);
    value.text_element_id = Some(label);
    let transaction = |center: Point<f64>| {
        let mut value = value.clone();
        value.body.center = center;
        let mut transaction = Transaction::new("duplicate labeled bubble");
        transaction.insert_speech_bubble(owner, ElementMeta::default(), value);
        transaction.insert_text(
            label,
            ElementMeta::default(),
            TextData {
                text: "copied label".into(),
                layout: TextLayoutSize::new(70.0, 20.0),
                ..Default::default()
            },
        );
        transaction
    };
    let cache = DocumentSceneCache::new();
    let model = DocumentModel::new();
    let mut presentation = EditorPresentationState {
        duplicate_preview: Some(transaction(Point::default())),
        ..Default::default()
    };
    let local_owner = ElementId {
        index: 0,
        generation: 1,
    };
    let local_label = ElementId {
        index: 1,
        generation: 1,
    };
    let copy = duplicate_preview_scene(&cache, &presentation).expect("valid labeled copy preview");
    assert_eq!(
        copy.model.bound_text_id_for_speech_bubble(local_owner),
        Some(local_label)
    );
    assert_eq!(
        copy.model.speech_bubble_id_for_text(local_label),
        Some(local_owner)
    );
    let relation_builds = copy.model.relation_index_build_count();
    let order_builds = copy.cache.order_plan_build_count();
    drop(copy);

    for center in [
        Point::default(),
        Point::new(35.0, 25.0),
        Point::new(-40.0, 15.0),
    ] {
        presentation.duplicate_preview = Some(transaction(center));
        let copy = duplicate_preview_scene(&cache, &presentation).unwrap();
        assert_eq!(
            copy.model.speech_bubble(local_owner).unwrap().body.center,
            center
        );
        assert_eq!(copy.model.text(local_label).unwrap().center, center);
        assert_eq!(copy.model.relation_index_build_count(), relation_builds);
        assert_eq!(copy.cache.order_plan_build_count(), order_builds);
        drop(copy);
        let items = compose_scene_items(&cache, &model, &presentation, frame());
        assert_eq!(items.len(), 2);
        assert!(
            matches!(&items[0], SceneDisplayItem::SpeechBubble(item) if item.body.id == display_item_id(owner) && Point::new(item.body.center_x, item.body.center_y) == center)
        );
        assert!(
            matches!(&items[1], SceneDisplayItem::Text(item) if item.id == display_item_id(label) && Point::new(item.center_x, item.center_y) == center)
        );
    }
}
