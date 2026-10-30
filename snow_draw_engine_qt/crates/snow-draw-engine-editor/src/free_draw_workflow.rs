use std::sync::Arc;

use snow_draw_engine_core::{
    BrushMode, PATH_CHUNK_COMMAND_CAPACITY, PathGeometry, PathSegmentMode, Point, PressureKnot,
    StrokePointFilter, catmull_rom_path_commands, pressure_path_geometry,
    reverse_pressure_profiles,
};
use snow_draw_engine_document::{ElementMeta, FreeDrawData, FreeDrawStyle};

use super::*;

const MAX_PENDING_SAMPLES: usize = 128;
const PENDING_OVERLAP: usize = 4;
// Round caps need a real segment for click-only dots.
const DOT_SEGMENT_LENGTH: f64 = 1e-3;

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct StreamingFreeDrawBuilder {
    committed_vertices: Vec<Point<f64>>,
    committed_modes: Vec<PathSegmentMode>,
    committed_distances: Vec<f64>,
    pressure_segments: Vec<Vec<PressureKnot>>,
    pending_samples: Vec<Point<f64>>,
    pending_distances: Vec<f64>,
    filtered_samples: Vec<Point<f64>>,
    filtered_distances: Vec<f64>,
    pressure_capture: crate::free_draw_pressure::PressureCapture,
    filter_origin_distance: f64,
    defer_preview: bool,
    preview_dirty: bool,
    preview_committed_count: usize,
    point_filter: StrokePointFilter,
    shift_active: bool,
    rdp_tolerance: f64,
    closure_radius: f64,
    geometry_revision: u64,
    style: ShapeStyle,
    preview: Option<Arc<FreeDrawPreview>>,
    reverse_output: bool,
}

impl Default for StreamingFreeDrawBuilder {
    fn default() -> Self {
        Self::new(
            Point::default(),
            1.0,
            crate::defaults::editor_style_defaults().free_draw,
        )
    }
}

impl StreamingFreeDrawBuilder {
    pub(crate) fn new(start: Point<f64>, zoom: f64, style: ShapeStyle) -> Self {
        let stroke_width = style.stroke_width;
        let zoom = zoom.max(1e-6);
        let screen_width = stroke_width.max(0.0) * zoom;
        let sample_spacing = (screen_width * 0.03).clamp(0.75, 1.5) / zoom;
        let response_distance = (screen_width * 0.35).max(6.0) / zoom;
        Self {
            committed_vertices: Vec::new(),
            committed_modes: Vec::new(),
            committed_distances: Vec::new(),
            pressure_segments: Vec::new(),
            pending_samples: vec![start],
            pending_distances: vec![0.0],
            filtered_samples: Vec::new(),
            filtered_distances: Vec::new(),
            pressure_capture: crate::free_draw_pressure::PressureCapture::new(
                start,
                None,
                0,
                stroke_width,
                sample_spacing,
            ),
            filter_origin_distance: 0.0,
            defer_preview: false,
            preview_dirty: false,
            preview_committed_count: 0,
            point_filter: StrokePointFilter::new(start, sample_spacing, response_distance),
            shift_active: false,
            rdp_tolerance: (screen_width * 0.06).clamp(0.35, 1.25) / zoom,
            closure_radius: 10.0_f64.max(screen_width * 0.5) / zoom,
            geometry_revision: 1,
            style,
            preview: None,
            reverse_output: false,
        }
    }

    fn continue_from(data: &FreeDrawData, at_start: bool, zoom: f64) -> Self {
        let mut vertices = data.global_vertices();
        let mut modes = data.segment_modes.clone();
        if at_start {
            vertices.reverse();
            modes.reverse();
        }
        let start = *vertices.last().expect("validated free draw has endpoints");
        let mut builder = Self::new(start, zoom, ShapeStyle::from_free_draw(data));
        builder.reverse_output = at_start;
        builder.committed_vertices = vertices;
        builder.committed_modes = modes;
        builder.preview_committed_count = builder.committed_vertices.len();
        builder.committed_distances = vec![0.0; builder.committed_vertices.len()];
        builder.pressure_segments = data.pressure_segments.clone();
        if builder.pressure_segments.is_empty() {
            builder
                .pressure_segments
                .resize_with(builder.committed_modes.len(), || {
                    vec![
                        PressureKnot {
                            t: 0.0,
                            pressure: 0.5,
                        },
                        PressureKnot {
                            t: 1.0,
                            pressure: 0.5,
                        },
                    ]
                });
        }
        if at_start {
            reverse_pressure_profiles(&mut builder.pressure_segments);
        }
        let terminal = builder
            .pressure_segments
            .last()
            .and_then(|p| p.last())
            .map_or(0.5, |k| k.pressure);
        builder.pressure_capture.restart(start, 0.0, terminal);
        let geometry = Arc::new(PathGeometry::from_commands(1, data.path_commands(), false));
        let pressure_geometry = data.pressure_geometry(1, &geometry).map(Arc::new);
        // Seed the exact original preview. Do not run closure detection until movement.
        builder.preview = Some(Arc::new(FreeDrawPreview {
            geometry,
            brush_mode: data.brush_mode,
            pressure_geometry,
            stroke: data.stroke,
            stroke_width: data.stroke_width,
            stroke_style: data.stroke_style,
            fill: data.fill,
            fill_style: data.fill_style,
            opacity: data.opacity,
        }));
        builder
    }

    #[cfg(test)]
    pub(crate) fn append(&mut self, point: Point<f64>, shift: bool) -> bool {
        self.append_input(point, shift, None, 0)
    }

    pub(crate) fn append_input(
        &mut self,
        point: Point<f64>,
        shift: bool,
        pressure: Option<f64>,
        timestamp: u64,
    ) -> bool {
        if !point.x.is_finite() || !point.y.is_finite() {
            return false;
        }
        let pressure_changed = self.pressure_capture.observe(point, pressure, timestamp);
        let changed = self.append_position(point, shift);
        if !changed
            && pressure_changed
            && !self.shift_active
            && self
                .committed_vertices
                .last()
                .is_some_and(|last| distance(*last, point) <= 1e-12)
            && let Some(profile) = self.pressure_segments.last_mut()
        {
            let start = profile.partition_point(|knot| knot.t < 1.0);
            let incoming = profile
                .get(start)
                .or_else(|| profile.last())
                .map_or(0.5, |knot| knot.pressure);
            let peak = profile[start..]
                .iter()
                .map(|knot| knot.pressure)
                .fold(self.pressure_capture.pressure, f64::max);
            profile.truncate(start);
            profile.push(PressureKnot {
                t: 1.0,
                pressure: incoming,
            });
            if peak != incoming {
                profile.push(PressureKnot {
                    t: 1.0,
                    pressure: peak,
                });
            }
            if self.pressure_capture.pressure != peak {
                profile.push(PressureKnot {
                    t: 1.0,
                    pressure: self.pressure_capture.pressure,
                });
            }
        }
        if !changed && pressure_changed && self.style.brush_mode == BrushMode::PressureSensitive {
            self.advance_revision();
            self.request_preview();
            return true;
        }
        changed
    }

    fn append_position(&mut self, point: Point<f64>, shift: bool) -> bool {
        if shift {
            return self.append_straight(point);
        }
        if self.shift_active {
            self.shift_active = false;
            let anchor = self.last_point();
            self.pending_samples.clear();
            self.pending_samples.push(anchor);
            self.pending_distances.clear();
            self.pending_distances
                .push(self.pressure_capture.previous_distance);
            self.filter_origin_distance = self.pressure_capture.previous_distance;
            self.point_filter.reset(anchor);
        }
        self.filtered_samples.clear();
        self.filtered_distances.clear();
        self.point_filter
            .ingest_with_metadata(point, |sample, source_distance| {
                self.filtered_samples.push(sample);
                self.filtered_distances
                    .push(self.filter_origin_distance + source_distance);
            });
        if self.filtered_samples.is_empty() {
            return false;
        }
        for index in 0..self.filtered_samples.len() {
            self.pending_samples.push(self.filtered_samples[index]);
            self.pending_distances.push(self.filtered_distances[index]);
            if self.pending_samples.len() == MAX_PENDING_SAMPLES {
                self.compact_pending_window();
            }
        }
        self.advance_revision();
        self.request_preview();
        true
    }

