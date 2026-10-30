use super::*;
use crate::{ActiveTool, SpeechBubbleTextMeasurement, speech_bubble_text_constraints};
use snow_draw_engine_core::{ColorRgba8, CornerRadii, EngineConfig};
use snow_draw_engine_document::{
    ElementData, ElementMeta, FillStyle, HighlightShape, Operation, RectangleElementKind,
    SpeechBubbleData, StrokeStyle, TextData, TextLayoutSize, Transaction,
    speech_bubble_default_tail,
};
use snow_draw_engine_interaction::{
    InputEvent, Modifiers, PointerButton, PointerButtons, PointerDevice, PointerEvent,
    PointerEventType,
};

fn insert_bubble(document: &mut DocumentModel, center: Point<f64>, labeled: bool) -> ElementId {
    let owner = document.allocate_element_id();
    let label = labeled.then(|| document.allocate_element_id());
    let mut bubble = SpeechBubbleData::new(RectangleData {
        text_element_id: None,
        stroke_rendering_seed: 0,
        stroke_rendering_style: Default::default(),
        rectangle_kind: RectangleElementKind::Rectangle,
        highlight_shape: HighlightShape::Rectangle,
        center,
        width: 120.0,
        height: 60.0,
        rotation: 0.3,
        fill: ColorRgba8 {
            a: 255,
            ..Default::default()
        },
        fill_style: FillStyle::Solid,
        stroke: ColorRgba8 {
            a: 255,
            ..Default::default()
        },
        stroke_width: 2.0,
        stroke_style: StrokeStyle::Solid,
        corner_radii: CornerRadii::splat(6.0),
        opacity: 1.0,
    });
    bubble.text_element_id = label;
    bubble.tails.push(speech_bubble_default_tail(
        &bubble,
        Point::new(0.0, -100.0),
        1.0,
    ));
    let mut transaction = Transaction::new("duplicate bubble fixture");
    transaction.insert_speech_bubble(owner, ElementMeta::default(), bubble);
    if let Some(label) = label {
        transaction.insert_text(
            label,
            ElementMeta::default(),
            TextData {
                text: "wrapped label".into(),
                layout: TextLayoutSize::new(80.0, 20.0),
                ..Default::default()
            },
        );
    }
    document.apply_transaction(transaction).unwrap();
    owner
}

fn measure_labels(editor: &mut Editor, document: &DocumentModel) {
    let layouts: Vec<_> = editor
        .speech_bubble_text_layout_requests(document)
        .into_iter()
        .map(|request| {
            let constraints =
                speech_bubble_text_constraints(request.body, 180.0, 3.0, 140.0).unwrap();
            (
                request.text_id,
                SpeechBubbleTextMeasurement {
                    key: request.key,
                    size: TextLayoutSize::with_content(
                        constraints.content_width,
                        140.0,
                        60.0,
                        140.0,
                    ),
                    minimum_grapheme_width: 180.0,
                    text_ink_reserve: 3.0,
                },
            )
        })
        .collect();
    editor
        .apply_speech_bubble_text_layouts(document, &layouts)
        .unwrap();
}

fn start_copy_drag(editor: &mut Editor, document: &DocumentModel, start: Point<f64>) {
    for (event_type, point) in [
        (PointerEventType::Down, start),
        (
            PointerEventType::Move,
            Point::new(start.x + 35.0, start.y + 25.0),
        ),
    ] {
        editor
            .process_input(
                document,
                InputEvent::Pointer(PointerEvent {
                    pressure: None,
                    timestamp_ms: 0,
                    pointer_id: 1,
                    event_type,
                    device: PointerDevice::Mouse,
                    position: Point::new(point.x + 400.0, point.y + 300.0),
                    button: Some(PointerButton::Primary),
                    buttons: PointerButtons(PointerButtons::PRIMARY),
                    modifiers: Modifiers {
                        alt: true,
                        ..Default::default()
                    },
                }),
            )
            .unwrap();
    }
}

