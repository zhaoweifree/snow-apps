use crate::{HighlightShape, normalize_corner_radii};
use snow_draw_engine_core::{CornerRadii, Point};

/// Diamond tips use the existing corner fields in clockwise order: top, right, bottom, left.
pub fn normalize_shape_corner_radii(
    shape: HighlightShape,
    width: f64,
    height: f64,
    radii: CornerRadii,
) -> CornerRadii {
    let radii = normalize_corner_radii(width, height, radii);
    if shape != HighlightShape::Diamond || width <= 0.0 || height <= 0.0 {
        return radii;
    }
    let a = width / 2.0;
    let b = height / 2.0;
    let edge = a.hypot(b);
    let trims = [
        radii.top_left * (b / a),
        radii.top_right * (a / b),
        radii.bottom_right * (b / a),
        radii.bottom_left * (a / b),
    ];
    let mut scale = 1.0_f64;
    for index in 0..4 {
        let sum = trims[index] + trims[(index + 1) % 4];
        if sum > edge {
            scale = scale.min(edge / sum);
        }
    }
    radii.scaled(scale)
}

#[derive(Clone, Copy, Debug)]
struct Corner {
    entry: Point<f64>,
    exit: Point<f64>,
    center: Point<f64>,
    radius: f64,
}

fn cross(a: Point<f64>, b: Point<f64>) -> f64 {
    a.x * b.y - a.y * b.x
}

fn subtract(a: Point<f64>, b: Point<f64>) -> Point<f64> {
    Point::new(a.x - b.x, a.y - b.y)
}

impl Corner {
    fn in_arc_sector(self, point: Point<f64>) -> bool {
        let direction = |point: Point<f64>| {
            let vector = subtract(point, self.center);
            let scale = vector.x.abs().max(vector.y.abs()).max(f64::MIN_POSITIVE);
            Point::new(vector.x / scale, vector.y / scale)
        };
        let start = direction(self.entry);
        let end = direction(self.exit);
        let point = direction(point);
        let epsilon = f64::EPSILON * 16.0;
        cross(start, point) >= -epsilon && cross(point, end) >= -epsilon
    }
}

/// The contour is shared by picking and magnifier leader placement.
pub struct RoundedDiamond {
    half_width: f64,
    half_height: f64,
    corners: [Corner; 4],
}

impl RoundedDiamond {
    pub fn path_commands(
        &self,
        center: Point<f64>,
        rotation: f64,
    ) -> Vec<snow_draw_engine_core::arrow::ArrowPathCommand> {
        use snow_draw_engine_core::arrow::ArrowPathCommand;
        if self.half_width <= 0.0 || self.half_height <= 0.0 {
            return Vec::new();
        }
        let (sin, cos) = rotation.sin_cos();
        let map = |point: Point<f64>| {
            [
                center.x + point.x * cos - point.y * sin,
                center.y + point.x * sin + point.y * cos,
            ]
        };
        let mut commands = vec![ArrowPathCommand::MoveTo {
            point: map(self.corners[0].entry),
        }];
        for (index, corner) in self.corners.iter().enumerate() {
            if index != 0 {
                commands.push(ArrowPathCommand::LineTo {
                    point: map(corner.entry),
                });
            }
            if corner.radius <= 0.0 {
                continue;
            }
            let start = Point::new(
                (corner.entry.x - corner.center.x) / corner.radius,
                (corner.entry.y - corner.center.y) / corner.radius,
            );
            let end = Point::new(
                (corner.exit.x - corner.center.x) / corner.radius,
                (corner.exit.y - corner.center.y) / corner.radius,
            );
            let angle = start.y.atan2(start.x);
            let sweep = cross(start, end)
                .atan2(start.x * end.x + start.y * end.y)
                .max(0.0);
            let steps = (sweep / std::f64::consts::FRAC_PI_2 - 1e-12)
                .ceil()
                .max(1.0) as usize;
            let delta = sweep / steps as f64;
            let k = 4.0 / 3.0 * (delta / 4.0).tan();
            for step in 0..steps {
                let (s0, c0) = (angle + step as f64 * delta).sin_cos();
                let (s1, c1) = (angle + (step + 1) as f64 * delta).sin_cos();
                let r = corner.radius;
                commands.push(ArrowPathCommand::CubicTo {
                    control_1: map(Point::new(
                        corner.center.x + r * (c0 - k * s0),
                        corner.center.y + r * (s0 + k * c0),
                    )),
                    control_2: map(Point::new(
                        corner.center.x + r * (c1 + k * s1),
                        corner.center.y + r * (s1 - k * c1),
                    )),
                    end: map(Point::new(
                        corner.center.x + r * c1,
                        corner.center.y + r * s1,
                    )),
                });
            }
        }
        commands.push(ArrowPathCommand::LineTo {
            point: map(self.corners[0].entry),
        });
        commands
    }