    fn append_straight(&mut self, point: Point<f64>) -> bool {
        if !self.shift_active {
            if let Some(endpoint) = self.point_filter.settle_endpoint() {
                self.push_pending_sample_at(endpoint, self.pressure_capture.previous_distance);
            }
            self.flush_pending();
            let start = self.last_point();
            if distance(start, point) < self.point_filter.sample_spacing() {
                return false;
            }
            self.committed_vertices.push(point);
            let begin = self.committed_distances.last().copied().unwrap_or(0.0);
            self.committed_distances
                .push(self.pressure_capture.distance);
            self.pressure_segments.push(
                self.pressure_capture
                    .profile(begin, self.pressure_capture.distance),
            );
            self.committed_modes.push(PathSegmentMode::Straight);
            self.shift_active = true;
            self.advance_revision();
            self.request_preview();
            return true;
        }
        let Some(&endpoint) = self.committed_vertices.last() else {
            return false;
        };
        // Shift changes the centerline, not the scalar capture. Rebuild the complete
        // active profile even when sub-spacing movement leaves its endpoint unchanged.
        let begin = self.committed_distances[self.committed_distances.len() - 2];
        *self.committed_distances.last_mut().unwrap() = self.pressure_capture.distance;
        *self.pressure_segments.last_mut().unwrap() = self
            .pressure_capture
            .profile(begin, self.pressure_capture.distance);
        self.pressure_capture.discard_before(begin);
        if distance(endpoint, point) < self.point_filter.sample_spacing() {
            return false;
        }
        *self.committed_vertices.last_mut().unwrap() = point;
        self.advance_revision();
        self.request_preview();
        true
    }

    fn push_pending_sample(&mut self, point: Point<f64>) {
        self.push_pending_sample_at(point, self.pressure_capture.distance);
    }

    fn push_pending_sample_at(&mut self, point: Point<f64>, source_distance: f64) {
        if self
            .pending_samples
            .last()
            .is_some_and(|last| distance(*last, point) <= 1e-12)
        {
            return;
        }
        self.pending_samples.push(point);
        self.pending_distances.push(source_distance);
        if self.pending_samples.len() == MAX_PENDING_SAMPLES {
            self.compact_pending_window();
        }
    }

    fn compact_pending_window(&mut self) {
        let boundary = MAX_PENDING_SAMPLES - PENDING_OVERLAP;
        let indices = simplify_rdp_indices(&self.pending_samples[..=boundary], self.rdp_tolerance);
        let prefix: Vec<_> = indices
            .iter()
            .map(|&i| (self.pending_samples[i], self.pending_distances[i]))
            .collect();
        self.commit_curve_vertices(&prefix);
        self.pending_samples.drain(..boundary);
        self.pending_distances.drain(..boundary);
        self.pressure_capture
            .discard_before(self.pending_distances[0]);
        debug_assert_eq!(self.pending_samples.len(), PENDING_OVERLAP);
    }

    fn flush_pending(&mut self) {
        if self.pending_samples.is_empty() {
            return;
        }
        let indices = simplify_rdp_indices(&self.pending_samples, self.rdp_tolerance);
        let simplified: Vec<_> = indices
            .iter()
            .map(|&i| (self.pending_samples[i], self.pending_distances[i]))
            .collect();
        self.commit_curve_vertices(&simplified);
        self.pending_samples.clear();
        self.pending_distances.clear();
    }

    fn commit_curve_vertices(&mut self, vertices: &[(Point<f64>, f64)]) {
        for (point, source_distance) in vertices {
            if self
                .committed_vertices
                .last()
                .is_some_and(|last| distance(*last, *point) <= 1e-12)
            {
                continue;
            }
            if !self.committed_vertices.is_empty() {
                self.committed_modes.push(PathSegmentMode::Curve);
                self.pressure_segments.push(
                    self.pressure_capture
                        .profile(*self.committed_distances.last().unwrap(), *source_distance),
                );
            }
            self.committed_vertices.push(*point);
            self.committed_distances.push(*source_distance);
        }
    }

    pub(crate) fn preview(&self) -> Option<Arc<FreeDrawPreview>> {
        self.preview.clone()
    }

    fn request_preview(&mut self) {
        self.preview_dirty = true;
        if !self.defer_preview {
            self.rebuild_preview();
        }
    }

    fn rebuild_preview(&mut self) {
        self.preview_dirty = false;
        let previous_geometry = self
            .preview
            .as_ref()
            .map(|preview| preview.geometry.clone());
        let previous = previous_geometry.as_deref();
        let closure_candidate = self
            .first_point()
            .is_some_and(|first| distance(first, self.last_point()) <= self.closure_radius);
        let mut full_vertices_and_modes = (closure_candidate
            || previous.is_some_and(|geometry| geometry.closed))
        .then(|| self.simplified_tail_vertices_and_modes(0));
        let preview_closed = closure_candidate
            && full_vertices_and_modes
                .as_ref()
                .is_some_and(|(vertices, _)| vertices.len() >= 4);
        let previous_command_count = previous.map_or(0, |geometry| {
            geometry
                .chunks
                .iter()
                .map(|chunk| chunk.commands.len())
                .sum()
        });
        let first_changed_command = self.committed_vertices.len().saturating_sub(3);
        let mut command_start =
            first_changed_command / PATH_CHUNK_COMMAND_CAPACITY * PATH_CHUNK_COMMAND_CAPACITY;
        if self.reverse_output || preview_closed || previous.is_some_and(|geometry| geometry.closed)
        {
            command_start = 0;
        }
        command_start = command_start.min(
            previous_command_count / PATH_CHUNK_COMMAND_CAPACITY * PATH_CHUNK_COMMAND_CAPACITY,
        );
        let vertex_start = if command_start == 0 {
            0
        } else {
            command_start - 2
        };
        let (mut vertices, mut modes) = full_vertices_and_modes
            .take()
            .unwrap_or_else(|| self.simplified_tail_vertices_and_modes(vertex_start));
        if vertices.len() < 2 {
            if self.style.brush_mode == BrushMode::PressureSensitive && vertices.len() == 1 {
                let point = vertices[0];
                vertices.push(Point::new(point.x + DOT_SEGMENT_LENGTH, point.y));
                modes.push(PathSegmentMode::Curve);
            } else {
                self.preview = None;
                return;
            }
        }
        if preview_closed {
            vertices.pop();
            if modes.len() >= vertices.len() {
                modes.pop();
            }
            modes.push(if self.shift_active {
                PathSegmentMode::Straight
            } else {
                PathSegmentMode::Curve
            });
        }
        if self.reverse_output {
            vertices.reverse();
            modes.reverse();
            if preview_closed {
                modes.rotate_left(1);
            }
        }
        let mut commands = catmull_rom_path_commands(&vertices, &modes, preview_closed);
        let start_point = if command_start == 0 {
            [0.0; 2]
        } else {
            let point = vertices[1];
            commands.drain(..2);
            [point.x, point.y]
        };
        let geometry = Arc::new(if let Some(previous) = previous {
            previous.replace_tail(
                self.geometry_revision,
                command_start as u32,
                start_point,
                commands,
                preview_closed,
            )
        } else {
            PathGeometry::from_commands(self.geometry_revision, commands, preview_closed)
        });
        let pressure_geometry = if self.style.brush_mode == BrushMode::PressureSensitive {
            let retain_profiles = self.pressure_segments.len();
            self.append_pending_pressure_profiles();
            if self.pressure_segments.is_empty() {
                self.pressure_segments.push(
                    self.pressure_capture
                        .profile(0.0, self.pressure_capture.distance),
                );
            }
            if self.reverse_output {
                reverse_pressure_profiles(&mut self.pressure_segments);
                if preview_closed {
                    self.pressure_segments.rotate_left(1);
                }
            }
            let previous_pressure = self
                .preview
                .as_ref()
                .and_then(|preview| preview.pressure_geometry.as_deref());
            let pressure = Arc::new(pressure_path_geometry(
                self.geometry_revision,
                &geometry,
                &self.pressure_segments,
                self.style.stroke_width,
                self.style.stroke_style,
                previous_pressure,
                if self.reverse_output
                    || preview_closed
                    || previous.is_some_and(|geometry| geometry.closed)
                {
                    0
                } else {
                    first_changed_command
                        .saturating_sub(1)
                        .min(self.preview_committed_count.saturating_sub(4))
                },
            ));
            if self.reverse_output {
                if preview_closed {
                    self.pressure_segments.rotate_right(1);
                }
                reverse_pressure_profiles(&mut self.pressure_segments);
            }
            self.pressure_segments.truncate(retain_profiles);
            Some(pressure)
        } else {
            None
        };
        self.preview_committed_count = self.committed_vertices.len();
        self.preview = Some(Arc::new(FreeDrawPreview {
            geometry,
            brush_mode: self.style.brush_mode,
            pressure_geometry,
            stroke: self.style.stroke,
            stroke_width: self.style.stroke_width,
            stroke_style: self.style.stroke_style,
            fill: self.style.fill,
            fill_style: self.style.fill_style,
            opacity: self.style.opacity,
        }));
    }

