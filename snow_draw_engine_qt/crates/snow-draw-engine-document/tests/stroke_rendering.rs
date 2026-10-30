use snow_draw_engine_core::arrow::{ArrowType, StrokeRenderingStyle, StrokeStyle};
use snow_draw_engine_core::{ColorRgba8, CornerRadii, Point};
use snow_draw_engine_document::{
    ArrowData, FillStyle, HighlightShape, RectangleData, RectangleElementKind, arrow_bounds,
    arrow_hit_test, arrow_paint_bounds, rect_bounds, rect_paint_bounds, rectangle_hit_test,
};

fn rectangle() -> RectangleData {
    RectangleData {
        text_element_id: None,
        rectangle_kind: RectangleElementKind::Rectangle,
        highlight_shape: HighlightShape::Rectangle,
        center: Point::default(),
        width: 100.0,
        height: 80.0,
        rotation: 0.0,
        fill: ColorRgba8::default(),
        fill_style: FillStyle::Solid,
        stroke: ColorRgba8 {
            r: 30,
            g: 50,
            b: 70,
            a: 255,
        },
        stroke_width: 2.0,
        stroke_style: StrokeStyle::Solid,
        stroke_rendering_style: StrokeRenderingStyle::Architect,
        stroke_rendering_seed: 0,
        corner_radii: CornerRadii::default(),
        opacity: 1.0,
    }
}

#[test]
fn stroke_rendering_bounds_and_hits_expand_without_changing_logical_shape() {
    let mut rect = rectangle();
    let logical = rect_bounds(&rect);
    assert_eq!(rect_paint_bounds(&rect), logical);
    assert!(!rectangle_hit_test(&rect, Point::new(53.0, 0.0), 0.0));
    rect.stroke_rendering_style = StrokeRenderingStyle::Cartoonist;
    rect.stroke_rendering_seed = 99;
    assert_eq!(rect_bounds(&rect), logical);
    let paint = rect_paint_bounds(&rect);
    assert_eq!(paint.min_x, logical.min_x - 4.0);
    assert_eq!(paint.max_y, logical.max_y + 4.0);
    assert!(rectangle_hit_test(&rect, Point::new(53.0, 0.0), 0.0));
    rect.stroke.a = 0;
    assert_eq!(rect_paint_bounds(&rect), logical);
}

#[test]
fn stroke_rendering_arrow_bounds_and_reconstruction_preserve_appearance() {
    let mut arrow = ArrowData::from_global_points(
        &[Point::new(0.0, 0.0), Point::new(100.0, 0.0)],
        rectangle().stroke,
        2.0,
        StrokeStyle::Dashed,
        ArrowType::Straight,
        None,
        None,
    )
    .unwrap();
    let logical = arrow_bounds(&arrow);
    assert!(!arrow_hit_test(&arrow, Point::new(50.0, 3.0), 0.0));
    arrow.stroke_rendering_style = StrokeRenderingStyle::Artist;
    arrow.stroke_rendering_seed = 12345;
    assert_eq!(arrow_bounds(&arrow), logical);
    assert_eq!(arrow_paint_bounds(&arrow).min_y, logical.min_y - 2.0);
    assert!(arrow_hit_test(&arrow, Point::new(50.0, 3.0), 0.0));
    let mut rebuilt = ArrowData::from_global_points(
        &arrow.global_points(),
        arrow.stroke,
        3.0,
        arrow.stroke_style,
        ArrowType::Curve,
        None,
        None,
    )
    .unwrap();
    rebuilt.inherit_linear_metadata_from(&arrow);
    assert_eq!(rebuilt.stroke_rendering_style, arrow.stroke_rendering_style);
    assert_eq!(rebuilt.stroke_rendering_seed, arrow.stroke_rendering_seed);
}

#[test]
fn stroke_rendering_json_is_backward_compatible_and_preserves_seed_zero() {
    let rect = rectangle();
    let mut legacy = serde_json::to_value(rect).unwrap();
    legacy
        .as_object_mut()
        .unwrap()
        .remove("stroke_rendering_style");
    legacy
        .as_object_mut()
        .unwrap()
        .remove("stroke_rendering_seed");
    let restored: RectangleData = serde_json::from_value(legacy).unwrap();
    assert_eq!(
        restored.stroke_rendering_style,
        StrokeRenderingStyle::Architect
    );
    assert_eq!(restored.stroke_rendering_seed, 0);
    let mut styled = rect;
    styled.stroke_rendering_style = StrokeRenderingStyle::Cartoonist;
    let value = serde_json::to_value(styled).unwrap();
    assert_eq!(value["stroke_rendering_style"], "cartoonist");
    assert_eq!(
        serde_json::from_value::<RectangleData>(value).unwrap(),
        styled
    );
    let mut bad = serde_json::to_value(styled).unwrap();
    bad["stroke_rendering_style"] = "unknown".into();
    assert!(serde_json::from_value::<RectangleData>(bad).is_err());
}

#[test]
fn serial_number_stroke_rendering_preserves_geometry_and_legacy_documents() {
    use snow_draw_engine_document::{
        SerialNumberData, SerialNumberType, serial_number_bounds, serial_number_hit_test,
        serial_number_paint_bounds,
    };
    for kind in [
        SerialNumberType::OutlinedCircle,
        SerialNumberType::OutlinedSquare,
        SerialNumberType::SolidCircle,
        SerialNumberType::SolidSquare,
        SerialNumberType::Circle,
    ] {
        let mut serial = SerialNumberData {
            serial_number_type: kind,
            ..Default::default()
        };
        let logical = serial_number_bounds(&serial);
        serial.stroke_rendering_style = StrokeRenderingStyle::Cartoonist;
        serial.stroke_rendering_seed = 12345;
        assert_eq!(serial_number_bounds(&serial), logical);
        let outlined = matches!(
            kind,
            SerialNumberType::OutlinedCircle | SerialNumberType::OutlinedSquare
        );
        let paint = serial_number_paint_bounds(&serial);
        assert_eq!(
            paint.min_x,
            logical.min_x - if outlined { 4.0 } else { 0.0 }
        );
        assert_eq!(
            serial_number_hit_test(&serial, Point::new(logical.max_x + 2.0, 0.0), 0.0),
            outlined
        );
        let value = serde_json::to_value(&serial).unwrap();
        assert_eq!(
            serde_json::from_value::<SerialNumberData>(value.clone()).unwrap(),
            serial
        );
        let mut legacy = value;
        legacy
            .as_object_mut()
            .unwrap()
            .remove("stroke_rendering_style");
        legacy
            .as_object_mut()
            .unwrap()
            .remove("stroke_rendering_seed");
        let restored: SerialNumberData = serde_json::from_value(legacy).unwrap();
        assert_eq!(
            restored.stroke_rendering_style,
            StrokeRenderingStyle::Architect
        );
        assert_eq!(restored.stroke_rendering_seed, 0);
    }
}
