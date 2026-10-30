//! Variable-width brush contours. All primitives wind in the same direction so a single
//! winding fill provides union coverage, including translucent crossings, without path unions.
use serde::{Deserialize, Serialize};

use crate::{PathCommand, PathGeometry, Point, arrow::StrokeStyle};

pub const PRESSURE_SEGMENTS_PER_CHUNK: usize = 4;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum BrushMode {
    #[default]
    Regular,
    PressureSensitive,
}

#[derive(Clone, Copy, Debug, PartialEq, Serialize, Deserialize)]
pub struct PressureKnot {
    pub t: f64,
    pub pressure: f64,
}

/// Pressure response follows perfect-freehand 1.2.0's radius formula with Excalidraw's
/// thinning=0.6 and sine easing (reference packages/element/src/shape.ts). The extra
/// normalization preserves Snow's nominal width at pressure 0.5.
/// https://github.com/steveruizok/perfect-freehand/blob/v1.2.0/packages/perfect-freehand/src/getStrokeRadius.ts
pub fn pressure_radius(width: f64, pressure: f64) -> f64 {
    width.max(0.0)
        * 0.5
        * ((0.2 + 0.6 * pressure.clamp(0.0, 1.0)) * std::f64::consts::FRAC_PI_2).sin()
        / std::f64::consts::FRAC_1_SQRT_2
}

pub fn validate_pressure_profiles(profiles: &[Vec<PressureKnot>], edge_count: usize) -> bool {
    profiles.is_empty()
        || (profiles.len() == edge_count
            && profiles.iter().all(|profile| {
                !profile.is_empty()
                    && profile.iter().all(|knot| {
                        knot.t.is_finite()
                            && (0.0..=1.0).contains(&knot.t)
                            && knot.pressure.is_finite()
                            && (0.0..=1.0).contains(&knot.pressure)
                    })
                    && profile.windows(2).all(|knots| knots[0].t <= knots[1].t)
            }))
}

pub fn reverse_pressure_profiles(profiles: &mut [Vec<PressureKnot>]) {
    profiles.reverse();
    for profile in profiles {
        profile.reverse();
        for knot in profile {
            knot.t = 1.0 - knot.t;
        }
    }
}

pub fn pressure_at(profile: &[PressureKnot], t: f64) -> f64 {
    if profile.is_empty() {
        return 0.5;
    }
    let after = profile.partition_point(|knot| knot.t <= t);
    if after == 0 {
        return profile[0].pressure;
    }
    let left = profile[after - 1];
    if after == profile.len() {
        return left.pressure;
    }
    let right = profile[after];
    let span = right.t - left.t;
    if span <= 1e-12 {
        return right.pressure;
    }
    left.pressure + (right.pressure - left.pressure) * ((t - left.t) / span).clamp(0.0, 1.0)
}

/// Rebuild only groups containing changed centerline segments. Prefix chunks and their
/// centerline lengths retain their identity. Profiles are addressed by segment-local t.
pub fn pressure_path_geometry(
    revision: u64,
    centerline: &PathGeometry,
    profiles: &[Vec<PressureKnot>],
    width: f64,
    style: StrokeStyle,
    previous: Option<&PathGeometry>,
    first_changed_segment: usize,
) -> PathGeometry {
    let retain = previous.map_or(0, |previous| {
        (first_changed_segment / PRESSURE_SEGMENTS_PER_CHUNK).min(previous.chunks.len())
    });
    let first_segment = retain * PRESSURE_SEGMENTS_PER_CHUNK;
    let first_command = first_segment + 1;
    let first_chunk = centerline.chunks.partition_point(|chunk| {
        chunk.command_start as usize + chunk.commands.len() <= first_command
    });
    let mut current = centerline
        .chunks
        .get(first_chunk)
        .map_or(Point::default(), |chunk| {
            Point::new(chunk.start_point[0], chunk.start_point[1])
        });
    let mut cumulative_length = previous
        .and_then(|p| p.chunks.get(retain.wrapping_sub(1)))
        .map_or(0.0, |chunk| chunk.cumulative_end_length);
    let mut group_start = cumulative_length;
    let mut group = Vec::new();
    let mut tail = Vec::new();
    let mut segment_count = 0usize;
    for chunk in &centerline.chunks[first_chunk..] {
        for (offset, command) in chunk.commands.iter().copied().enumerate() {
            let index = chunk.command_start as usize + offset;
            if index < first_command {
                current = command_end(command);
                continue;
            }
            if let PathCommand::MoveTo { point } = command {
                current = Point::new(point[0], point[1]);
                continue;
            }
            let profile = profiles.get(index - 1).map_or(&[][..], Vec::as_slice);
            let samples = segment_samples(current, command, profile, width);
            visit_segment_coverage(&samples, width, style, cumulative_length, |primitive| {
                primitive.append_to(&mut group, width);
                false
            });
            cumulative_length += samples.length;
            current = command_end(command);
            segment_count += 1;
            if segment_count.is_multiple_of(PRESSURE_SEGMENTS_PER_CHUNK) {
                tail.push((std::mem::take(&mut group), group_start, cumulative_length));
                group_start = cumulative_length;
            }
        }
    }
    if !segment_count.is_multiple_of(PRESSURE_SEGMENTS_PER_CHUNK) {
        tail.push((group, group_start, cumulative_length));
    }
    PathGeometry::replace_contour_tail(previous, revision, retain, tail)
}