    pub fn new(width: f64, height: f64, radii: CornerRadii) -> Self {
        let a = (width / 2.0).max(0.0);
        let b = (height / 2.0).max(0.0);
        let radii = normalize_shape_corner_radii(HighlightShape::Diamond, width, height, radii);
        let vertices = [
            Point::new(0.0, -b),
            Point::new(a, 0.0),
            Point::new(0.0, b),
            Point::new(-a, 0.0),
        ];
        let values = [
            radii.top_left,
            radii.top_right,
            radii.bottom_right,
            radii.bottom_left,
        ];
        let edge = a.hypot(b);
        let corners = std::array::from_fn(|index| {
            let vertex = vertices[index];
            let radius = values[index];
            if a <= 0.0 || b <= 0.0 || radius <= 0.0 {
                return Corner {
                    entry: vertex,
                    exit: vertex,
                    center: vertex,
                    radius: 0.0,
                };
            }
            let (cotangent, sine, inward) = if index.is_multiple_of(2) {
                (
                    b / a,
                    a / edge,
                    Point::new(0.0, if index == 0 { 1.0 } else { -1.0 }),
                )
            } else {
                (
                    a / b,
                    b / edge,
                    Point::new(if index == 1 { -1.0 } else { 1.0 }, 0.0),
                )
            };
            let trim = radius * cotangent / edge;
            let toward = |other: Point<f64>| {
                Point::new(
                    vertex.x + (other.x - vertex.x) * trim,
                    vertex.y + (other.y - vertex.y) * trim,
                )
            };
            Corner {
                entry: toward(vertices[(index + 3) % 4]),
                exit: toward(vertices[(index + 1) % 4]),
                center: Point::new(
                    vertex.x + inward.x * radius / sine,
                    vertex.y + inward.y * radius / sine,
                ),
                radius,
            }
        });
        Self {
            half_width: a,
            half_height: b,
            corners,
        }
    }

    pub fn contains(&self, point: Point<f64>) -> bool {
        if !point.x.is_finite()
            || !point.y.is_finite()
            || self.half_width <= 0.0
            || self.half_height <= 0.0
            || point.x.abs() / self.half_width + point.y.abs() / self.half_height
                > 1.0 + f64::EPSILON * 16.0
        {
            return false;
        }
        self.corners.iter().all(|corner| {
            corner.radius <= 0.0
                || !corner.in_arc_sector(point)
                || (point.x - corner.center.x).hypot(point.y - corner.center.y)
                    <= corner.radius + 1e-9
        })
    }

    pub fn outline_distance(&self, point: Point<f64>) -> f64 {
        if !point.x.is_finite()
            || !point.y.is_finite()
            || self.half_width <= 0.0
            || self.half_height <= 0.0
        {
            return f64::INFINITY;
        }
        let mut nearest = f64::INFINITY;
        for (index, corner) in self.corners.iter().enumerate() {
            let next = self.corners[(index + 1) % 4];
            let edge = subtract(next.entry, corner.exit);
            let delta = subtract(point, corner.exit);
            let scale = edge
                .x
                .abs()
                .max(edge.y.abs())
                .max(delta.x.abs())
                .max(delta.y.abs())
                .max(f64::MIN_POSITIVE);
            let scaled_edge = Point::new(edge.x / scale, edge.y / scale);
            let scaled_delta = Point::new(delta.x / scale, delta.y / scale);
            let length = scaled_edge.x * scaled_edge.x + scaled_edge.y * scaled_edge.y;
            let t = if length > 0.0 {
                ((scaled_delta.x * scaled_edge.x + scaled_delta.y * scaled_edge.y) / length)
                    .clamp(0.0, 1.0)
            } else {
                0.0
            };
            nearest = nearest.min((delta.x - edge.x * t).hypot(delta.y - edge.y * t));
            if corner.radius > 0.0 && corner.in_arc_sector(point) {
                nearest = nearest.min(
                    ((point.x - corner.center.x).hypot(point.y - corner.center.y) - corner.radius)
                        .abs(),
                );
            }
        }
        nearest
    }

    pub fn exterior_distance(&self, point: Point<f64>) -> f64 {
        if self.contains(point) {
            0.0
        } else {
            self.outline_distance(point)
        }
    }
}