    fn append_pending_pressure_profiles(&mut self) {
        let mut points = self.pending_samples.clone();
        let mut distances = self.pending_distances.clone();
        if !self.shift_active {
            let endpoint = self.point_filter.last_raw();
            if points
                .last()
                .is_none_or(|last| distance(*last, endpoint) > 1e-12)
            {
                points.push(endpoint);
                distances.push(self.pressure_capture.distance);
            }
        }
        let mut previous = self.committed_vertices.last().copied();
        let mut begin = self.committed_distances.last().copied().unwrap_or(0.0);
        for index in simplify_rdp_indices(&points, self.rdp_tolerance) {
            if previous.is_some_and(|point| distance(point, points[index]) <= 1e-12) {
                continue;
            }
            if previous.is_some() {
                self.pressure_segments
                    .push(self.pressure_capture.profile(begin, distances[index]));
            }
            begin = distances[index];
            previous = Some(points[index]);
        }
    }

    pub(crate) fn finish(mut self) -> Option<(FreeDrawData, Arc<PathGeometry>)> {
        self.defer_preview = false;
        if !self.shift_active
            && let Some(endpoint) = self.point_filter.settle_endpoint()
        {
            self.push_pending_sample(endpoint);
            self.advance_revision();
            self.rebuild_preview();
        }
        let preview_geometry = self
            .preview
            .as_ref()
            .map(|preview| preview.geometry.clone());
        self.flush_pending();
        let &start = self.committed_vertices.first()?;
        if self.committed_vertices.len() == 1 {
            self.committed_vertices
                .push(Point::new(start.x + DOT_SEGMENT_LENGTH, start.y));
            self.committed_modes.push(PathSegmentMode::Curve);
            self.pressure_segments.push(
                self.pressure_capture
                    .profile(0.0, self.pressure_capture.distance),
            );
        }
        let mut closed = false;
        if self.committed_vertices.len() >= 4
            && distance(self.committed_vertices[0], *self.committed_vertices.last()?)
                <= self.closure_radius
        {
            self.committed_vertices.pop();
            if self.committed_modes.len() >= self.committed_vertices.len() {
                self.committed_modes.pop();
            }
            if self.committed_vertices.len() >= 3 {
                closed = true;
                self.committed_modes.push(if self.shift_active {
                    PathSegmentMode::Straight
                } else {
                    PathSegmentMode::Curve
                });
            }
        }
        if self.reverse_output {
            self.committed_vertices.reverse();
            self.committed_modes.reverse();
            reverse_pressure_profiles(&mut self.pressure_segments);
            if closed {
                self.committed_modes.rotate_left(1);
                self.pressure_segments.rotate_left(1);
            }
        }
        let mut data = FreeDrawData::from_global_vertices(
            &self.committed_vertices,
            self.committed_modes,
            closed,
            FreeDrawStyle {
                brush_mode: self.style.brush_mode,
                stroke: self.style.stroke,
                stroke_width: self.style.stroke_width,
                stroke_style: self.style.stroke_style,
                fill: self.style.fill,
                fill_style: self.style.fill_style,
                opacity: self.style.opacity,
            },
        )?;
        data.pressure_segments = self.pressure_segments;
        snow_draw_engine_document::validate_free_draw(&data).ok()?;
        let geometry = if closed {
            Arc::new(data.path_geometry(self.geometry_revision))
        } else {
            preview_geometry.unwrap_or_else(|| Arc::new(data.path_geometry(self.geometry_revision)))
        };
        Some((data, geometry))
    }

    #[cfg(test)]
    fn simplified_vertices_and_modes(&self) -> (Vec<Point<f64>>, Vec<PathSegmentMode>) {
        self.simplified_tail_vertices_and_modes(0)
    }

    fn simplified_tail_vertices_and_modes(
        &self,
        vertex_start: usize,
    ) -> (Vec<Point<f64>>, Vec<PathSegmentMode>) {
        let vertex_start = vertex_start.min(self.committed_vertices.len());
        let mut vertices = self.committed_vertices[vertex_start..].to_vec();
        let mut modes =
            self.committed_modes[vertex_start.min(self.committed_modes.len())..].to_vec();
        let mut tail_samples = self.pending_samples.clone();
        if !self.shift_active {
            let endpoint = self.point_filter.last_raw();
            if tail_samples
                .last()
                .is_none_or(|last| distance(*last, endpoint) > 1e-12)
            {
                tail_samples.push(endpoint);
            }
        }
        let simplified = simplify_rdp(&tail_samples, self.rdp_tolerance);
        for point in simplified {
            if vertices
                .last()
                .is_some_and(|last| distance(*last, point) <= 1e-12)
            {
                continue;
            }
            if !vertices.is_empty() {
                modes.push(PathSegmentMode::Curve);
            }
            vertices.push(point);
        }
        (vertices, modes)
    }

    fn last_point(&self) -> Point<f64> {
        self.pending_samples
            .last()
            .copied()
            .or_else(|| self.committed_vertices.last().copied())
            .unwrap_or_default()
    }

    fn first_point(&self) -> Option<Point<f64>> {
        self.committed_vertices
            .first()
            .copied()
            .or_else(|| self.pending_samples.first().copied())
    }

    fn advance_revision(&mut self) {
        self.geometry_revision = self.geometry_revision.wrapping_add(1).max(1);
    }

    #[cfg(test)]
    fn pending_len(&self) -> usize {
        self.pending_samples.len()
    }
    #[cfg(test)]
    fn committed_vertices(&self) -> &[Point<f64>] {
        &self.committed_vertices
    }
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct FreeDrawContinuation {
    id: ElementId,
    original: FreeDrawData,
    meta: ElementMeta,
    at_start: bool,
    endpoint: Point<f64>,
    press: Point<f64>,
    moved: bool,
}

impl FreeDrawContinuation {
    fn valid(&self, document: &DocumentModel) -> bool {
        document
            .free_draw(self.id)
            .is_ok_and(|data| data == &self.original)
            && document
                .element(self.id)
                .is_ok_and(|element| element.meta == self.meta)
    }

    fn sample(&mut self, point: Point<f64>) -> Point<f64> {
        self.moved |= distance(point, self.press) > 1e-9;
        if self.moved { point } else { self.endpoint }
    }
}

impl Editor {
    pub(crate) fn resolve_free_draw_endpoint(
        &self,
        document: &DocumentModel,
        point: Point<f64>,
    ) -> Option<(ElementId, bool, Point<f64>)> {
        if self.state.active_tool != ActiveTool::FreeDraw
            || !matches!(self.state.interaction, InteractionState::Idle)
        {
            return None;
        }
        let zoom = self.camera().zoom.max(1e-6);
        // A small viewport query uses the document spatial index and returns paint order.
        let candidates = document.visible_element_ids(snow_draw_engine_core::ViewportQuery {
            camera: snow_draw_engine_core::Camera {
                center: point,
                zoom,
            },
            surface: snow_draw_engine_core::SurfaceSize {
                width: 18,
                height: 18,
            },
        });
        let mut best = None;
        let mut best_distance = 8.0 / zoom;
        for id in candidates.into_iter().rev() {
            let Ok(element) = document.element(id) else {
                continue;
            };
            if !element.meta.visible || element.meta.locked {
                continue;
            }
            let Ok(data) = document.free_draw(id) else {
                continue;
            };
            if data.closed {
                continue;
            }
            let vertices = data.global_vertices();
            for (at_start, endpoint) in [(false, vertices.last()), (true, vertices.first())] {
                let Some(&endpoint) = endpoint else {
                    continue;
                };
                let d = distance(point, endpoint);
                if d <= best_distance && (best.is_none() || d < best_distance) {
                    best_distance = d;
                    best = Some((id, at_start, endpoint));
                }
            }
        }
        best
    }

