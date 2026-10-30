use super::*;
use snow_draw_engine_core::CornerRadii;
use snow_draw_engine_display::{OverlayDisplayItem, UiShapeKind};
use snow_draw_engine_document::{HighlightShape, RectangleData};
use snow_draw_engine_interaction::{
    Modifiers, PointerButton, PointerButtons, PointerDevice, PointerEvent, PointerEventType,
};

fn pointer(
    engine: &mut Engine,
    viewport: ViewportId,
    kind: PointerEventType,
    point: Point<f64>,
    shift: bool,
) {
    engine
        .process_input_with_viewport_changes(
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
                modifiers: Modifiers {
                    shift,
                    ..Default::default()
                },
            }),
        )
        .unwrap();
}

fn fixture(tool: ActiveTool) -> (Engine, ViewportId, ElementId) {
    fixture_with_radius(tool, 0.0)
}

fn fixture_with_radius(tool: ActiveTool, radius: f64) -> (Engine, ViewportId, ElementId) {
    let mut config = EngineConfig::default();
    config.style_defaults.editor.rectangle.shape = HighlightShape::Diamond;
    config.style_defaults.editor.rectangle.corner_radii = CornerRadii::splat(radius);
    config.style_defaults.editor.spotlight_shape = HighlightShape::Diamond;
    config.style_defaults.editor.spotlight_corner_radii = CornerRadii::splat(radius);
    config.style_defaults.editor.magnifier.shape = HighlightShape::Diamond;
    config.style_defaults.editor.magnifier.corner_radii = CornerRadii::splat(radius);
    let mut engine = Engine::new(config);
    let viewport = engine.create_viewport(ViewportConfig::default()).unwrap();
    engine
        .set_viewport_surface_size(viewport, 800, 600)
        .unwrap();
    engine.set_viewport_active_tool(viewport, tool).unwrap();
    for (kind, point) in [
        (PointerEventType::Down, Point::new(-120.0, -80.0)),
        (PointerEventType::Move, Point::new(120.0, 80.0)),
        (PointerEventType::Up, Point::new(120.0, 80.0)),
    ] {
        pointer(&mut engine, viewport, kind, point, false);
    }
    let id = engine.model.paint_order()[0];
    engine
        .set_viewport_active_tool(viewport, ActiveTool::Select)
        .unwrap();
    engine
        .select_element_with_viewport_changes(viewport, id)
        .unwrap();
    (engine, viewport, id)
}

fn proxy(engine: &Engine, id: ElementId) -> RectangleData {
    engine.model.document().element_rect_proxy(id).unwrap()
}

#[test]
fn rounded_diamond_all_handles_commit_cancel_and_preserve_history_for_every_tool() {
    for tool in [
        ActiveTool::Shape,
        ActiveTool::Spotlight,
        ActiveTool::Magnifier,
    ] {
        for index in 0..4 {
            let (mut engine, viewport, id) = fixture(tool);
            let before = proxy(&engine, id);
            let patch = engine.acquire_patch(viewport, None).unwrap();
            let handles: Vec<_> = patch
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
                .collect();
            assert_eq!(handles.len(), 4, "{tool:?}");
            let from = handles[index];
            let expected = snow_draw_engine_document::diamond_corner_handle_local_point(
                before.width,
                before.height,
                before.corner_radii,
                1.0,
                index,
            );
            assert!((from.x - expected.x - before.center.x).abs() < 1e-9);
            assert!((from.y - expected.y - before.center.y).abs() < 1e-9);
            let inward = [
                Point::new(0.0, 18.0),
                Point::new(-18.0, 0.0),
                Point::new(0.0, -18.0),
                Point::new(18.0, 0.0),
            ][index];
            let to = Point::new(from.x + inward.x, from.y + inward.y);
            pointer(&mut engine, viewport, PointerEventType::Down, from, false);
            pointer(&mut engine, viewport, PointerEventType::Move, to, false);
            engine
                .process_input_with_viewport_changes(viewport, InputEvent::FocusLost)
                .unwrap();
            assert_eq!(proxy(&engine, id).corner_radii, before.corner_radii);
            for kind in [
                PointerEventType::Down,
                PointerEventType::Move,
                PointerEventType::Up,
            ] {
                pointer(
                    &mut engine,
                    viewport,
                    kind,
                    if kind == PointerEventType::Down {
                        from
                    } else {
                        to
                    },
                    false,
                );
            }
            let after = proxy(&engine, id);
            let values = [
                after.corner_radii.top_left,
                after.corner_radii.top_right,
                after.corner_radii.bottom_right,
                after.corner_radii.bottom_left,
            ];
            assert!(values[index] > 0.0, "{tool:?}: tip {index}");
            assert!(
                values
                    .iter()
                    .enumerate()
                    .all(|(i, r)| i == index || *r == 0.0)
            );
            engine.undo_with_viewport_changes().unwrap();
            assert_eq!(proxy(&engine, id).corner_radii, before.corner_radii);
            engine.redo_with_viewport_changes().unwrap();
            assert_eq!(proxy(&engine, id).corner_radii, after.corner_radii);
            let bytes = engine.serialize_document_session().unwrap();
            let restored = Engine::from_serialized_document_session_with_config(
                &bytes,
                EngineConfig::default(),
            )
            .unwrap();
            assert_eq!(proxy(&restored, id).corner_radii, after.corner_radii);
            engine
                .duplicate_selected_with_viewport_changes(viewport, Point::new(280.0, 0.0))
                .unwrap();
            let duplicate = *engine.model.paint_order().last().unwrap();
            assert_eq!(proxy(&engine, duplicate).corner_radii, after.corner_radii);
            let before_resize = proxy(&engine, duplicate);
            let from = Point::new(
                before_resize.center.x - before_resize.width / 2.0 - 4.0,
                before_resize.center.y - before_resize.height / 2.0 - 4.0,
            );
            let to = Point::new(from.x + 60.0, from.y + 40.0);
            for kind in [
                PointerEventType::Down,
                PointerEventType::Move,
                PointerEventType::Up,
            ] {
                pointer(
                    &mut engine,
                    viewport,
                    kind,
                    if kind == PointerEventType::Down {
                        from
                    } else {
                        to
                    },
                    false,
                );
            }
            let resized = proxy(&engine, duplicate);
            assert!(
                resized.width < before_resize.width && resized.height < before_resize.height,
                "{tool:?}"
            );
            assert_eq!(resized.highlight_shape, HighlightShape::Diamond);
            let normalized = snow_draw_engine_document::normalize_shape_corner_radii(
                resized.highlight_shape,
                resized.width,
                resized.height,
                resized.corner_radii,
            );
            assert_eq!(normalized, resized.corner_radii);
            engine.undo_with_viewport_changes().unwrap();
            assert_eq!(
                proxy(&engine, duplicate).corner_radii,
                before_resize.corner_radii
            );
        }
    }
}