#[test]
fn speech_bubble_duplicate_preview_keeps_unlabeled_original_stationary() {
    let mut document = DocumentModel::new();
    let owner = insert_bubble(&mut document, Point::default(), false);
    let original = document.speech_bubble(owner).unwrap().clone();
    let mut config = EngineConfig::default();
    config.snap.enabled = false;
    let mut editor = Editor::new(config).unwrap();
    editor.set_surface_size(800, 600).unwrap();
    editor.set_active_tool(ActiveTool::Select).unwrap();
    editor.select_element(&document, owner).unwrap();
    start_copy_drag(&mut editor, &document, original.body.center);

    let presentation = editor.presentation_state(&document);
    assert!(presentation.speech_bubble_previews.is_empty());
    let transaction = presentation.duplicate_preview.unwrap();
    let copy = transaction
        .operations()
        .iter()
        .find_map(|operation| match operation {
            Operation::InsertElement {
                data: ElementData::SpeechBubble(copy),
                ..
            } => Some(copy),
            _ => None,
        })
        .unwrap();
    assert_eq!(copy.body.center, Point::new(35.0, 25.0));
    assert_eq!(copy.tails, original.tails);
    assert_eq!(document.speech_bubble(owner).unwrap(), &original);
}

#[test]
fn speech_bubble_duplicate_preview_preserves_stationary_measured_body_and_label_geometry() {
    let mut document = DocumentModel::new();
    let owner = insert_bubble(&mut document, Point::new(-120.0, -40.0), true);
    let other = insert_bubble(&mut document, Point::new(160.0, -40.0), true);
    let mut config = EngineConfig::default();
    config.snap.enabled = false;
    let mut editor = Editor::new(config).unwrap();
    editor.set_surface_size(800, 600).unwrap();
    editor.set_active_tool(ActiveTool::Select).unwrap();
    measure_labels(&mut editor, &document);
    let expected = editor.presentation_state(&document);
    assert_eq!(expected.speech_bubble_previews.len(), 2);
    assert_eq!(expected.speech_bubble_text_previews.len(), 2);
    let start = expected
        .speech_bubble_previews
        .iter()
        .find(|(id, _)| *id == owner)
        .unwrap()
        .1
        .body
        .center;
    editor.select_element(&document, owner).unwrap();
    start_copy_drag(&mut editor, &document, start);
    measure_labels(&mut editor, &document);
    editor.invalidate_arrow_text_measurements();
    let requests = editor.speech_bubble_text_layout_requests(&document);
    assert_eq!(requests.len(), 2);
    for request in &requests {
        assert_eq!(
            request.body,
            document.speech_bubble(request.owner_id).unwrap().body
        );
    }
    measure_labels(&mut editor, &document);

    let actual = editor.presentation_state(&document);
    assert!(actual.duplicate_preview.is_some());
    for id in [owner, other] {
        assert_eq!(
            actual
                .speech_bubble_previews
                .iter()
                .find(|(owner, _)| *owner == id),
            expected
                .speech_bubble_previews
                .iter()
                .find(|(owner, _)| *owner == id),
        );
        let label = document.bound_text_id_for_speech_bubble(id).unwrap();
        assert_eq!(
            actual
                .speech_bubble_text_previews
                .iter()
                .find(|(id, _)| *id == label),
            expected
                .speech_bubble_text_previews
                .iter()
                .find(|(id, _)| *id == label),
        );
    }
}