    pub(crate) fn free_draw_endpoint_feedback(
        &self,
        document: &DocumentModel,
    ) -> Option<Point<f64>> {
        let position = self.state.ui.free_draw_hover_position?;
        let point = view_to_canvas(position, &self.camera(), self.surface_size());
        self.resolve_free_draw_endpoint(document, point)
            .map(|(_, _, endpoint)| endpoint)
    }

    pub(crate) fn begin_free_draw_continuation(
        &mut self,
        document: &DocumentModel,
        event: PointerEvent,
        (id, at_start, endpoint): (ElementId, bool, Point<f64>),
    ) {
        let original = document.free_draw(id).expect("resolved endpoint").clone();
        let meta = document.element(id).expect("resolved endpoint").meta;
        let builder =
            StreamingFreeDrawBuilder::continue_from(&original, at_start, self.camera().zoom);
        let press = view_to_canvas(event.position, &self.camera(), self.surface_size());
        self.set_hovered_element(None);
        self.state.interaction = InteractionState::CreatingFreeDraw(CreateFreeDrawState {
            pointer_id: event.pointer_id,
            builder,
            continuation: Some(FreeDrawContinuation {
                id,
                original,
                meta,
                at_start,
                endpoint,
                press,
                moved: false,
            }),
        });
        self.state.creation_preview = None;
        self.bump_scene_state_revision();
    }

    pub(crate) fn free_draw_replacement_preview(
        &self,
        document: &DocumentModel,
    ) -> Option<(ElementId, Arc<FreeDrawPreview>)> {
        let InteractionState::CreatingFreeDraw(state) = &self.state.interaction else {
            return None;
        };
        let target = state.continuation.as_ref()?;
        if !target.valid(document) {
            return None;
        }
        Some((target.id, state.builder.preview()?))
    }
}

impl Editor {
    pub(crate) fn begin_free_draw_creation(
        &mut self,
        event: PointerEvent,
        start_canvas_position: Point<f64>,
    ) {
        let style = self.state.default_free_draw_style;
        self.set_hovered_element(None);
        self.state.interaction = InteractionState::CreatingFreeDraw(CreateFreeDrawState {
            pointer_id: event.pointer_id,
            continuation: None,
            builder: StreamingFreeDrawBuilder::new(
                start_canvas_position,
                self.camera().zoom,
                style,
            ),
        });
        if let InteractionState::CreatingFreeDraw(state) = &mut self.state.interaction {
            state.builder.pressure_capture = crate::free_draw_pressure::PressureCapture::new(
                start_canvas_position,
                event.pressure,
                event.timestamp_ms,
                style.stroke_width,
                state.builder.point_filter.sample_spacing(),
            );
            if style.brush_mode == BrushMode::PressureSensitive {
                state.builder.request_preview();
            }
        }
        self.state.creation_preview = None;
        self.bump_scene_state_revision();
    }

    pub(crate) fn process_free_draw_creation_pointer_event(
        &mut self,
        document: &DocumentModel,
        event: PointerEvent,
    ) -> Result<InteractionOutput, ErrorCode> {
        if let InteractionState::CreatingFreeDraw(state) = &self.state.interaction
            && state
                .continuation
                .as_ref()
                .is_some_and(|target| !target.valid(document))
        {
            return self.handle_free_draw_pointer_cancel(event);
        }
        match event.event_type {
            PointerEventType::Move | PointerEventType::Enter => {
                self.handle_free_draw_pointer_move(event)
            }
            PointerEventType::Up => self.handle_free_draw_pointer_up(document, event),
            PointerEventType::Cancel => self.handle_free_draw_pointer_cancel(event),
            PointerEventType::Down | PointerEventType::DoubleClick | PointerEventType::Leave => {
                Ok(InteractionOutput::default())
            }
        }
    }

    fn handle_free_draw_pointer_move(
        &mut self,
        event: PointerEvent,
    ) -> Result<InteractionOutput, ErrorCode> {
        let point = view_to_canvas(event.position, &self.camera(), self.surface_size());
        let InteractionState::CreatingFreeDraw(state) = &mut self.state.interaction else {
            return Ok(InteractionOutput::default());
        };
        if state.pointer_id != event.pointer_id {
            return Ok(InteractionOutput::default());
        }
        let point = state
            .continuation
            .as_mut()
            .map_or(point, |target| target.sample(point));
        let changed = state.builder.append_input(
            point,
            event.modifiers.shift,
            event.pressure,
            event.timestamp_ms,
        );
        if changed {
            self.bump_scene_state_revision();
        }
        Ok(InteractionOutput {
            consumed: true,
            capture: PointerCaptureCommand::NoChange,
            cursor: CursorCommand::Set(CursorStyle::Crosshair),
        })
    }

    fn handle_free_draw_pointer_up(
        &mut self,
        document: &DocumentModel,
        event: PointerEvent,
    ) -> Result<InteractionOutput, ErrorCode> {
        let point = view_to_canvas(event.position, &self.camera(), self.surface_size());
        let interaction = std::mem::replace(&mut self.state.interaction, InteractionState::Idle);
        let InteractionState::CreatingFreeDraw(mut state) = interaction else {
            return Ok(InteractionOutput::default());
        };
        if state.pointer_id != event.pointer_id {
            self.state.interaction = InteractionState::CreatingFreeDraw(state);
            return Ok(InteractionOutput::default());
        }
        let point = state
            .continuation
            .as_mut()
            .map_or(point, |target| target.sample(point));
        // Tablet release normally reports zero after contact has ended.
        let pressure = event.pressure.filter(|pressure| *pressure > 0.0);
        state.builder.append_input(
            point,
            event.modifiers.shift,
            pressure.or(Some(state.builder.pressure_capture.pressure)),
            event.timestamp_ms,
        );
        let finalized = if state
            .continuation
            .as_ref()
            .is_some_and(|target| !target.moved)
        {
            None
        } else {
            state.builder.finish()
        };
        self.cancel_interaction();
        self.bump_scene_state_revision();
        if let Some((free_draw, _geometry)) = finalized {
            let mut transaction = Transaction::new(if state.continuation.is_some() {
                "continue free draw"
            } else {
                "create free draw"
            });
            if let Some(target) = state.continuation {
                transaction.update_free_draw(target.id, free_draw);
            } else {
                transaction.insert_free_draw(
                    document.peek_next_element_id(),
                    ElementMeta::default(),
                    free_draw,
                );
            }
            self.queue_command(EditorCommand::ApplyTransaction(
                ApplyTransactionCommand::new(transaction),
            ));
        }
        Ok(InteractionOutput {
            consumed: true,
            capture: PointerCaptureCommand::Release,
            cursor: CursorCommand::Set(CursorStyle::Crosshair),
        })
    }

    fn handle_free_draw_pointer_cancel(
        &mut self,
        event: PointerEvent,
    ) -> Result<InteractionOutput, ErrorCode> {
        let InteractionState::CreatingFreeDraw(state) = &self.state.interaction else {
            return Ok(InteractionOutput::default());
        };
        if state.pointer_id != event.pointer_id {
            return Ok(InteractionOutput::default());
        }
        self.cancel_interaction();
        self.bump_scene_state_revision();
        Ok(InteractionOutput {
            consumed: true,
            capture: PointerCaptureCommand::Release,
            cursor: CursorCommand::Set(CursorStyle::Crosshair),
        })
    }

    pub(crate) fn free_draw_creation_preview(&self) -> Option<ElementCreationPreview> {
        let InteractionState::CreatingFreeDraw(state) = &self.state.interaction else {
            return None;
        };
        if state.continuation.is_some() {
            return None;
        }
        state
            .builder
            .preview()
            .map(ElementCreationPreview::FreeDraw)
    }