#[test]
fn rounded_diamond_shift_drag_updates_all_tips_for_every_tool() {
    for tool in [
        ActiveTool::Shape,
        ActiveTool::Spotlight,
        ActiveTool::Magnifier,
    ] {
        let (mut engine, viewport, id) = fixture(tool);
        let rect = proxy(&engine, id);
        let from = snow_draw_engine_document::diamond_corner_handle_local_point(
            rect.width,
            rect.height,
            rect.corner_radii,
            1.0,
            0,
        );
        let to = Point::new(from.x, from.y + 18.0);
        for kind in [
            PointerEventType::Down,
            PointerEventType::Move,
            PointerEventType::Up,
        ] {
            pointer(
                &mut engine,
                viewport,
                kind,
                if kind == PointerEventType::Down {
                    from
                } else {
                    to
                },
                true,
            );
        }
        let radii = proxy(&engine, id).corner_radii;
        assert!(radii.top_left > 0.0);
        assert_eq!(radii, CornerRadii::splat(radii.top_left));
        for shape in [HighlightShape::Ellipse, HighlightShape::Diamond] {
            if tool == ActiveTool::Magnifier {
                let mut style = snow_draw_engine_editor::MagnifierStyle::from_data(
                    engine.model.magnifier(id).unwrap(),
                );
                style.shape = shape;
                engine
                    .set_viewport_magnifier_style_patch(
                        viewport,
                        style,
                        snow_draw_engine_editor::MAGNIFIER_STYLE_PROPERTY_SHAPE,
                        false,
                    )
                    .unwrap();
            } else {
                let mut style = engine
                    .viewport_style_toolbar_state(viewport)
                    .unwrap()
                    .shape_style;
                style.shape = shape;
                engine
                    .set_viewport_shape_style_patch(
                        viewport,
                        ShapeStylePatch {
                            kind: if tool == ActiveTool::Spotlight {
                                snow_draw_engine_editor::ShapeKind::Spotlight
                            } else {
                                snow_draw_engine_editor::ShapeKind::Rectangle
                            },
                            style,
                            properties: snow_draw_engine_editor::SHAPE_STYLE_PROPERTY_SHAPE,
                        },
                    )
                    .unwrap();
            }
            let changed = proxy(&engine, id);
            assert_eq!(changed.highlight_shape, shape);
            assert_eq!(changed.corner_radii, radii);
        }
    }
}

#[test]
fn rounded_diamond_creation_clamps_large_defaults_for_every_tool() {
    for tool in [
        ActiveTool::Shape,
        ActiveTool::Spotlight,
        ActiveTool::Magnifier,
    ] {
        let (engine, _, id) = fixture_with_radius(tool, 1000.0);
        let rect = proxy(&engine, id);
        let expected =
            snow_draw_engine_document::diamond_uniform_corner_radius_limit(rect.width, rect.height);
        assert!((rect.corner_radii.top_left - expected).abs() < 1e-9);
        assert_eq!(
            rect.corner_radii,
            CornerRadii::splat(rect.corner_radii.top_left)
        );
    }
}