#[test]
fn speech_bubble_duplicate_preview_matches_measured_source_appearance() {
    let mut document = DocumentModel::new();
    let owner = insert_bubble(&mut document, Point::default(), true);
    let label = document.bound_text_id_for_speech_bubble(owner).unwrap();
    let mut config = EngineConfig::default();
    config.snap.enabled = false;
    let mut editor = Editor::new(config).unwrap();
    editor.set_surface_size(800, 600).unwrap();
    editor.set_active_tool(ActiveTool::Select).unwrap();
    measure_labels(&mut editor, &document);
    let expected = editor.presentation_state(&document);
    let original = &expected
        .speech_bubble_previews
        .iter()
        .find(|(id, _)| *id == owner)
        .unwrap()
        .1;
    let original_text = &expected
        .speech_bubble_text_previews
        .iter()
        .find(|(id, _)| *id == label)
        .unwrap()
        .1;
    editor.select_element(&document, owner).unwrap();
    start_copy_drag(&mut editor, &document, original.body.center);

    let actual = editor.presentation_state(&document);
    let transaction = actual.duplicate_preview.unwrap();
    let copied = transaction
        .operations()
        .iter()
        .find_map(|operation| match operation {
            Operation::InsertElement {
                data: ElementData::SpeechBubble(copy),
                ..
            } => Some(copy),
            _ => None,
        })
        .unwrap();
    assert_eq!(copied.body.width, original.body.width);
    assert_eq!(copied.body.height, original.body.height);
    assert_eq!(copied.tails, original.tails);
    assert_eq!(
        copied.body.center,
        Point::new(original.body.center.x + 35.0, original.body.center.y + 25.0)
    );
    let copied_text = transaction
        .operations()
        .iter()
        .find_map(|operation| match operation {
            Operation::InsertElement {
                data: ElementData::Text(copy),
                ..
            } => Some(copy),
            _ => None,
        })
        .unwrap();
    assert_eq!(copied_text.layout, original_text.layout);
    assert_eq!(copied_text.text_ink_reserve, original_text.text_ink_reserve);
    assert_eq!(copied_text.center, copied.body.center);
    let target = copied.body.center;
    let update = editor
        .process_input(
            &document,
            InputEvent::Pointer(PointerEvent {
                pressure: None,
                timestamp_ms: 0,
                pointer_id: 1,
                event_type: PointerEventType::Up,
                device: PointerDevice::Mouse,
                position: Point::new(target.x + 400.0, target.y + 300.0),
                button: Some(PointerButton::Primary),
                buttons: PointerButtons::default(),
                modifiers: Modifiers {
                    alt: true,
                    ..Default::default()
                },
            }),
        )
        .unwrap();
    let Some(crate::EditorCommand::ApplyTransaction(command)) = update.command else {
        panic!("copy drag commits its preview");
    };
    assert_eq!(command.transaction, transaction);
    document.apply_transaction(command.transaction).unwrap();
    let copied_owner = editor.selected_ids()[0];
    let committed = document.speech_bubble(copied_owner).unwrap();
    assert_eq!(committed, copied);
    assert_eq!(
        document.text(committed.text_element_id.unwrap()).unwrap(),
        copied_text
    );
}

#[test]
fn border_fill_invalidates_bound_text_measurement_without_changing_color() {
    let mut document = DocumentModel::new();
    let owner = insert_bubble(&mut document, Point::new(0.0, 0.0), true);
    let label = document
        .speech_bubble(owner)
        .unwrap()
        .text_element_id
        .unwrap();
    let mut text = document.text(label).unwrap().clone();
    text.fill = ColorRgba8 {
        r: 255,
        g: 204,
        b: 199,
        a: 255,
    };
    text.fill_style = FillStyle::Solid;
    let mut change = Transaction::new("filled label");
    change.update_text(label, text.clone());
    document.apply_transaction(change).unwrap();
    let mut editor = Editor::new(EngineConfig::default()).unwrap();
    measure_labels(&mut editor, &document);
    assert!(
        editor
            .speech_bubble_text_layout_requests(&document)
            .is_empty()
    );

    text.fill_style = FillStyle::Border;
    let mut change = Transaction::new("border label");
    change.update_text(label, text.clone());
    document.apply_transaction(change).unwrap();
    let requests = editor.speech_bubble_text_layout_requests(&document);
    assert!(requests.iter().any(|request| request.text_id == label));
    assert_eq!(document.text(label).unwrap().fill, text.fill);
    measure_labels(&mut editor, &document);
    assert!(
        editor
            .speech_bubble_text_layout_requests(&document)
            .is_empty()
    );
}