pub fn pressure_segment_hit_test(
    start: Point<f64>,
    command: PathCommand,
    profile: &[PressureKnot],
    width: f64,
    point: Point<f64>,
    tolerance: f64,
) -> bool {
    pressure_segment_hit_test_styled(
        start,
        command,
        profile,
        width,
        point,
        tolerance,
        StrokeStyle::Solid,
        0.0,
    )
    .0
}

/// The returned length advances dash phase even when the query misses this segment.
// Geometry/style inputs stay explicit at this low-level query boundary.
#[allow(clippy::too_many_arguments)]
pub fn pressure_segment_hit_test_styled(
    start: Point<f64>,
    command: PathCommand,
    profile: &[PressureKnot],
    width: f64,
    point: Point<f64>,
    tolerance: f64,
    style: StrokeStyle,
    start_length: f64,
) -> (bool, f64) {
    let end = command_end(command);
    let mut min = Point::new(start.x.min(end.x), start.y.min(end.y));
    let mut max = Point::new(start.x.max(end.x), start.y.max(end.y));
    let controls = match command {
        PathCommand::CubicTo {
            control_1,
            control_2,
            ..
        } => vec![control_1, control_2],
        PathCommand::QuadTo { control, .. } => vec![control],
        _ => Vec::new(),
    };
    for p in controls {
        min.x = min.x.min(p[0]);
        min.y = min.y.min(p[1]);
        max.x = max.x.max(p[0]);
        max.y = max.y.max(p[1]);
    }
    let max_pressure = profile
        .iter()
        .map(|k| k.pressure)
        .reduce(f64::max)
        .unwrap_or(0.5);
    let radius = pressure_radius(width, max_pressure) + tolerance;
    let candidate = point.x >= min.x - radius
        && point.x <= max.x + radius
        && point.y >= min.y - radius
        && point.y <= max.y + radius;
    if !candidate && style == StrokeStyle::Solid {
        return (false, 0.0);
    }
    let samples = segment_samples(start, command, profile, width);
    let hit = candidate
        && visit_segment_coverage(&samples, width, style, start_length, |primitive| {
            primitive.contains(width, point, tolerance)
        });
    (hit, samples.length)
}

type PressureSample = (Point<f64>, f64);

struct StationaryPeak {
    sample: PressureSample,
    distance: f64,
}

struct SegmentSamples {
    points: Vec<PressureSample>,
    stationary_peaks: Vec<StationaryPeak>,
    length: f64,
}

enum StrokePrimitive<'a> {
    Ribbon(&'a [PressureSample]),
    Disk(PressureSample),
}

impl StrokePrimitive<'_> {
    fn append_to(&self, commands: &mut Vec<PathCommand>, width: f64) {
        match self {
            Self::Ribbon(samples) => ribbon(commands, samples, width),
            Self::Disk((point, pressure)) => {
                circle(commands, *point, pressure_radius(width, *pressure))
            }
        }
    }

    fn contains(&self, width: f64, point: Point<f64>, tolerance: f64) -> bool {
        match self {
            Self::Ribbon(samples) => ribbon_contains(samples, width, point, tolerance),
            Self::Disk((center, pressure)) => {
                (point.x - center.x).hypot(point.y - center.y)
                    <= pressure_radius(width, *pressure) + tolerance
            }
        }
    }
}

#[derive(Clone, Copy)]
enum StrokePattern {
    Solid,
    Dashed { period: f64, on: f64 },
    Dotted { period: f64 },
}

impl StrokePattern {
    fn new(style: StrokeStyle, width: f64) -> Self {
        match style {
            StrokeStyle::Solid => Self::Solid,
            StrokeStyle::Dashed => Self::Dashed {
                period: width * 4.4,
                on: width * 2.0,
            },
            StrokeStyle::Dotted => Self::Dotted {
                period: width * 2.0,
            },
        }
    }