pub fn diamond_corner_radius_limit(
    width: f64,
    height: f64,
    radii: CornerRadii,
    index: usize,
) -> f64 {
    if width <= 0.0 || height <= 0.0 {
        return 0.0;
    }
    let radii = normalize_shape_corner_radii(HighlightShape::Diamond, width, height, radii);
    let values = [
        radii.top_left,
        radii.top_right,
        radii.bottom_right,
        radii.bottom_left,
    ];
    let a = width / 2.0;
    let b = height / 2.0;
    let edge = a.hypot(b);
    let cotangents = [b / a, a / b, b / a, a / b];
    let previous = (index + 3) % 4;
    let next = (index + 1) % 4;
    let rect_limit = match index {
        0 => (width - values[1]).min(height - values[3]),
        1 => (width - values[0]).min(height - values[2]),
        2 => (width - values[3]).min(height - values[1]),
        _ => (width - values[2]).min(height - values[0]),
    };
    rect_limit
        .min((edge - values[previous] * cotangents[previous]) / cotangents[index])
        .min((edge - values[next] * cotangents[next]) / cotangents[index])
        .max(0.0)
}

pub fn diamond_uniform_corner_radius_limit(width: f64, height: f64) -> f64 {
    let a = (width / 2.0).max(0.0);
    let b = (height / 2.0).max(0.0);
    if a <= 0.0 || b <= 0.0 {
        0.0
    } else {
        a * (b / a.hypot(b))
    }
}

fn handle_insets(width: f64, height: f64, zoom: f64, index: usize, limit: f64) -> (f64, f64) {
    let a = width / 2.0;
    let b = height / 2.0;
    let extent = if index.is_multiple_of(2) { b } else { a };
    let perpendicular = if index.is_multiple_of(2) { a } else { b };
    let minimum = 12.0 / zoom.max(0.0001);
    let recession = if perpendicular > 0.0 {
        limit * (a.hypot(b) / perpendicular - 1.0)
    } else {
        0.0
    };
    // Keep neighboring 12 px hit targets distinct as handles move inward.
    let maximum = (recession + minimum)
        .max(extent / 2.0)
        .min(extent - 9.0 / zoom.max(0.0001))
        .max(0.0);
    (minimum.min(maximum), maximum)
}

pub fn diamond_corner_handle_local_point(
    width: f64,
    height: f64,
    radii: CornerRadii,
    zoom: f64,
    index: usize,
) -> Point<f64> {
    let radii = normalize_shape_corner_radii(HighlightShape::Diamond, width, height, radii);
    let radius = [
        radii.top_left,
        radii.top_right,
        radii.bottom_right,
        radii.bottom_left,
    ][index];
    let limit = diamond_corner_radius_limit(width, height, radii, index);
    let (minimum, maximum) = handle_insets(width, height, zoom, index, limit);
    let inset = minimum
        + (maximum - minimum)
            * if limit > 0.0 {
                (radius / limit).clamp(0.0, 1.0)
            } else {
                0.0
            };
    match index {
        0 => Point::new(0.0, -height / 2.0 + inset),
        1 => Point::new(width / 2.0 - inset, 0.0),
        2 => Point::new(0.0, height / 2.0 - inset),
        _ => Point::new(-width / 2.0 + inset, 0.0),
    }
}