    pub fn defer_free_draw_preview(&mut self, defer: bool) {
        if let InteractionState::CreatingFreeDraw(state) = &mut self.state.interaction {
            state.builder.defer_preview = defer;
            if !defer && state.builder.preview_dirty {
                state.builder.rebuild_preview();
            }
        }
    }
}

fn simplify_rdp(points: &[Point<f64>], tolerance: f64) -> Vec<Point<f64>> {
    simplify_rdp_indices(points, tolerance)
        .into_iter()
        .map(|index| points[index])
        .collect()
}

fn simplify_rdp_indices(points: &[Point<f64>], tolerance: f64) -> Vec<usize> {
    if points.len() <= 2 {
        return (0..points.len()).collect();
    }
    let mut keep = vec![false; points.len()];
    keep[0] = true;
    keep[points.len() - 1] = true;
    let mut stack = vec![(0usize, points.len() - 1)];
    while let Some((start, end)) = stack.pop() {
        let mut maximum = tolerance;
        let mut furthest = None;
        for index in start + 1..end {
            let distance = distance_to_segment(points[index], points[start], points[end]);
            if distance > maximum {
                maximum = distance;
                furthest = Some(index);
            }
        }
        if let Some(index) = furthest {
            keep[index] = true;
            stack.push((start, index));
            stack.push((index, end));
        }
    }
    keep.into_iter()
        .enumerate()
        .filter_map(|(index, keep)| keep.then_some(index))
        .collect()
}

fn distance_to_segment(point: Point<f64>, start: Point<f64>, end: Point<f64>) -> f64 {
    let dx = end.x - start.x;
    let dy = end.y - start.y;
    let length_sq = dx * dx + dy * dy;
    if length_sq <= 1e-12 {
        return distance(point, start);
    }
    let t = (((point.x - start.x) * dx + (point.y - start.y) * dy) / length_sq).clamp(0.0, 1.0);
    distance(point, Point::new(start.x + dx * t, start.y + dy * t))
}

fn distance(left: Point<f64>, right: Point<f64>) -> f64 {
    (right.x - left.x).hypot(right.y - left.y)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn pressure_shift_stroke_preserves_moving_and_stationary_peaks() {
        for mode in [BrushMode::Regular, BrushMode::PressureSensitive] {
            let mut style = crate::defaults::editor_style_defaults().free_draw;
            style.brush_mode = mode;
            let mut builder = StreamingFreeDrawBuilder::new(Point::default(), 1.0, style);
            for (index, (x, pressure)) in [(20.0, 0.2), (60.0, 0.9), (60.0, 0.2), (140.0, 0.2)]
                .into_iter()
                .enumerate()
            {
                builder.append_input(
                    Point::new(x, 0.0),
                    true,
                    Some(pressure),
                    100 + index as u64 * 8,
                );
            }
            let preview = builder.preview().unwrap();
            let (data, _) = builder.finish().unwrap();
            assert_eq!(data.vertices.len(), 2);
            assert_eq!(data.segment_modes, [PathSegmentMode::Straight]);
            assert!(
                data.pressure_segments[0]
                    .iter()
                    .any(|knot| knot.pressure == 0.9)
            );
            assert!(snow_draw_engine_core::pressure_at(&data.pressure_segments[0], 1.0) < 0.3);
            if mode == BrushMode::PressureSensitive {
                let pressure = data.pressure_geometry(2, &data.path_geometry(2)).unwrap();
                assert_eq!(
                    preview
                        .pressure_geometry
                        .as_ref()
                        .unwrap()
                        .flattened_commands(),
                    pressure.flattened_commands()
                );
                assert!(snow_draw_engine_document::free_draw_hit_test(
                    &data,
                    Point::new(
                        60.0,
                        snow_draw_engine_core::pressure_radius(style.stroke_width, 0.8)
                    ),
                    0.0
                ));
            }
        }
    }

    #[test]
    fn pressure_shift_sub_spacing_input_updates_preview_and_committed_profile() {
        let mut style = crate::defaults::editor_style_defaults().free_draw;
        style.brush_mode = BrushMode::PressureSensitive;
        style.stroke_width = 16.0;
        let mut builder = StreamingFreeDrawBuilder::new(Point::default(), 1.0, style);
        builder.append_input(Point::new(20.0, 0.0), true, Some(0.2), 100);
        builder.append_input(Point::new(20.1, 0.0), true, Some(0.9), 108);
        assert!(
            builder
                .preview()
                .unwrap()
                .pressure_geometry
                .as_ref()
                .unwrap()
                .canvas_bounds[3]
                >= snow_draw_engine_core::pressure_radius(style.stroke_width, 0.9)
        );
        builder.append_input(Point::new(20.1, 0.0), true, Some(0.2), 116);
        builder.append_input(Point::new(40.0, 0.0), true, Some(0.2), 124);
        let (data, _) = builder.finish().unwrap();
        assert!(
            data.pressure_segments[0]
                .iter()
                .any(|knot| knot.pressure == 0.9)
        );
    }

    #[test]
    fn pressure_shift_continuation_keeps_historical_profiles_and_new_peaks() {
        let mut style = crate::defaults::editor_style_defaults().free_draw;
        style.brush_mode = BrushMode::PressureSensitive;
        let mut original = StreamingFreeDrawBuilder::new(Point::default(), 1.0, style);
        original.append_input(Point::new(40.0, 0.0), true, Some(0.2), 100);
        original.append_input(Point::new(100.0, 0.0), true, Some(0.4), 108);
        let (data, _) = original.finish().unwrap();
        for at_start in [false, true] {
            let mut builder = StreamingFreeDrawBuilder::continue_from(&data, at_start, 1.0);
            let endpoint = builder.last_point();
            let direction = if at_start { -1.0 } else { 1.0 };
            for (index, (offset, pressure)) in [(20.0, 0.2), (60.0, 0.9), (100.0, 0.2)]
                .into_iter()
                .enumerate()
            {
                builder.append_input(
                    Point::new(endpoint.x + direction * offset, endpoint.y),
                    true,
                    Some(pressure),
                    200 + index as u64 * 8,
                );
            }
            let preview = builder.preview().unwrap();
            let (continued, _) = builder.finish().unwrap();
            let historical = usize::from(at_start);
            let added = usize::from(!at_start);
            assert_eq!(
                continued.pressure_segments[historical],
                data.pressure_segments[0]
            );
            assert!(
                continued.pressure_segments[added]
                    .iter()
                    .any(|knot| knot.pressure == 0.9)
            );
            let pressure = continued
                .pressure_geometry(2, &continued.path_geometry(2))
                .unwrap();
            assert_eq!(
                preview
                    .pressure_geometry
                    .as_ref()
                    .unwrap()
                    .flattened_commands(),
                pressure.flattened_commands()
            );
        }
    }

    #[test]
    fn pressure_deferred_moves_rebuild_preview_once_at_editor_flush() {
        let document = DocumentModel::new();
        let mut editor = Editor::new(EngineConfig::default()).unwrap();
        editor.set_surface_size(320, 200).unwrap();
        editor
            .set_camera(snow_draw_engine_core::Camera {
                center: Point::new(160.0, 100.0),
                zoom: 1.0,
            })
            .unwrap();
        editor.state.default_free_draw_style.brush_mode = BrushMode::PressureSensitive;
        let mut event = PointerEvent {
            pointer_id: 7,
            event_type: PointerEventType::Down,
            device: snow_draw_engine_interaction::PointerDevice::Pen,
            position: Point::new(20.0, 100.0),
            button: Some(snow_draw_engine_interaction::PointerButton::Primary),
            buttons: snow_draw_engine_interaction::PointerButtons(1),
            modifiers: Default::default(),
            pressure: Some(0.2),
            timestamp_ms: 1,
        };
        editor.begin_free_draw_creation(event, event.position);
        let preview = |editor: &Editor| {
            let Some(ElementCreationPreview::FreeDraw(preview)) =
                editor.free_draw_creation_preview()
            else {
                panic!("pressure creation must retain a preview");
            };
            preview
        };
        let before = preview(&editor);
        let before_pressure = before.pressure_geometry.as_ref().unwrap();
        editor.defer_free_draw_preview(true);
        event.event_type = PointerEventType::Move;
        event.button = None;
        for index in 1..=32 {
            event.position = Point::new(20.0 + f64::from(index) * 3.0, 100.0);
            event.pressure = Some(0.2 + f64::from(index) / 50.0);
            event.timestamp_ms = 1 + index as u64 * 8;
            editor
                .process_free_draw_creation_pointer_event(&document, event)
                .unwrap();
            let deferred = preview(&editor);
            assert!(Arc::ptr_eq(&before, &deferred));
            assert!(Arc::ptr_eq(
                before_pressure,
                deferred.pressure_geometry.as_ref().unwrap()
            ));
        }
        editor.defer_free_draw_preview(false);
        let after = preview(&editor);
        assert!(!Arc::ptr_eq(&before, &after));
        assert!(!Arc::ptr_eq(
            before_pressure,
            after.pressure_geometry.as_ref().unwrap()
        ));
        assert!(after.geometry.revision > before.geometry.revision);
        editor.defer_free_draw_preview(true);
        editor.defer_free_draw_preview(false);
        assert!(Arc::ptr_eq(&after, &preview(&editor)));
    }

    #[test]
    fn pressure_prepend_closure_rotates_profiles_with_segments() {
        let mut data = FreeDrawData::from_global_vertices(
            &[
                Point::new(10.0, 10.0),
                Point::new(30.0, 10.0),
                Point::new(30.0, 30.0),
                Point::new(10.0, 30.0),
            ],
            vec![PathSegmentMode::Straight; 3],
            false,
            FreeDrawStyle {
                brush_mode: BrushMode::PressureSensitive,
                stroke: Default::default(),
                stroke_width: 4.0,
                stroke_style: snow_draw_engine_document::StrokeStyle::Solid,
                fill: Default::default(),
                fill_style: snow_draw_engine_document::FillStyle::Solid,
                opacity: 1.0,
            },
        )
        .unwrap();
        data.pressure_segments = [0.2, 0.4, 0.6]
            .into_iter()
            .map(|pressure| {
                vec![
                    PressureKnot { t: 0.0, pressure },
                    PressureKnot { t: 1.0, pressure },
                ]
            })
            .collect();
        let mut builder = StreamingFreeDrawBuilder::continue_from(&data, true, 1.0);
        builder.append_input(Point::new(10.0, 30.0), true, Some(0.8), 100);
        let preview = builder.preview().unwrap();
        assert!(preview.geometry.closed);
        let (closed, _) = builder.finish().unwrap();
        assert!(closed.closed);
        assert_eq!(&closed.pressure_segments[..3], data.pressure_segments);
        let pressure = closed
            .pressure_geometry(3, &closed.path_geometry(3))
            .unwrap();
        assert!(
            preview
                .pressure_geometry
                .as_ref()
                .unwrap()
                .flattened_commands()
                == pressure.flattened_commands()
        );
    }

    #[test]
    fn pressure_continuation_and_selection_transform_preserve_profiles() {
        let mut style = crate::defaults::editor_style_defaults().free_draw;
        style.brush_mode = BrushMode::PressureSensitive;
        let mut original = StreamingFreeDrawBuilder::new(Point::default(), 1.0, style);
        for i in 1..=50 {
            original.append_input(
                Point::new(i as f64 * 3.0, (i as f64 * 0.2).sin() * 10.0),
                false,
                Some(0.2 + (i % 7) as f64 / 10.0),
                100 + i * 8,
            );
        }
        let (data, _) = original.finish().unwrap();
        for at_start in [false, true] {
            let mut builder = StreamingFreeDrawBuilder::continue_from(&data, at_start, 1.0);
            let endpoint = builder.last_point();
            builder.append_input(
                Point::new(endpoint.x + if at_start { -30.0 } else { 30.0 }, endpoint.y),
                true,
                Some(0.6),
                900,
            );
            let (continued, _) = builder.finish().unwrap();
            let historical = if at_start {
                &continued.pressure_segments[1..]
            } else {
                &continued.pressure_segments[..data.pressure_segments.len()]
            };
            assert_eq!(historical.len(), data.pressure_segments.len());
            for (before, after) in data.pressure_segments.iter().zip(historical) {
                assert_eq!(before.len(), after.len());
                for (before, after) in before.iter().zip(after) {
                    assert!((before.t - after.t).abs() < 1e-12);
                    assert_eq!(before.pressure, after.pressure);
                }
            }
        }
        let mut document = DocumentModel::new();
        let id = document.peek_next_element_id();
        let mut insert = Transaction::new("pressure fixture");
        insert.insert_free_draw(id, ElementMeta::default(), data.clone());
        document.apply_transaction(insert).unwrap();
        let mut rect = document.element_rect_proxy(id).unwrap();
        rect.center = Point::new(200.0, 100.0);
        rect.width *= 1.8;
        rect.height *= 0.4;
        rect.rotation = 0.7;
        let mut update = Transaction::new("pressure transform");
        crate::document_ops::append_selection_element_update(
            &mut update,
            &document,
            SelectionRectState { id, rect },
            None,
            false,
            None,
        )
        .unwrap();
        document.apply_transaction(update).unwrap();
        let transformed = document.free_draw(id).unwrap();
        assert_eq!(transformed.pressure_segments, data.pressure_segments);
        assert_eq!(transformed.brush_mode, data.brush_mode);
        assert_eq!(transformed.stroke_width, data.stroke_width);
    }

    #[test]
    fn pressure_mode_is_a_free_draw_only_mixed_property() {
        let mut document = DocumentModel::new();
        let mut ids = Vec::new();
        for mode in [BrushMode::Regular, BrushMode::PressureSensitive] {
            let mut style = crate::defaults::editor_style_defaults().free_draw;
            style.brush_mode = mode;
            let mut builder = StreamingFreeDrawBuilder::new(Point::default(), 1.0, style);
            builder.append_input(Point::new(100.0, 0.0), false, Some(0.6), 100);
            let (data, _) = builder.finish().unwrap();
            let id = document.peek_next_element_id();
            ids.push(id);
            let mut insert = Transaction::new("mixed pressure fixture");
            insert.insert_free_draw(id, ElementMeta::default(), data);
            document.apply_transaction(insert).unwrap();
        }
        let mut editor = Editor::new(EngineConfig::default()).unwrap();
        editor.set_active_tool(ActiveTool::FreeDraw).unwrap();
        editor.set_selection_state_with_document(Some(&document), ids.clone(), Some(ids[0]));
        assert_eq!(
            editor.shape_style_mixed(&document) & crate::SHAPE_STYLE_MIXED_BRUSH_MODE,
            crate::SHAPE_STYLE_MIXED_BRUSH_MODE
        );
        assert_eq!(
            ShapeKind::Line.supported_properties() & crate::SHAPE_STYLE_PROPERTY_BRUSH_MODE,
            0
        );
        assert_ne!(
            ShapeKind::FreeDraw.supported_properties() & crate::SHAPE_STYLE_PROPERTY_BRUSH_MODE,
            0
        );
    }

    #[test]
    fn pressure_chunk_boundaries_match_full_rebuild() {
        let mut style = crate::defaults::editor_style_defaults().free_draw;
        style.brush_mode = BrushMode::PressureSensitive;
        let mut builder = StreamingFreeDrawBuilder::new(Point::default(), 1.0, style);
        let mut checked = 0;
        for i in 1..=500 {
            builder.append_input(
                Point::new(i as f64 * 3.0, if i % 2 == 0 { 12.0 } else { -12.0 }),
                false,
                Some(0.2 + (i % 7) as f64 / 10.0),
                100 + i * 8,
            );
            let changed_command = builder.committed_vertices.len().saturating_sub(3);
            if changed_command > 0 && changed_command.is_multiple_of(16) {
                let preview = builder.preview().unwrap();
                let retained = builder.pressure_segments.len();
                builder.append_pending_pressure_profiles();
                let full = pressure_path_geometry(
                    preview.geometry.revision,
                    &preview.geometry,
                    &builder.pressure_segments,
                    style.stroke_width,
                    style.stroke_style,
                    None,
                    0,
                );
                builder.pressure_segments.truncate(retained);
                let actual = preview
                    .pressure_geometry
                    .as_ref()
                    .unwrap()
                    .flattened_commands();
                let expected = full.flattened_commands();
                assert!(
                    actual == expected,
                    "changed centerline command {changed_command}"
                );
                checked += 1;
            }
        }
        assert!(checked > 0);
    }

    #[test]
    fn pressure_profiles_do_not_change_regular_centerline_and_survive_mode_switch() {
        let regular_style = crate::defaults::editor_style_defaults().free_draw;
        let mut pressure_style = regular_style;
        pressure_style.brush_mode = BrushMode::PressureSensitive;
        let mut regular = StreamingFreeDrawBuilder::new(Point::default(), 1.0, regular_style);
        let mut pressure = StreamingFreeDrawBuilder::new(Point::default(), 1.0, pressure_style);
        for i in 1..=50 {
            let point = Point::new(i as f64 * 2.0, 0.0);
            let factor = if (20..=30).contains(&i) { 0.9 } else { 0.2 };
            regular.append_input(point, false, Some(factor), 100 + i * 8);
            pressure.append_input(point, false, Some(factor), 100 + i * 8);
        }
        let (mut regular_data, _) = regular.finish().unwrap();
        let (pressure_data, _) = pressure.finish().unwrap();
        assert_eq!(regular_data.vertices, pressure_data.vertices);
        assert_eq!(regular_data.segment_modes, pressure_data.segment_modes);
        assert_eq!(
            regular_data.pressure_segments,
            pressure_data.pressure_segments
        );
        assert!(
            regular_data
                .pressure_segments
                .iter()
                .flatten()
                .any(|knot| knot.pressure > 0.85)
        );
        let canonical = regular_data.path_commands();
        regular_data.brush_mode = BrushMode::PressureSensitive;
        assert_eq!(canonical, regular_data.path_commands());
        assert!(
            regular_data
                .pressure_geometry(1, &regular_data.path_geometry(1))
                .is_some()
        );
    }

    #[test]
    fn pressure_only_dot_retains_hold_peak_without_extra_position_vertices() {
        let mut style = crate::defaults::editor_style_defaults().free_draw;
        style.brush_mode = BrushMode::PressureSensitive;
        let mut builder = StreamingFreeDrawBuilder::new(Point::default(), 1.0, style);
        builder.append_input(Point::default(), false, Some(0.9), 100);
        builder.append_input(Point::default(), false, Some(0.2), 108);
        let (data, _) = builder.finish().unwrap();
        assert_eq!(data.vertices.len(), 2);
        assert!(
            data.pressure_segments[0]
                .iter()
                .any(|knot| knot.pressure == 0.9)
        );
        let geometry = data.pressure_geometry(2, &data.path_geometry(2)).unwrap();
        assert!(
            geometry.canvas_bounds[3]
                >= snow_draw_engine_core::pressure_radius(style.stroke_width, 0.9)
        );
    }

    #[test]
    fn pressure_tail_geometry_keeps_frozen_prefix() {
        let mut style = crate::defaults::editor_style_defaults().free_draw;
        style.brush_mode = BrushMode::PressureSensitive;
        let mut builder = StreamingFreeDrawBuilder::new(Point::default(), 1.0, style);
        builder.defer_preview = true;
        for i in 1..=1200 {
            builder.append_input(
                Point::new(i as f64 * 3.0, if i % 2 == 0 { 10.0 } else { -10.0 }),
                false,
                Some(0.2 + (i % 7) as f64 / 10.0),
                100 + i * 8,
            );
        }
        builder.defer_preview = false;
        builder.rebuild_preview();
        let before = builder
            .preview()
            .unwrap()
            .pressure_geometry
            .clone()
            .unwrap();
        builder.append_input(Point::new(3603.0, -10.0), false, Some(0.7), 9808);
        let after = builder
            .preview()
            .unwrap()
            .pressure_geometry
            .clone()
            .unwrap();
        let prefix = before
            .chunks
            .iter()
            .zip(after.chunks.iter())
            .take_while(|(a, b)| Arc::ptr_eq(&a.commands, &b.commands))
            .count();
        assert!(prefix > 0);
        assert!(
            after.chunks.len() - prefix
                <= MAX_PENDING_SAMPLES / snow_draw_engine_core::PRESSURE_SEGMENTS_PER_CHUNK + 4
        );
        assert!(builder.pending_len() <= MAX_PENDING_SAMPLES);
    }

    #[test]
    fn continuation_preserves_long_mixed_history_and_matches_preview() {
        for at_start in [false, true] {
            let vertices: Vec<_> = (0..600)
                .map(|i| Point::new(i as f64 * 3.0, (i as f64 * 0.4).sin() * 20.0))
                .collect();
            let modes: Vec<_> = (0..599)
                .map(|i| {
                    if i % 3 == 0 {
                        PathSegmentMode::Straight
                    } else {
                        PathSegmentMode::Curve
                    }
                })
                .collect();
            let data = FreeDrawData::from_global_vertices(
                &vertices,
                modes.clone(),
                false,
                FreeDrawStyle {
                    brush_mode: Default::default(),
                    stroke: Default::default(),
                    stroke_width: 2.0,
                    stroke_style: snow_draw_engine_document::StrokeStyle::Solid,
                    fill: Default::default(),
                    fill_style: snow_draw_engine_document::FillStyle::Solid,
                    opacity: 0.5,
                },
            )
            .unwrap();
            let mut builder = StreamingFreeDrawBuilder::continue_from(&data, at_start, 1.0);
            let endpoint = if at_start {
                vertices[0]
            } else {
                *vertices.last().unwrap()
            };
            // Shift on an unmoved endpoint must not edit the original terminal segment.
            assert!(!builder.append(endpoint, true));
            let direction = if at_start { -1.0 } else { 1.0 };
            builder.append(Point::new(endpoint.x + direction * 30.0, endpoint.y), true);
            for i in 1..300 {
                builder.append(
                    Point::new(
                        endpoint.x + direction * (30.0 + i as f64),
                        endpoint.y + i as f64 * 0.5,
                    ),
                    false,
                );
            }
            let preview = builder.preview().unwrap();
            let (result, geometry) = builder.finish().unwrap();
            // Normalizing into the new bounds can introduce floating-point rounding.
            fn coordinates(command: &snow_draw_engine_core::PathCommand) -> Vec<f64> {
                use snow_draw_engine_core::PathCommand::*;
                match command {
                    MoveTo { point } | LineTo { point } => point.to_vec(),
                    QuadTo { control, end } => [*control, *end].concat(),
                    CubicTo {
                        control_1,
                        control_2,
                        end,
                    } => [*control_1, *control_2, *end].concat(),
                }
            }
            let expected = result.path_commands();
            for commands in [
                preview.geometry.flattened_commands(),
                geometry.flattened_commands(),
            ] {
                assert_eq!(commands.len(), expected.len());
                for (index, (actual, expected)) in commands.iter().zip(&expected).enumerate() {
                    assert_eq!(
                        std::mem::discriminant(actual),
                        std::mem::discriminant(expected)
                    );
                    for (a, b) in coordinates(actual).iter().zip(coordinates(expected)) {
                        assert!((a - b).abs() < 1e-9, "command {index}: {a} vs {b}");
                    }
                }
            }
            let actual = result.global_vertices();
            let old = if at_start {
                &actual[actual.len() - vertices.len()..]
            } else {
                &actual[..vertices.len()]
            };
            for (a, b) in old.iter().zip(&vertices) {
                assert!(distance(*a, *b) < 1e-9);
            }
            let old_modes = if at_start {
                &result.segment_modes[result.segment_modes.len() - modes.len()..]
            } else {
                &result.segment_modes[..modes.len()]
            };
            assert_eq!(old_modes, modes);
        }
    }

    #[test]
    fn rejects_non_finite_duplicates_and_sub_spacing_samples() {
        let mut style = crate::defaults::editor_style_defaults().free_draw;
        style.stroke_width = 2.0;
        let mut builder = StreamingFreeDrawBuilder::new(Point::new(0.0, 0.0), 1.0, style);
        assert!(!builder.append(Point::new(0.0, 0.0), false));
        assert!(!builder.append(Point::new(f64::NAN, 1.0), false));
        assert!(!builder.append(Point::new(0.25, 0.0), false));
        assert!(builder.append(Point::new(1.0, 0.0), false));
    }

    #[test]
    fn pending_window_is_bounded_and_frozen_prefix_does_not_change() {
        let mut style = crate::defaults::editor_style_defaults().free_draw;
        style.stroke_width = 1.0;
        let mut builder = StreamingFreeDrawBuilder::new(Point::new(0.0, 0.0), 1.0, style);
        for index in 1..1000 {
            builder.append(Point::new(index as f64, (index % 11) as f64), false);
            assert!(builder.pending_len() <= MAX_PENDING_SAMPLES);
        }
        let frozen = builder.committed_vertices().to_vec();
        for index in 1000..1100 {
            builder.append(Point::new(index as f64, (index % 11) as f64), false);
        }
        assert_eq!(
            &builder.committed_vertices()[..frozen.len()],
            frozen.as_slice()
        );
    }

    #[test]
    fn steady_append_reuses_prefix_and_changes_at_most_three_chunks() {
        let mut style = crate::defaults::editor_style_defaults().free_draw;
        style.stroke_width = 1.0;
        let mut builder = StreamingFreeDrawBuilder::new(Point::new(0.0, 0.0), 1.0, style);
        for index in 1..=300 {
            builder.append(
                Point::new(index as f64, if index % 2 == 0 { 10.0 } else { -10.0 }),
                false,
            );
        }
        let before = builder.preview().unwrap().geometry.clone();
        builder.append(Point::new(301.0, -12.0), false);
        let after = builder.preview().unwrap().geometry.clone();
        let unchanged_prefix = before
            .chunks
            .iter()
            .zip(after.chunks.iter())
            .take_while(|(left, right)| Arc::ptr_eq(&left.commands, &right.commands))
            .count();

        assert!(unchanged_prefix > 0);
        assert!(after.chunks.len().saturating_sub(unchanged_prefix) <= 3);
    }

    #[test]
    fn rdp_retains_corners() {
        let points = [
            Point::new(0.0, 0.0),
            Point::new(10.0, 0.0),
            Point::new(10.0, 10.0),
        ];
        assert_eq!(simplify_rdp(&points, 0.5), points);
    }

    #[test]
    fn shift_creates_and_replaces_one_straight_endpoint() {
        let mut style = crate::defaults::editor_style_defaults().free_draw;
        style.stroke_width = 1.0;
        let mut builder = StreamingFreeDrawBuilder::new(Point::new(0.0, 0.0), 1.0, style);
        builder.append(Point::new(5.0, 1.0), false);
        builder.append(Point::new(10.0, 0.0), true);
        builder.append(Point::new(20.0, 0.0), true);
        let (vertices, modes) = builder.simplified_vertices_and_modes();
        assert_eq!(vertices.last(), Some(&Point::new(20.0, 0.0)));
        assert_eq!(modes.last(), Some(&PathSegmentMode::Straight));
    }

    #[test]
    fn finish_closes_without_duplicate_vertex_and_preview_matches_open_final() {
        let mut style = crate::defaults::editor_style_defaults().free_draw;
        style.stroke_width = 1.0;
        style.fill = snow_draw_engine_core::ColorRgba8 {
            r: 255,
            g: 0,
            b: 0,
            a: 255,
        };
        style.fill_style = snow_draw_engine_document::FillStyle::Line;
        let mut open = StreamingFreeDrawBuilder::new(Point::new(0.0, 0.0), 1.0, style);
        open.append(Point::new(20.0, 0.0), false);
        open.append(Point::new(30.0, 10.0), false);
        let preview = open.preview().unwrap();
        let (final_data, final_geometry) = open.finish().unwrap();
        assert_eq!(
            preview.geometry.flattened_commands(),
            final_geometry.flattened_commands()
        );
        let final_vertices = final_data.global_vertices();
        assert_eq!(final_vertices.first(), Some(&Point::new(0.0, 0.0)));
        assert_eq!(final_vertices.last(), Some(&Point::new(30.0, 10.0)));
        assert!(final_vertices.len() >= 3);

        let mut closed = StreamingFreeDrawBuilder::new(Point::new(0.0, 0.0), 1.0, style);
        closed.append(Point::new(20.0, 0.0), false);
        closed.append(Point::new(20.0, 20.0), false);
        closed.append(Point::new(1.0, 1.0), false);
        let preview = closed.preview().unwrap();
        assert!(preview.geometry.closed);
        assert_eq!(
            preview.fill_style,
            snow_draw_engine_document::FillStyle::Line
        );
        closed.append(Point::new(40.0, 40.0), false);
        assert!(!closed.preview().unwrap().geometry.closed);
        closed.append(Point::new(1.0, 1.0), false);
        assert!(closed.preview().unwrap().geometry.closed);
        let (data, _) = closed.finish().unwrap();
        assert!(data.closed);
        assert_ne!(data.vertices.first(), data.vertices.last());
        assert_eq!(data.segment_modes.len(), data.vertices.len());
    }

    #[test]
    fn finish_without_drag_registers_a_dot() {
        let mut style = crate::defaults::editor_style_defaults().free_draw;
        style.stroke_width = 1.0;
        let builder = StreamingFreeDrawBuilder::new(Point::new(12.0, 34.0), 1.0, style);
        let (data, _) = builder
            .finish()
            .expect("a click without movement draws a dot");
        let vertices = data.global_vertices();
        assert_eq!(vertices.len(), 2);
        assert_eq!(vertices[0], Point::new(12.0, 34.0));
        assert!(distance(vertices[0], vertices[1]) <= DOT_SEGMENT_LENGTH);
        assert!(!data.closed);
    }

    #[test]
    fn finish_accepts_strokes_of_any_length() {
        let mut style = crate::defaults::editor_style_defaults().free_draw;
        style.stroke_width = 1.0;
        let mut builder = StreamingFreeDrawBuilder::new(Point::new(0.0, 0.0), 1.0, style);
        builder.append(Point::new(0.5, 0.0), false);
        let (data, _) = builder
            .finish()
            .expect("a short drag still registers a stroke");
        assert_eq!(data.global_vertices().last(), Some(&Point::new(0.5, 0.0)));
    }

    #[test]
    fn finalized_pipeline_keeps_stabilized_history_and_exact_endpoint() {
        let mut style = crate::defaults::editor_style_defaults().free_draw;
        style.stroke_width = 1.0;
        let mut builder = StreamingFreeDrawBuilder::new(Point::new(0.0, 0.0), 1.0, style);
        for x in 1..=120 {
            let y = if x % 2 == 0 { 1.0 } else { -1.0 };
            builder.append(Point::new(x as f64, y), false);
        }

        let (data, _) = builder.finish().unwrap();
        let vertices = data.global_vertices();
        let maximum_interior_deviation = vertices[1..vertices.len() - 1]
            .iter()
            .map(|point| point.y.abs())
            .fold(0.0_f64, f64::max);
        assert!(
            maximum_interior_deviation < 0.5,
            "persisted deviation was {maximum_interior_deviation}"
        );
        assert_eq!(vertices.last(), Some(&Point::new(120.0, 1.0)));
    }

    #[test]
    fn finalized_stroke_preserves_exact_pointer_endpoint() {
        let mut style = crate::defaults::editor_style_defaults().free_draw;
        style.stroke_width = 1.0;
        let endpoint = Point::new(47.25, 13.75);
        let mut builder = StreamingFreeDrawBuilder::new(Point::new(0.0, 0.0), 1.0, style);
        builder.append(Point::new(20.0, 2.0), false);
        builder.append(endpoint, false);

        let (data, _) = builder.finish().unwrap();
        assert_eq!(data.global_vertices().last(), Some(&endpoint));
    }

    #[test]
    fn smoothing_parameters_are_locked_to_screen_space() {
        let mut style = crate::defaults::editor_style_defaults().free_draw;
        style.stroke_width = 1.0;
        let normal = StreamingFreeDrawBuilder::new(Point::new(0.0, 0.0), 1.0, style);
        let zoomed = StreamingFreeDrawBuilder::new(Point::new(0.0, 0.0), 2.0, style);

        assert_eq!(
            normal.point_filter.sample_spacing(),
            zoomed.point_filter.sample_spacing() * 2.0
        );
        assert_eq!(
            normal.point_filter.response_distance(),
            zoomed.point_filter.response_distance() * 2.0
        );
        assert_eq!(normal.rdp_tolerance, zoomed.rdp_tolerance * 2.0);
    }
}