    fn paints_at(self, distance: f64) -> bool {
        match self {
            Self::Solid => true,
            Self::Dashed { period, on } => {
                let phase = distance.rem_euclid(period);
                phase <= on + 1e-9 || period - phase <= 1e-9
            }
            Self::Dotted { period } => {
                let phase = distance.rem_euclid(period);
                phase.min(period - phase) <= 1e-9
            }
        }
    }
}

/// Both contour generation and hit testing consume the same painted primitives.
/// Duplicate-t peaks paint once at their sampled arc-length phase, never in a pattern gap.
fn visit_segment_coverage(
    samples: &SegmentSamples,
    width: f64,
    style: StrokeStyle,
    start_length: f64,
    mut visit: impl FnMut(StrokePrimitive<'_>) -> bool,
) -> bool {
    // Solid centerlines retain hit-test tolerance even at zero width. Patterned
    // strokes need a positive period before dash or dot traversal can proceed.
    if width <= 0.0 && style != StrokeStyle::Solid {
        return false;
    }
    let pattern = StrokePattern::new(style, width);
    match pattern {
        StrokePattern::Solid => {
            if visit(StrokePrimitive::Ribbon(&samples.points)) {
                return true;
            }
        }
        StrokePattern::Dashed { period, on } => {
            for run in dash_runs(&samples.points, period, on, start_length) {
                if visit(StrokePrimitive::Ribbon(&run)) {
                    return true;
                }
            }
        }
        StrokePattern::Dotted { period } => {
            for sample in dot_samples(&samples.points, period, start_length) {
                if visit(StrokePrimitive::Disk(sample)) {
                    return true;
                }
            }
        }
    }
    samples.stationary_peaks.iter().any(|peak| {
        pattern.paints_at(start_length + peak.distance) && visit(StrokePrimitive::Disk(peak.sample))
    })
}

fn capsule_hit(
    a: Point<f64>,
    b: Point<f64>,
    ra: f64,
    rb: f64,
    point: Point<f64>,
    tolerance: f64,
) -> bool {
    let dx = b.x - a.x;
    let dy = b.y - a.y;
    let squared = dx * dx + dy * dy;
    let t = if squared <= 1e-12 {
        0.0
    } else {
        (((point.x - a.x) * dx + (point.y - a.y) * dy) / squared).clamp(0.0, 1.0)
    };
    (point.x - a.x - dx * t).hypot(point.y - a.y - dy * t) <= ra + (rb - ra) * t + tolerance
}

fn command_end(command: PathCommand) -> Point<f64> {
    let point = match command {
        PathCommand::MoveTo { point } | PathCommand::LineTo { point } => point,
        PathCommand::QuadTo { end, .. } | PathCommand::CubicTo { end, .. } => end,
    };
    Point::new(point[0], point[1])
}

fn evaluate_segment(start: Point<f64>, command: PathCommand, t: f64) -> Point<f64> {
    let end = command_end(command);
    let mt = 1.0 - t;
    match command {
        PathCommand::CubicTo {
            control_1: a,
            control_2: b,
            ..
        } => Point::new(
            mt.powi(3) * start.x
                + 3.0 * mt * mt * t * a[0]
                + 3.0 * mt * t * t * b[0]
                + t.powi(3) * end.x,
            mt.powi(3) * start.y
                + 3.0 * mt * mt * t * a[1]
                + 3.0 * mt * t * t * b[1]
                + t.powi(3) * end.y,
        ),
        PathCommand::QuadTo { control: c, .. } => Point::new(
            mt * mt * start.x + 2.0 * mt * t * c[0] + t * t * end.x,
            mt * mt * start.y + 2.0 * mt * t * c[1] + t * t * end.y,
        ),
        _ => Point::new(
            start.x + (end.x - start.x) * t,
            start.y + (end.y - start.y) * t,
        ),
    }
}

fn segment_samples(
    start: Point<f64>,
    command: PathCommand,
    profile: &[PressureKnot],
    width: f64,
) -> SegmentSamples {
    let mut knots = vec![0.0, 1.0];
    knots.extend(profile.iter().map(|knot| knot.t));
    knots.sort_unstable_by(f64::total_cmp);
    knots.dedup();
    let mut ts = vec![0.0];
    for pair in knots.windows(2) {
        flatten_interval(start, command, profile, width, pair[0], pair[1], 0, &mut ts);
    }
    let mut result = SegmentSamples {
        points: Vec::with_capacity(ts.len()),
        stationary_peaks: Vec::new(),
        length: 0.0,
    };
    let mut peaks = profile
        .chunk_by(|a, b| a.t == b.t)
        .filter(|knots| knots.len() > 1)
        .peekable();
    for t in ts {
        let point = evaluate_segment(start, command, t);
        if let Some((previous, _)) = result.points.last() {
            result.length += (point.x - previous.x).hypot(point.y - previous.y);
        }
        result.points.push((point, pressure_at(profile, t)));
        if peaks.peek().is_some_and(|knots| knots[0].t == t) {
            let pressure = peaks
                .next()
                .unwrap()
                .iter()
                .map(|knot| knot.pressure)
                .fold(0.0, f64::max);
            result.stationary_peaks.push(StationaryPeak {
                sample: (point, pressure),
                distance: result.length,
            });
        }
    }
    result
}

// Recursive bounds carry the original curve, pressure profile, and parameter interval.
#[allow(clippy::too_many_arguments)]
fn flatten_interval(
    start: Point<f64>,
    command: PathCommand,
    profile: &[PressureKnot],
    width: f64,
    a: f64,
    b: f64,
    depth: u8,
    output: &mut Vec<f64>,
) {
    const TOLERANCE: f64 = 0.125;
    let pa = evaluate_segment(start, command, a);
    let pb = evaluate_segment(start, command, b);
    let mid = (a + b) * 0.5;
    let dx = pb.x - pa.x;
    let dy = pb.y - pa.y;
    let chord_sq = dx * dx + dy * dy;
    let deviation = |t: f64| {
        let p = evaluate_segment(start, command, t);
        let ratio = if chord_sq <= 1e-18 {
            0.0
        } else {
            (((p.x - pa.x) * dx + (p.y - pa.y) * dy) / chord_sq).clamp(0.0, 1.0)
        };
        (p.x - pa.x - ratio * dx).hypot(p.y - pa.y - ratio * dy)
    };
    let bend = deviation(mid)
        .max(deviation((a + mid) * 0.5))
        .max(deviation((mid + b) * 0.5));
    let radius_error = (pressure_radius(width, pressure_at(profile, mid))
        - (pressure_radius(width, pressure_at(profile, a))
            + pressure_radius(width, pressure_at(profile, b)))
            * 0.5)
        .abs();
    if depth < 10 && (bend > TOLERANCE || radius_error > TOLERANCE) {
        flatten_interval(start, command, profile, width, a, mid, depth + 1, output);
        flatten_interval(start, command, profile, width, mid, b, depth + 1, output);
    } else {
        output.push(b);
    }
}

fn ribbon_sides(samples: &[(Point<f64>, f64)], width: f64) -> (Vec<[f64; 2]>, Vec<[f64; 2]>) {
    let mut left = Vec::with_capacity(samples.len());
    let mut right = Vec::with_capacity(samples.len());
    for (index, (point, pressure)) in samples.iter().copied().enumerate() {
        let before = samples[index.saturating_sub(1)].0;
        let after = samples[(index + 1).min(samples.len() - 1)].0;
        let mut dx = after.x - before.x;
        let mut dy = after.y - before.y;
        let length = dx.hypot(dy);
        if length <= 1e-12 {
            dx = 1.0;
            dy = 0.0;
        } else {
            dx /= length;
            dy /= length;
        }
        let radius = pressure_radius(width, pressure);
        left.push([point.x - dy * radius, point.y + dx * radius]);
        right.push([point.x + dy * radius, point.y - dx * radius]);
    }
    (left, right)
}

fn ribbon_contains(
    samples: &[(Point<f64>, f64)],
    width: f64,
    point: Point<f64>,
    tolerance: f64,
) -> bool {
    if samples.len() < 2 {
        return false;
    }
    let (left, right) = ribbon_sides(samples, width);
    let polygon: Vec<_> = right.iter().chain(left.iter().rev()).copied().collect();
    let mut winding = 0i32;
    for (a, b) in polygon
        .iter()
        .zip(polygon.iter().cycle().skip(1))
        .take(polygon.len())
    {
        let cross = (b[0] - a[0]) * (point.y - a[1]) - (point.x - a[0]) * (b[1] - a[1]);
        if a[1] <= point.y && b[1] > point.y && cross > 0.0 {
            winding += 1;
        }
        if a[1] > point.y && b[1] <= point.y && cross < 0.0 {
            winding -= 1;
        }
        if tolerance > 0.0
            && capsule_hit(
                Point::new(a[0], a[1]),
                Point::new(b[0], b[1]),
                0.0,
                0.0,
                point,
                tolerance,
            )
        {
            return true;
        }
    }
    winding != 0
        || [samples[0], samples[samples.len() - 1]]
            .iter()
            .any(|(p, pressure)| {
                (point.x - p.x).hypot(point.y - p.y)
                    <= pressure_radius(width, *pressure) + tolerance
            })
}

fn ribbon(out: &mut Vec<PathCommand>, samples: &[(Point<f64>, f64)], width: f64) {
    if samples.len() < 2 || width <= 0.0 {
        return;
    }
    let (left, right) = ribbon_sides(samples, width);
    let first = left[0];
    out.push(PathCommand::MoveTo { point: first });
    // Positive winding matches circles even when stroke direction is reversed.
    for point in right.iter().chain(left.iter().rev()) {
        out.push(PathCommand::LineTo { point: *point });
    }
    out.push(PathCommand::LineTo { point: first });
    circle(out, samples[0].0, pressure_radius(width, samples[0].1));
    let last = samples[samples.len() - 1];
    circle(out, last.0, pressure_radius(width, last.1));
}

fn interpolate_sample(a: (Point<f64>, f64), b: (Point<f64>, f64), t: f64) -> (Point<f64>, f64) {
    (
        Point::new(a.0.x + (b.0.x - a.0.x) * t, a.0.y + (b.0.y - a.0.y) * t),
        a.1 + (b.1 - a.1) * t,
    )
}

/// One contour per continuous painted dash, with caps only at its ends. Adaptive
/// curve/radius subdivision must never create thousands of overlapping cap disks.
fn dash_runs(
    samples: &[(Point<f64>, f64)],
    period: f64,
    on: f64,
    start_length: f64,
) -> Vec<Vec<(Point<f64>, f64)>> {
    let mut runs = Vec::new();
    let mut cumulative = start_length;
    let mut active_cycle = None;
    for pair in samples.windows(2) {
        let length = (pair[1].0.x - pair[0].0.x).hypot(pair[1].0.y - pair[0].0.y);
        if length <= 1e-12 {
            continue;
        }
        let end = cumulative + length;
        let mut cycle = (cumulative / period).floor();
        while cycle * period < end {
            let begin = cumulative.max(cycle * period);
            let stop = end.min(cycle * period + on);
            if stop > begin {
                let a = interpolate_sample(
                    pair[0],
                    pair[1],
                    ((begin - cumulative) / length).clamp(0.0, 1.0),
                );
                let b = interpolate_sample(
                    pair[0],
                    pair[1],
                    ((stop - cumulative) / length).clamp(0.0, 1.0),
                );
                if active_cycle != Some(cycle) {
                    runs.push(vec![a]);
                    active_cycle = Some(cycle);
                }
                let run = runs.last_mut().unwrap();
                if run.last().is_none_or(|last| last.0 != a.0) {
                    run.push(a);
                }
                run.push(b);
            }
            cycle += 1.0;
        }
        cumulative = end;
    }
    runs
}

fn dot_samples(
    samples: &[(Point<f64>, f64)],
    period: f64,
    start_length: f64,
) -> Vec<(Point<f64>, f64)> {
    let mut dots = Vec::new();
    let mut at = (start_length / period).ceil() * period;
    let mut cumulative = start_length;
    for pair in samples.windows(2) {
        let length = (pair[1].0.x - pair[0].0.x).hypot(pair[1].0.y - pair[0].0.y);
        if length <= 1e-12 {
            continue;
        }
        while at <= cumulative + length + 1e-9 {
            dots.push(interpolate_sample(
                pair[0],
                pair[1],
                ((at - cumulative) / length).clamp(0.0, 1.0),
            ));
            at += period;
        }
        cumulative += length;
    }
    dots
}

fn circle(out: &mut Vec<PathCommand>, center: Point<f64>, radius: f64) {
    if radius <= 0.0 {
        return;
    }
    const K: f64 = 0.552_284_749_830_793_6;
    let (x, y, r, k) = (center.x, center.y, radius, radius * K);
    out.push(PathCommand::MoveTo { point: [x + r, y] });
    for (control_1, control_2, end) in [
        ([x + r, y + k], [x + k, y + r], [x, y + r]),
        ([x - k, y + r], [x - r, y + k], [x - r, y]),
        ([x - r, y - k], [x - k, y - r], [x, y - r]),
        ([x + k, y - r], [x + r, y - k], [x + r, y]),
    ] {
        out.push(PathCommand::CubicTo {
            control_1,
            control_2,
            end,
        });
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Arc;

    fn stationary_profile(t: f64) -> Vec<PressureKnot> {
        vec![
            PressureKnot {
                t: 0.0,
                pressure: 0.2,
            },
            PressureKnot { t, pressure: 0.2 },
            PressureKnot { t, pressure: 0.9 },
            PressureKnot { t, pressure: 0.2 },
            PressureKnot {
                t: 1.0,
                pressure: 0.2,
            },
        ]
    }

    #[test]
    fn pressure_stationary_dot_keeps_peak_for_every_stroke_pattern() {
        let start = Point::new(40.0, 100.0);
        let command = PathCommand::LineTo {
            point: [40.001, 100.0],
        };
        let centerline = PathGeometry::from_commands(
            1,
            vec![
                PathCommand::MoveTo {
                    point: [start.x, start.y],
                },
                command,
            ],
            false,
        );
        let profile = stationary_profile(0.0);
        for style in [StrokeStyle::Solid, StrokeStyle::Dashed, StrokeStyle::Dotted] {
            let geometry = pressure_path_geometry(
                1,
                &centerline,
                std::slice::from_ref(&profile),
                16.0,
                style,
                None,
                0,
            );
            assert!(
                geometry.canvas_bounds[3] >= start.y + pressure_radius(16.0, 0.9),
                "{style:?}"
            );
            assert!(
                pressure_segment_hit_test_styled(
                    start,
                    command,
                    &profile,
                    16.0,
                    Point::new(start.x, start.y + 9.0),
                    0.0,
                    style,
                    0.0
                )
                .0,
                "{style:?}"
            );
            assert!(
                !pressure_segment_hit_test_styled(
                    start,
                    command,
                    &profile,
                    16.0,
                    Point::new(start.x, start.y + 12.0),
                    0.0,
                    style,
                    0.0
                )
                .0,
                "{style:?}"
            );
        }
    }

    #[test]
    fn pressure_stationary_pattern_peaks_respect_global_phase_and_gaps() {
        let start = Point::default();
        let command = PathCommand::LineTo {
            point: [160.0, 0.0],
        };
        let centerline = PathGeometry::from_commands(
            1,
            vec![PathCommand::MoveTo { point: [0.0, 0.0] }, command],
            false,
        );
        for (style, x, painted) in [
            (StrokeStyle::Dashed, 16.0, true),
            (StrokeStyle::Dashed, 48.0, false),
            (StrokeStyle::Dotted, 32.0, true),
            (StrokeStyle::Dotted, 16.0, false),
        ] {
            let profile = stationary_profile(x / 160.0);
            let geometry = pressure_path_geometry(
                1,
                &centerline,
                std::slice::from_ref(&profile),
                16.0,
                style,
                None,
                0,
            );
            assert_eq!(geometry.canvas_bounds[3] > 9.0, painted, "{style:?}, x={x}");
            let query = Point::new(x, 9.0);
            assert_eq!(
                pressure_segment_hit_test_styled(
                    start, command, &profile, 16.0, query, 0.0, style, 0.0
                )
                .0,
                painted,
                "{style:?}, x={x}"
            );
            // Starting this same segment at a different phase reverses peak eligibility.
            let offset = if style == StrokeStyle::Dashed {
                35.2
            } else {
                16.0
            };
            assert_eq!(
                pressure_segment_hit_test_styled(
                    start, command, &profile, 16.0, query, 0.0, style, offset
                )
                .0,
                !painted,
                "offset {style:?}, x={x}"
            );
        }
    }

    #[test]
    fn pressure_stationary_pattern_peaks_use_curve_arc_length() {
        // x(t) = 160t^3: the midpoint parameter is at x=20, not half the path length.
        let command = PathCommand::CubicTo {
            control_1: [0.0, 0.0],
            control_2: [0.0, 0.0],
            end: [160.0, 0.0],
        };
        let profile = stationary_profile(0.5);
        let centerline = PathGeometry::from_commands(
            1,
            vec![PathCommand::MoveTo { point: [0.0, 0.0] }, command],
            false,
        );
        let geometry = pressure_path_geometry(
            1,
            &centerline,
            std::slice::from_ref(&profile),
            10.0,
            StrokeStyle::Dotted,
            None,
            0,
        );
        assert!(geometry.canvas_bounds[3] > 6.0);
        assert!(
            pressure_segment_hit_test_styled(
                Point::default(),
                command,
                &profile,
                10.0,
                Point::new(20.0, 6.0),
                0.0,
                StrokeStyle::Dotted,
                0.0,
            )
            .0
        );
        assert!(
            !pressure_segment_hit_test_styled(
                Point::default(),
                command,
                &profile,
                10.0,
                Point::new(20.0, 6.0),
                0.0,
                StrokeStyle::Dotted,
                10.0,
            )
            .0
        );
    }

    #[test]
    fn pressure_stationary_pattern_peaks_match_incremental_chunk_rebuilds() {
        for style in [StrokeStyle::Solid, StrokeStyle::Dashed, StrokeStyle::Dotted] {
            let step = if style == StrokeStyle::Dashed {
                35.2
            } else {
                32.0
            };
            let commands = std::iter::once(PathCommand::MoveTo { point: [0.0, 0.0] })
                .chain((1..=5).map(|i| PathCommand::LineTo {
                    point: [f64::from(i) * step, 0.0],
                }))
                .collect();
            let centerline = PathGeometry::from_commands(1, commands, false);
            let mut profiles = vec![
                vec![
                    PressureKnot {
                        t: 0.0,
                        pressure: 0.2
                    },
                    PressureKnot {
                        t: 1.0,
                        pressure: 0.2
                    }
                ];
                5
            ];
            let before = pressure_path_geometry(1, &centerline, &profiles, 16.0, style, None, 0);
            profiles[4] = stationary_profile(0.0);
            let tail =
                pressure_path_geometry(2, &centerline, &profiles, 16.0, style, Some(&before), 4);
            let full = pressure_path_geometry(2, &centerline, &profiles, 16.0, style, None, 0);
            assert_eq!(tail.flattened_commands(), full.flattened_commands());
            assert!(Arc::ptr_eq(
                &before.chunks[0].commands,
                &tail.chunks[0].commands
            ));
            assert!(tail.canvas_bounds[3] > 9.0, "{style:?}");
            assert!(
                pressure_segment_hit_test_styled(
                    Point::new(step * 4.0, 0.0),
                    PathCommand::LineTo {
                        point: [step * 5.0, 0.0]
                    },
                    &profiles[4],
                    16.0,
                    Point::new(step * 4.0, 9.0),
                    0.0,
                    style,
                    step * 4.0,
                )
                .0,
                "{style:?}"
            );
        }
    }

    #[test]
    fn pressure_zero_width_preserves_solid_hit_tolerance_and_skips_patterns() {
        for style in [StrokeStyle::Solid, StrokeStyle::Dashed, StrokeStyle::Dotted] {
            assert_eq!(
                pressure_segment_hit_test_styled(
                    Point::default(),
                    PathCommand::LineTo { point: [10.0, 0.0] },
                    &[],
                    0.0,
                    Point::new(5.0, 1.0),
                    2.0,
                    style,
                    0.0,
                )
                .0,
                style == StrokeStyle::Solid,
            );
        }
    }

    #[test]
    fn pressure_dashes_cap_runs_instead_of_every_sample() {
        let path = PathGeometry::from_commands(
            1,
            vec![
                PathCommand::MoveTo { point: [0.0, 0.0] },
                PathCommand::LineTo {
                    point: [500.0, 0.0],
                },
            ],
            false,
        );
        let profiles = vec![
            (0..=256)
                .map(|i| PressureKnot {
                    t: i as f64 / 256.0,
                    pressure: 0.5 + (i as f64 * 0.04).sin() * 0.4,
                })
                .collect(),
        ];
        let dashed = pressure_path_geometry(1, &path, &profiles, 8.0, StrokeStyle::Dashed, None, 0);
        let commands = dashed.flattened_commands();
        let subpaths = commands
            .iter()
            .filter(|c| matches!(c, PathCommand::MoveTo { .. }))
            .count();
        let maximum_runs = (500.0_f64 / (8.0 * 4.4)).ceil() as usize + 1;
        assert!(
            subpaths <= maximum_runs * 3,
            "too many cap/ribbon subpaths: {subpaths}"
        );
        assert!(
            commands.len() < 1000,
            "too many native fill commands: {}",
            commands.len()
        );
        let dotted = pressure_path_geometry(1, &path, &profiles, 8.0, StrokeStyle::Dotted, None, 0);
        assert!(
            dotted
                .flattened_commands()
                .iter()
                .filter(|c| matches!(c, PathCommand::MoveTo { .. }))
                .count()
                <= 32
        );
    }

    #[test]
    fn pressure_cold_large_stroke_has_local_chunks_and_reconstructible_tail() {
        let commands: Vec<_> = std::iter::once(PathCommand::MoveTo { point: [0.0, 0.0] })
            .chain((1..=16000).map(|i| PathCommand::LineTo {
                point: [i as f64 * 8.0, 0.0],
            }))
            .collect();
        let profiles = vec![
            vec![
                PressureKnot {
                    t: 0.0,
                    pressure: 0.5
                },
                PressureKnot {
                    t: 1.0,
                    pressure: 0.5
                }
            ];
            16000
        ];
        let path = PathGeometry::from_commands(1, commands.clone(), false);
        for style in [StrokeStyle::Solid, StrokeStyle::Dashed, StrokeStyle::Dotted] {
            let cold = pressure_path_geometry(1, &path, &profiles, 4.0, style, None, 0);
            let count: usize = cold.chunks.iter().map(|c| c.commands.len()).sum();
            assert!(
                cold.chunks
                    .iter()
                    .all(|c| c.canvas_bounds[2] - c.canvas_bounds[0] < 64.0)
            );
            assert!(count < 400000, "large stroke command count: {count}");
            let mut changed = commands.clone();
            changed[15997] = PathCommand::LineTo {
                point: [15997.0 * 8.0, 2.0],
            };
            let path = PathGeometry::from_commands(2, changed, false);
            let tail = pressure_path_geometry(2, &path, &profiles, 4.0, style, Some(&cold), 15996);
            let full = pressure_path_geometry(2, &path, &profiles, 4.0, style, None, 0);
            assert!(tail.flattened_commands() == full.flattened_commands());
            assert!(Arc::ptr_eq(
                &cold.chunks[0].commands,
                &tail.chunks[0].commands
            ));
            eprintln!(
                "pressure cold style={style:?} segments=16000 chunks={} commands={count}",
                cold.chunks.len()
            );
        }
    }

    #[test]
    fn pressure_peak_shoulders_hit_only_the_painted_ribbon() {
        let profile = vec![
            PressureKnot {
                t: 0.0,
                pressure: 0.0,
            },
            PressureKnot {
                t: 0.5,
                pressure: 1.0,
            },
            PressureKnot {
                t: 0.6,
                pressure: 0.0,
            },
            PressureKnot {
                t: 1.0,
                pressure: 0.0,
            },
        ];
        assert!(!pressure_segment_hit_test(
            Point::default(),
            PathCommand::LineTo {
                point: [100.0, 0.0]
            },
            &profile,
            20.0,
            Point::new(58.0, 9.0),
            0.0
        ));
        assert!(pressure_segment_hit_test(
            Point::default(),
            PathCommand::LineTo {
                point: [100.0, 0.0]
            },
            &profile,
            20.0,
            Point::new(50.0, 12.0),
            0.0
        ));
    }
    #[test]
    fn pressure_radius_matches_nominal_width_at_half_pressure() {
        assert!((pressure_radius(20.0, 0.5) - 10.0).abs() < 1e-12);
        assert!((pressure_radius(20.0, 0.0) / 10.0 - 0.4370160244488211).abs() < 1e-12);
        assert!((pressure_radius(20.0, 1.0) / 10.0 - 1.3449970239279146).abs() < 1e-12);
    }
    #[test]
    fn straight_profile_retains_multiple_bulges() {
        let path = PathGeometry::from_commands(
            1,
            vec![
                PathCommand::MoveTo { point: [0.0, 0.0] },
                PathCommand::LineTo {
                    point: [100.0, 0.0],
                },
            ],
            false,
        );
        let profiles = vec![vec![
            PressureKnot {
                t: 0.0,
                pressure: 0.1,
            },
            PressureKnot {
                t: 0.5,
                pressure: 1.0,
            },
            PressureKnot {
                t: 1.0,
                pressure: 0.1,
            },
        ]];
        let geometry =
            pressure_path_geometry(1, &path, &profiles, 20.0, StrokeStyle::Solid, None, 0);
        assert!(geometry.canvas_bounds[1] < -13.0);
        assert!(pressure_segment_hit_test(
            Point::default(),
            PathCommand::LineTo {
                point: [100.0, 0.0]
            },
            &profiles[0],
            20.0,
            Point::new(50.0, 12.0),
            0.0
        ));
        assert!(!pressure_segment_hit_test(
            Point::default(),
            PathCommand::LineTo {
                point: [100.0, 0.0]
            },
            &profiles[0],
            20.0,
            Point::new(0.0, 15.0),
            0.0
        ));
    }
    #[test]
    fn reversal_is_an_involution_and_keeps_stationary_peak() {
        let original = vec![vec![
            PressureKnot {
                t: 0.0,
                pressure: 0.3,
            },
            PressureKnot {
                t: 0.5,
                pressure: 0.2,
            },
            PressureKnot {
                t: 0.5,
                pressure: 0.9,
            },
            PressureKnot {
                t: 0.5,
                pressure: 0.4,
            },
            PressureKnot {
                t: 1.0,
                pressure: 0.6,
            },
        ]];
        let mut reversed = original.clone();
        reverse_pressure_profiles(&mut reversed);
        reverse_pressure_profiles(&mut reversed);
        assert_eq!(reversed, original);
    }
}
