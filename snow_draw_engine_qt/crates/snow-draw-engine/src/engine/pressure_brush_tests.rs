use super::*;
use snow_draw_engine_core::{BrushMode, PressureKnot};
use snow_draw_engine_display::PathGeometryRole;
use snow_draw_engine_editor::{SHAPE_STYLE_PROPERTY_BRUSH_MODE, ShapeKind};
use snow_draw_engine_interaction::{
    PointerButton, PointerButtons, PointerDevice, PointerEvent, PointerEventType,
};

fn stroke(mode: BrushMode) -> (Engine, ViewportId, ElementId) {
    let mut config = EngineConfig::default();
    config.style_defaults.editor.free_draw.brush_mode = mode;
    config.style_defaults.editor.free_draw.stroke_width = 12.0;
    let mut engine = Engine::new(config);
    let view = engine.create_viewport(ViewportConfig::default()).unwrap();
    engine.set_viewport_surface_size(view, 800, 600).unwrap();
    engine
        .set_viewport_snap_config(
            view,
            SnapConfig {
                enabled: false,
                ..Default::default()
            },
        )
        .unwrap();
    engine
        .set_viewport_active_tool(view, ActiveTool::FreeDraw)
        .unwrap();
    for i in 0..=31 {
        let kind = if i == 0 {
            PointerEventType::Down
        } else if i == 31 {
            PointerEventType::Up
        } else {
            PointerEventType::Move
        };
        engine
            .process_input(
                view,
                InputEvent::Pointer(PointerEvent {
                    pointer_id: 1,
                    event_type: kind,
                    device: PointerDevice::Pen,
                    position: Point::new(100.0 + i.min(30) as f64 * 4.0, 100.0),
                    button: Some(PointerButton::Primary),
                    buttons: PointerButtons(PointerButtons::PRIMARY),
                    modifiers: Default::default(),
                    pressure: Some(if i == 31 {
                        0.0
                    } else if (10..=20).contains(&i) {
                        0.9
                    } else {
                        0.2
                    }),
                    timestamp_ms: 100 + i * 8,
                }),
            )
            .unwrap();
    }
    let id = engine.model.paint_order()[0];
    (engine, view, id)
}

#[test]
fn pressure_brush_mode_edit_is_undoable_and_preserves_canonical_geometry() {
    let (mut engine, view, id) = stroke(BrushMode::Regular);
    let original = engine.model.free_draw(id).unwrap().clone();
    engine
        .select_element_with_viewport_changes(view, id)
        .unwrap();
    let mut style = engine
        .viewport_style_toolbar_state(view)
        .unwrap()
        .shape_style;
    style.brush_mode = BrushMode::PressureSensitive;
    engine
        .set_viewport_shape_style_patch(
            view,
            ShapeStylePatch {
                kind: ShapeKind::FreeDraw,
                style,
                properties: SHAPE_STYLE_PROPERTY_BRUSH_MODE,
            },
        )
        .unwrap();
    let pressure = engine.model.free_draw(id).unwrap();
    assert_eq!(pressure.brush_mode, BrushMode::PressureSensitive);
    assert_eq!(pressure.vertices, original.vertices);
    assert_eq!(pressure.pressure_segments, original.pressure_segments);
    let patch = engine.acquire_patch(view, None).unwrap();
    assert!(
        patch
            .path_geometry_ops
            .iter()
            .any(|op| op.role == PathGeometryRole::PressureOutline)
    );
    engine.undo().unwrap();
    assert_eq!(engine.model.free_draw(id).unwrap(), &original);
    engine.redo().unwrap();
    assert_eq!(
        engine.model.free_draw(id).unwrap().brush_mode,
        BrushMode::PressureSensitive
    );
}

#[test]
fn pressure_brush_profiles_round_trip_session_history_and_template() {
    let (mut engine, view, id) = stroke(BrushMode::PressureSensitive);
    let original = engine.model.free_draw(id).unwrap().clone();
    for history_only in [false, true] {
        let bytes = if history_only {
            engine.serialize_document_history()
        } else {
            engine.serialize_document_session()
        }
        .unwrap();
        let restored = if history_only {
            Engine::from_serialized_document_history_with_config(&bytes, Default::default())
        } else {
            Engine::from_serialized_document_session_with_config(&bytes, Default::default())
        }
        .unwrap();
        assert_eq!(restored.model.free_draw(id).unwrap(), &original);
    }
    engine
        .select_element_with_viewport_changes(view, id)
        .unwrap();
    let template = engine.serialize_selected_draw_template().unwrap();
    let decoded: snow_draw_engine_editor::DrawTemplate = serde_json::from_slice(&template).unwrap();
    assert!(
        serde_json::to_string(&decoded)
            .unwrap()
            .contains("pressure_segments")
    );
}

#[test]
fn pressure_brush_legacy_payloads_default_to_regular_without_profiles() {
    fn strip(value: &mut serde_json::Value) {
        match value {
            serde_json::Value::Object(fields) => {
                fields.remove("brush_mode");
                fields.remove("pressure_segments");
                for child in fields.values_mut() {
                    strip(child);
                }
            }
            serde_json::Value::Array(items) => {
                for child in items {
                    strip(child);
                }
            }
            _ => {}
        }
    }
    let (engine, _, id) = stroke(BrushMode::Regular);
    for history_only in [false, true] {
        let bytes = if history_only {
            engine.serialize_document_history()
        } else {
            engine.serialize_document_session()
        }
        .unwrap();
        let mut legacy: serde_json::Value = serde_json::from_slice(&bytes).unwrap();
        legacy["schemaVersion"] = serde_json::Value::from(if history_only { 9 } else { 13 });
        strip(&mut legacy);
        let bytes = serde_json::to_vec(&legacy).unwrap();
        let mut restored = if history_only {
            Engine::from_serialized_document_history_with_config(&bytes, Default::default())
        } else {
            Engine::from_serialized_document_session_with_config(&bytes, Default::default())
        }
        .unwrap();
        let data = restored.model.free_draw(id).unwrap();
        assert_eq!(data.brush_mode, BrushMode::Regular);
        assert!(data.pressure_segments.is_empty());
        restored.undo().unwrap();
        restored.redo().unwrap();
        assert!(
            restored
                .model
                .free_draw(id)
                .unwrap()
                .pressure_segments
                .is_empty()
        );
    }
}

#[test]
fn pressure_brush_profiles_reject_invalid_parameters_and_own_dash_gaps() {
    let (engine, _, id) = stroke(BrushMode::PressureSensitive);
    let mut data = engine.model.free_draw(id).unwrap().clone();
    data.pressure_segments[0].push(PressureKnot {
        t: 0.5,
        pressure: f64::NAN,
    });
    assert!(snow_draw_engine_document::validate_free_draw(&data).is_err());
    let mut data = engine.model.free_draw(id).unwrap().clone();
    data.stroke_style = snow_draw_engine_document::StrokeStyle::Dotted;
    let points = data.global_vertices();
    let a = points[0];
    assert!(snow_draw_engine_document::free_draw_hit_test(&data, a, 0.0));
    assert!(!snow_draw_engine_document::free_draw_hit_test(
        &data,
        Point::new(a.x + 12.0, a.y),
        0.0
    ));
}