pub fn diamond_corner_radius_from_local_point(
    width: f64,
    height: f64,
    radii: CornerRadii,
    zoom: f64,
    index: usize,
    point: Point<f64>,
) -> f64 {
    let handle_limit = diamond_corner_radius_limit(width, height, radii, index);
    let limit = handle_limit;
    let (minimum, maximum) = handle_insets(width, height, zoom, index, handle_limit);
    let inset = match index {
        0 => point.y + height / 2.0,
        1 => width / 2.0 - point.x,
        2 => height / 2.0 - point.y,
        _ => point.x + width / 2.0,
    };
    if maximum <= minimum {
        0.0
    } else {
        limit * ((inset - minimum) / (maximum - minimum)).clamp(0.0, 1.0)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn diamond_distances_remain_finite_at_large_coordinates() {
        let contour = RoundedDiamond::new(1e200, 1e200, CornerRadii::splat(1e198));
        let expected = 1e201 - 5e199 + 1e198 * (2.0_f64.sqrt() - 1.0);
        let distance = contour.exterior_distance(Point::new(0.0, -1e201));
        assert!(distance.is_finite());
        assert!((distance / expected - 1.0).abs() < 1e-14);
        assert!(!contour.contains(Point::new(f64::NAN, 0.0)));
    }

    #[test]
    fn diamond_radii_normalize_to_tangent_edges_and_preserve_legacy_validation() {
        for (width, height) in [(100.0, 100.0), (200.0, 40.0), (40.0, 200.0)] {
            let radius = diamond_uniform_corner_radius_limit(width, height);
            let normalized = normalize_shape_corner_radii(
                HighlightShape::Diamond,
                width,
                height,
                CornerRadii::splat(1000.0),
            );
            assert!((normalized.top_left - radius).abs() < 1e-9);
            assert!(crate::corner_radii_fit_within_rect(
                width, height, normalized
            ));
            let contour = RoundedDiamond::new(width, height, normalized);
            assert!(contour.contains(Point::default()));
            for point in [
                Point::new(0.0, -height / 2.0),
                Point::new(width / 2.0, 0.0),
                Point::new(0.0, height / 2.0),
                Point::new(-width / 2.0, 0.0),
            ] {
                assert!(!contour.contains(point));
                assert!(contour.exterior_distance(point) > 0.0);
            }
        }
        let empty = RoundedDiamond::new(0.0, 10.0, CornerRadii::splat(5.0));
        assert!(!empty.contains(Point::default()));
    }

    #[test]
    fn diamond_independent_arcs_hit_only_the_rounded_tip() {
        let radii = CornerRadii {
            top_left: 20.0,
            ..CornerRadii::default()
        };
        let contour = RoundedDiamond::new(100.0, 100.0, radii);
        let tip = Point::new(0.0, -50.0 + 20.0 * (2.0_f64.sqrt() - 1.0));
        assert!(!contour.contains(Point::new(0.0, -49.0)));
        assert!(contour.contains(tip));
        assert!(contour.outline_distance(tip) < 1e-9);
        assert!((contour.exterior_distance(Point::new(0.0, tip.y - 3.0)) - 3.0).abs() < 1e-9);
        for point in [
            Point::new(50.0, 0.0),
            Point::new(0.0, 50.0),
            Point::new(-50.0, 0.0),
        ] {
            assert!(contour.contains(point));
        }
        assert!(
            RoundedDiamond::new(100.0, 100.0, CornerRadii::default())
                .contains(Point::new(0.0, -50.0))
        );
        let rect = crate::RectangleData {
            text_element_id: None,
            stroke_rendering_seed: 0,
            stroke_rendering_style: Default::default(),
            rectangle_kind: crate::RectangleElementKind::Rectangle,
            highlight_shape: HighlightShape::Diamond,
            width: 100.0,
            height: 100.0,
            center: Point::default(),
            rotation: 0.0,
            corner_radii: radii,
            fill: snow_draw_engine_core::ColorRgba8 {
                a: 255,
                ..Default::default()
            },
            fill_style: crate::FillStyle::Solid,
            stroke: snow_draw_engine_core::ColorRgba8 {
                a: 255,
                ..Default::default()
            },
            stroke_width: 2.0,
            stroke_style: crate::StrokeStyle::Solid,
            opacity: 1.0,
        };
        assert!(!crate::rectangle_hit_test(
            &rect,
            Point::new(0.0, -49.0),
            0.0
        ));
        let spotlight = rect.into_spotlight();
        assert!(!crate::rectangle_hit_test(
            &spotlight,
            Point::new(0.0, tip.y - 3.0),
            2.0
        ));
        assert!(crate::rectangle_hit_test(
            &spotlight,
            Point::new(0.0, tip.y - 3.0),
            4.0
        ));
        let outline = crate::RectangleData {
            fill: Default::default(),
            stroke_width: 2.0,
            ..rect
        };
        assert!(crate::rectangle_hit_test(&outline, tip, 0.0));
        assert!(!crate::rectangle_hit_test(&outline, Point::default(), 0.0));
    }

    #[test]
    fn diamond_handle_geometry_round_trips_each_tip_at_each_zoom() {
        for (width, height) in [(200.0, 200.0), (300.0, 100.0), (100.0, 300.0)] {
            for zoom in [0.5, 1.0, 2.0] {
                for index in 0..4 {
                    let radii = CornerRadii::splat(6.0);
                    let point =
                        diamond_corner_handle_local_point(width, height, radii, zoom, index);
                    let radius = diamond_corner_radius_from_local_point(
                        width, height, radii, zoom, index, point,
                    );
                    assert!((radius - 6.0).abs() < 1e-9);
                    assert!(RoundedDiamond::new(width, height, radii).contains(point));
                }
            }
        }
    }
}
