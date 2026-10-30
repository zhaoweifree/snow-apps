use std::borrow::Cow;
use std::collections::{HashMap, HashSet};
use std::hash::{Hash, Hasher};

use crate::*;
use snow_draw_engine_core::{ErrorCode, Point};
use snow_draw_engine_document::{
    DocumentDelta, DocumentRevision, ElementData, ElementId, HighlightShape, Operation,
    RectangleData, SpeechBubbleData, SpeechBubbleTail, TextData, TextLayoutSize, Transaction,
    speech_bubble_base_width_from_point, speech_bubble_body_contains,
    speech_bubble_canvas_to_local, speech_bubble_default_tail, speech_bubble_grow_size,
    speech_bubble_local_to_canvas, speech_bubble_tail_base_points,
    speech_bubble_with_selection_rect,
};
use snow_draw_engine_model::DocumentModel;

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SpeechBubbleTextConstraints {
    pub body_width: f64,
    pub body_height: f64,
    pub content_width: f64,
    pub content_height: f64,
}

/// Width is resolved before wrapping. Height never participates in the width policy.
pub fn speech_bubble_text_constraints(
    body: RectangleData,
    minimum_width: f64,
    ink_reserve: f64,
    text_height: f64,
) -> Result<SpeechBubbleTextConstraints, ErrorCode> {
    if !body.width.is_finite()
        || !body.height.is_finite()
        || body.width <= 0.0
        || body.height <= 0.0
        || !body.stroke_width.is_finite()
        || body.stroke_width < 0.0
        || [
            body.corner_radii.top_left,
            body.corner_radii.top_right,
            body.corner_radii.bottom_right,
            body.corner_radii.bottom_left,
        ]
        .iter()
        .any(|v| !v.is_finite() || *v < 0.0)
    {
        return Err(ErrorCode::InvalidArgument);
    }
    if [minimum_width, ink_reserve, text_height]
        .iter()
        .any(|v| !v.is_finite() || *v < 0.0)
    {
        return Err(ErrorCode::InvalidArgument);
    }
    let padding = 5.0 + body.stroke_width.max(0.0) / 2.0 + ink_reserve;
    let g = minimum_width.max(1.0);
    let (width, height, cw, ch) = match body.highlight_shape {
        HighlightShape::Ellipse | HighlightShape::Diamond => {
            let factor = if body.highlight_shape == HighlightShape::Ellipse {
                std::f64::consts::FRAC_1_SQRT_2
            } else {
                0.5
            };
            let width = body.width.max((g + 2.0 * padding) / factor);
            let height = body
                .height
                .max((text_height.max(1.0) + 2.0 * padding) / factor);
            (
                width,
                height,
                width * factor - 2.0 * padding,
                height * factor - 2.0 * padding,
            )
        }
        HighlightShape::Rectangle => {
            let radius = [
                body.corner_radii.top_left,
                body.corner_radii.top_right,
                body.corner_radii.bottom_right,
                body.corner_radii.bottom_left,
            ]
            .into_iter()
            .fold(0.0_f64, f64::max);
            let k = 1.0 - std::f64::consts::FRAC_1_SQRT_2;
            let candidate = (g + 2.0 * padding) / (1.0 - k);
            let minimum = if candidate <= 2.0 * radius {
                candidate
            } else {
                g + 2.0 * padding + 2.0 * k * radius
            };
            let width = body.width.max(minimum);
            let inset = padding + k * radius.min(width / 2.0);
            let height = body.height.max(text_height.max(1.0) + 2.0 * inset);
            (width, height, width - 2.0 * inset, height - 2.0 * inset)
        }
    };
    if [width, height, cw, ch]
        .iter()
        .any(|v| !v.is_finite() || *v <= 0.0)
    {
        return Err(ErrorCode::InvalidArgument);
    }
    Ok(SpeechBubbleTextConstraints {
        body_width: width,
        body_height: height,
        content_width: cw.max(1.0),
        content_height: ch.max(1.0),
    })
}

#[derive(Clone, Debug, PartialEq)]
pub struct SpeechBubbleTextLayoutRequest {
    pub owner_id: ElementId,
    pub text_id: ElementId,
    pub body: RectangleData,
    pub text: TextData,
    pub key: u64,
}
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SpeechBubbleTextMeasurement {
    pub key: u64,
    pub size: TextLayoutSize,
    pub minimum_grapheme_width: f64,
    pub text_ink_reserve: f64,
}
#[derive(Clone, Debug)]
struct CachedSpeechBubbleText {
    request: SpeechBubbleTextLayoutRequest,
    bubble: SpeechBubbleData,
    text: TextData,
    needed: bool,
}
#[derive(Clone, Debug, Default)]
pub(crate) struct SpeechBubbleTextCache {
    revision: Option<DocumentRevision>,
    generation: u64,
    entries: HashMap<ElementId, CachedSpeechBubbleText>,
    dirty: HashSet<ElementId>,
    pending: HashSet<ElementId>,
    needed: HashSet<ElementId>,
    pub(crate) builds: u64,
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct SpeechBubbleTailDrag {
    pointer_id: u32,
    owner_id: ElementId,
    original: SpeechBubbleData,
    preview: SpeechBubbleData,
    initial_tail: SpeechBubbleTail,
    base: bool,
    offset: Point<f64>,
    start_anchor: Point<f64>,
}

#[derive(Clone, Copy)]
enum SpeechBubbleControlTarget {
    AddTail,
    TailBase(u64),
    TailTip(u64),
}

impl SpeechBubbleControlTarget {
    fn presentation_kind(self) -> u8 {
        match self {
            Self::AddTail => 0,
            Self::TailBase(_) => 1,
            Self::TailTip(_) => 2,
        }
    }
}

struct SpeechBubbleControl {
    target: SpeechBubbleControlTarget,
    center: Point<f64>,
    anchor: Point<f64>,
}

fn key(body: &RectangleData, text: &TextData, generation: u64) -> u64 {
    let mut hash = std::collections::hash_map::DefaultHasher::new();
    text.text.hash(&mut hash);
    text.font_family.hash(&mut hash);
    text.bold.hash(&mut hash);
    text.italic.hash(&mut hash);
    text.font_size.to_bits().hash(&mut hash);
    (text.horizontal_align as u8).hash(&mut hash);
    text.stroke_width.to_bits().hash(&mut hash);
    text.stroke.a.hash(&mut hash);
    text.fill.a.hash(&mut hash);
    (text.fill_style as u8).hash(&mut hash);
    body.width.to_bits().hash(&mut hash);
    (body.highlight_shape as u8).hash(&mut hash);
    body.stroke_width.to_bits().hash(&mut hash);
    for value in [
        body.corner_radii.top_left,
        body.corner_radii.top_right,
        body.corner_radii.bottom_right,
        body.corner_radii.bottom_left,
    ] {
        value.to_bits().hash(&mut hash);
    }
    generation.hash(&mut hash);
    hash.finish()
}

impl Editor {
    pub fn speech_bubble_document_layout_key(
        &self,
        document: &DocumentModel,
        owner: ElementId,
    ) -> Option<(ElementId, u64)> {
        let bubble = document.speech_bubble(owner).ok()?;
        let text_id = bubble.text_element_id?;
        let text = document.text(text_id).ok()?;
        Some((
            text_id,
            key(
                &bubble.body,
                text,
                self.state.arrow_text_measurement_generation,
            ),
        ))
    }

    pub fn speech_bubble_measured_document_values(
        &self,
        document: &DocumentModel,
        owner: ElementId,
    ) -> Option<(SpeechBubbleData, TextData)> {
        let (text_id, current_key) = self.speech_bubble_document_layout_key(document, owner)?;
        let measurement = self
            .speech_bubble_layouts
            .get(&text_id)
            .filter(|value| value.key == current_key)?;
        let bubble = document.speech_bubble(owner).ok()?.clone();
        let text = document.text(text_id).ok()?.clone();
        Some(self.measured_speech_bubble(bubble, text, *measurement))
    }

    pub fn speech_bubble_control_at(&self, document: &DocumentModel, point: Point<f64>) -> bool {
        let Some(&id) = self
            .state
            .selection
            .ids
            .first()
            .filter(|_| self.state.selection.ids.len() == 1)
        else {
            return false;
        };
        if document.speech_bubble(id).is_err() {
            return false;
        }
        let tolerance = selection_handle_hit_size(self.camera().zoom) / 2.0;
        if self
            .speech_bubble_handles(document)
            .iter()
            .any(|(_, p)| point_distance(point, *p) <= tolerance)
        {
            return true;
        }
        let elements = self.selected_document_elements_snapshot(document);
        let arrows = self.selected_arrows_snapshot(document);
        let padding =
            self.selection_frame_padding_for_selected_members(document, &elements, &arrows);
        let rect = self
            .selected_single_rectangle_snapshot(document)
            .map(|(_, rect)| rect);
        self.selection_bounds_snapshot(document)
            .and_then(|bounds| {
                selection_hit_target(
                    &bounds,
                    rect.as_ref(),
                    None,
                    padding,
                    selection_corner_handle_outset_for_members(
                        self.camera().zoom,
                        elements.len(),
                        arrows.len(),
                    ),
                    self.camera().zoom,
                    point,
                )
            })
            .is_some_and(|target| target != SelectionHitTarget::Move)
    }
    pub fn take_speech_bubble_text_edit_request(&mut self) -> Option<ElementId> {
        self.state.pending_speech_bubble_text_edit.take()
    }
    pub(crate) fn speech_bubble_snapshot(
        &self,
        document: &DocumentModel,
        id: ElementId,
    ) -> Option<SpeechBubbleData> {
        if let Some(drag) = &self.speech_bubble_tail_drag
            && drag.owner_id == id
        {
            return Some(drag.preview.clone());
        }
        if let Some((owner, bubble)) = &self.speech_bubble_draft_geometry
            && *owner == id
        {
            return Some(bubble.clone());
        }
        if let InteractionState::EditingSelection(state) = &self.state.interaction
            && let Some(value) = self.speech_bubble_selection_preview(document, state, id)
        {
            return Some(value);
        }
        let value = document.speech_bubble(id).ok()?;
        if let Some(preview) = self
            .selection_preview_elements()
            .and_then(|items| items.iter().find(|item| item.id == id))
        {
            return Some(speech_bubble_with_selection_rect(value, preview.rect));
        }
        Some(value.clone())
    }
    pub(crate) fn speech_bubble_selection_preview(
        &self,
        document: &DocumentModel,
        state: &EditSelectionState,
        id: ElementId,
    ) -> Option<SpeechBubbleData> {
        let source = document.speech_bubble(id).ok()?;
        let original_rect = state
            .original_elements
            .iter()
            .find(|value| value.id == id)?
            .rect;
        let mut rect = state
            .preview_elements
            .iter()
            .find(|value| value.id == id)?
            .rect;
        let mut transform = state.preview_transform;
        let mut original = source.clone();
        let cached = self.speech_bubble_text_cache.borrow();
        if let Some(entry) = cached.entries.get(&id).filter(|entry| {
            entry.bubble.body.center == original_rect.center
                && entry.bubble.body.width == original_rect.width
                && entry.bubble.body.height == original_rect.height
        }) {
            original = entry.bubble.clone();
        }
        drop(cached);
        if let SelectionTransform::Resize {
            scale_x, scale_y, ..
        } = transform
        {
            rect.corner_radii = flipped_corner_radii(
                rect.highlight_shape,
                original.body.corner_radii,
                scale_x < 0.0,
                scale_y < 0.0,
            );
        } else if matches!(state.mode, SelectionEditMode::CornerRadius { .. }) {
            let radius = |next, previous, raw| if next == previous { raw } else { next };
            rect.corner_radii = snow_draw_engine_core::CornerRadii {
                top_left: radius(
                    rect.corner_radii.top_left,
                    original_rect.corner_radii.top_left,
                    original.body.corner_radii.top_left,
                ),
                top_right: radius(
                    rect.corner_radii.top_right,
                    original_rect.corner_radii.top_right,
                    original.body.corner_radii.top_right,
                ),
                bottom_right: radius(
                    rect.corner_radii.bottom_right,
                    original_rect.corner_radii.bottom_right,
                    original.body.corner_radii.bottom_right,
                ),
                bottom_left: radius(
                    rect.corner_radii.bottom_left,
                    original_rect.corner_radii.bottom_left,
                    original.body.corner_radii.bottom_left,
                ),
            };
        } else {
            rect.corner_radii = original.body.corner_radii;
        }
        if let SelectionTransform::Resize {
            anchor_local,
            scale_x,
            scale_y,
        } = transform
            && let Some(text_id) = source.text_element_id
            && let Some(measured) = self.speech_bubble_layouts.get(&text_id)
            && let Ok(text) = document.text(text_id)
            && let Ok(constraints) = speech_bubble_text_constraints(
                rect,
                measured.minimum_grapheme_width,
                measured.text_ink_reserve,
                text.layout.ink_or_wrap().1,
            )
        {
            let sx = scale_x.signum()
                * scale_x
                    .abs()
                    .max(constraints.body_width / original_rect.width);
            let sy = scale_y.signum()
                * scale_y
                    .abs()
                    .max(constraints.body_height / original_rect.height);
            transform = SelectionTransform::Resize {
                anchor_local,
                scale_x: sx,
                scale_y: sy,
            };
            rect.width = original_rect.width * sx.abs();
            rect.height = original_rect.height * sy.abs();
            rect.center = transform.map_point(&state.original_bounds, original_rect.center);
        }
        if state.duplicate {
            // A new copy carries its complete geometry to the insertion position.
            original.body = rect;
            return Some(original);
        }
        Some(speech_bubble_with_selection_rect(&original, rect))
    }
    pub(crate) fn selection_speech_bubble_previews(
        &self,
        document: &DocumentModel,
        state: &EditSelectionState,
    ) -> Vec<(ElementId, SpeechBubbleData)> {
        state
            .preview_elements
            .iter()
            .filter_map(|preview| {
                self.speech_bubble_selection_preview(document, state, preview.id)
                    .map(|bubble| (preview.id, bubble))
            })
            .collect()
    }
    pub fn speech_bubble_text_request_build_count(&self) -> u64 {
        self.speech_bubble_text_cache.borrow().builds
    }
    pub(crate) fn speech_bubble_has_missing_target(&self, document: &DocumentModel) -> bool {
        self.speech_bubble_tail_drag.as_ref().is_some_and(|drag| {
            document.speech_bubble(drag.owner_id).is_err()
                || document
                    .element(drag.owner_id)
                    .is_ok_and(|element| element.meta.locked)
        })
    }
    pub fn sync_speech_bubble_text_cache(
        &mut self,
        document: &DocumentModel,
        delta: &DocumentDelta,
    ) {
        for id in &delta.removed {
            self.speech_bubble_layouts.remove(id);
        }
        let mut cache = self.speech_bubble_text_cache.borrow_mut();
        if cache.revision.is_none() {
            return;
        }
        for id in delta.touched.iter().chain(&delta.created) {
            if document.speech_bubble(*id).is_ok() {
                cache.dirty.insert(*id);
            }
            if let Some(owner) = document.speech_bubble_id_for_text(*id) {
                cache.dirty.insert(owner);
            }
        }
        for id in &delta.removed {
            cache.entries.remove(id);
            cache.dirty.remove(id);
            cache.pending.remove(id);
            cache.needed.remove(id);
        }
        cache.revision = Some(document.document_revision());
    }
    fn prepare_speech_bubble_text_cache(&self, document: &DocumentModel) {
        let mut cache = self.speech_bubble_text_cache.borrow_mut();
        let generation = self.state.arrow_text_measurement_generation;
        if cache.revision != Some(document.document_revision()) || cache.generation != generation {
            let builds = cache.builds;
            *cache = SpeechBubbleTextCache {
                revision: Some(document.document_revision()),
                generation,
                builds,
                ..Default::default()
            };
            cache.dirty = document
                .speech_bubble_text_bindings()
                .iter()
                .map(|(id, _)| *id)
                .collect();
        }
        let dirty = std::mem::take(&mut cache.dirty);
        for owner in dirty {
            let Some(text_id) = document.bound_text_id_for_speech_bubble(owner) else {
                cache.entries.remove(&owner);
                cache.pending.remove(&owner);
                cache.needed.remove(&owner);
                continue;
            };
            let (Ok(bubble), Ok(text)) = (document.speech_bubble(owner), document.text(text_id))
            else {
                continue;
            };
            let key = key(&bubble.body, text, generation);
            cache.builds += 1;
            let request = SpeechBubbleTextLayoutRequest {
                owner_id: owner,
                text_id,
                body: bubble.body,
                text: text.clone(),
                key,
            };
            let measured = self
                .speech_bubble_layouts
                .get(&text_id)
                .filter(|m| m.key == key);
            let (preview_bubble, preview_text) = measured
                .map_or((bubble.clone(), text.clone()), |m| {
                    self.measured_speech_bubble(bubble.clone(), text.clone(), *m)
                });
            let needed = preview_bubble != *bubble || preview_text != *text;
            if measured.is_none() {
                cache.pending.insert(owner);
            } else {
                cache.pending.remove(&owner);
            }
            if needed {
                cache.needed.insert(owner);
            } else {
                cache.needed.remove(&owner);
            }
            cache.entries.insert(
                owner,
                CachedSpeechBubbleText {
                    request,
                    bubble: preview_bubble,
                    text: preview_text,
                    needed,
                },
            );
        }
    }
    fn speech_bubble_selection_previews_for_originals(&self) -> Option<&[SelectionRectState]> {
        // Copy motion belongs to newly inserted IDs. Original owners keep their
        // passive measurements for both layout requests and scene overrides.
        if matches!(&self.state.interaction, InteractionState::EditingSelection(state) if state.duplicate)
        {
            None
        } else {
            self.selection_preview_elements()
        }
    }
    pub fn speech_bubble_text_layout_requests(
        &self,
        document: &DocumentModel,
    ) -> Vec<SpeechBubbleTextLayoutRequest> {
        self.prepare_speech_bubble_text_cache(document);
        let overrides: HashMap<_, _> = self
            .speech_bubble_selection_previews_for_originals()
            .into_iter()
            .flatten()
            .filter_map(|preview| {
                self.speech_bubble_snapshot(document, preview.id)
                    .map(|bubble| (preview.id, bubble))
            })
            .collect();
        let cache = self.speech_bubble_text_cache.borrow();
        let mut requests: Vec<_> = cache
            .pending
            .iter()
            .filter(|id| !overrides.contains_key(id))
            .filter_map(|id| cache.entries.get(id).map(|entry| entry.request.clone()))
            .collect();
        for (owner, bubble) in overrides {
            let Some(text_id) = bubble.text_element_id else {
                continue;
            };
            let Ok(text) = document.text(text_id) else {
                continue;
            };
            let key = key(
                &bubble.body,
                text,
                self.state.arrow_text_measurement_generation,
            );
            if !self
                .speech_bubble_layouts
                .get(&text_id)
                .is_some_and(|m| m.key == key)
            {
                requests.push(SpeechBubbleTextLayoutRequest {
                    owner_id: owner,
                    text_id,
                    body: bubble.body,
                    text: text.clone(),
                    key,
                });
            }
        }
        requests.sort_unstable_by_key(|r| document.paint_rank(r.owner_id));
        requests
    }
    pub fn apply_speech_bubble_text_layouts(
        &mut self,
        document: &DocumentModel,
        layouts: &[(ElementId, SpeechBubbleTextMeasurement)],
    ) -> Result<bool, ErrorCode> {
        let mut requests: HashMap<_, _> = self
            .speech_bubble_text_layout_requests(document)
            .into_iter()
            .map(|r| (r.text_id, r))
            .collect();
        let mut accepted = Vec::new();
        for &(id, measured) in layouts {
            let Some(request) = requests.get(&id).filter(|r| r.key == measured.key) else {
                continue;
            };
            snow_draw_engine_document::validate_text_layout_size(measured.size)?;
            let constraints = speech_bubble_text_constraints(
                request.body,
                measured.minimum_grapheme_width,
                measured.text_ink_reserve,
                measured.size.ink_or_wrap().1,
            )?;
            if (measured.size.width() - constraints.content_width).abs() > 0.01 {
                return Err(ErrorCode::InvalidArgument);
            }
            accepted.push((id, measured));
            requests.remove(&id);
        }
        if accepted.is_empty() {
            return Ok(false);
        }
        for (id, m) in accepted {
            self.speech_bubble_layouts.insert(id, m);
            if let Some(owner) = document.speech_bubble_id_for_text(id) {
                self.speech_bubble_text_cache
                    .borrow_mut()
                    .dirty
                    .insert(owner);
            }
        }
        self.bump_scene_state_revision();
        self.bump_overlay_state_revision();
        Ok(true)
    }
    fn measured_speech_bubble(
        &self,
        mut bubble: SpeechBubbleData,
        mut text: TextData,
        measurement: SpeechBubbleTextMeasurement,
    ) -> (SpeechBubbleData, TextData) {
        let constraints = speech_bubble_text_constraints(
            bubble.body,
            measurement.minimum_grapheme_width,
            measurement.text_ink_reserve,
            measurement.size.ink_or_wrap().1,
        )
        .expect("validated bubble measurement");
        bubble = speech_bubble_grow_size(&bubble, constraints.body_width, constraints.body_height);
        text.layout = measurement
            .size
            .with_wrap(constraints.content_width, constraints.content_height);
        text.text_ink_reserve = measurement.text_ink_reserve;
        text.center = bubble.body.center;
        text.rotation = bubble.body.rotation;
        text.auto_resize = true;
        (bubble, text)
    }
    pub(crate) fn speech_bubble_display_snapshot(
        &self,
        document: &DocumentModel,
        id: ElementId,
    ) -> Option<SpeechBubbleData> {
        self.speech_bubble_display_values(document, id)
            .map(|(bubble, _)| bubble)
    }
    pub(crate) fn speech_bubble_text_display_snapshot(
        &self,
        document: &DocumentModel,
        text_id: ElementId,
    ) -> Option<TextData> {
        let owner = document.speech_bubble_id_for_text(text_id)?;
        self.speech_bubble_display_values(document, owner)?
            .1
            .map(Cow::into_owned)
    }
    pub(crate) fn speech_bubble_display_rect(
        &self,
        document: &DocumentModel,
        id: ElementId,
    ) -> Option<RectangleData> {
        if let Some(bubble) = self.speech_bubble_display_snapshot(document, id) {
            return Some(snow_draw_engine_document::speech_bubble_body_proxy(&bubble));
        }
        let text = self.speech_bubble_text_display_snapshot(document, id)?;
        let mut rect = document.element_rect_proxy(id)?;
        rect.center = text.center;
        rect.width = text.width();
        rect.height = text.height();
        rect.rotation = text.rotation;
        Some(rect)
    }
    fn speech_bubble_display_values<'a>(
        &self,
        document: &'a DocumentModel,
        id: ElementId,
    ) -> Option<(SpeechBubbleData, Option<Cow<'a, TextData>>)> {
        let bubble = self.speech_bubble_snapshot(document, id)?;
        let Some(text_id) = bubble.text_element_id else {
            return Some((bubble, None));
        };
        let Ok(text) = document.text(text_id) else {
            return Some((bubble, None));
        };
        if let Some(measured) = self.speech_bubble_layouts.get(&text_id).filter(|m| {
            m.key
                == key(
                    &bubble.body,
                    text,
                    self.state.arrow_text_measurement_generation,
                )
        }) {
            let (bubble, text) = self.measured_speech_bubble(bubble, text.clone(), *measured);
            return Some((bubble, Some(Cow::Owned(text))));
        }
        if text.center == bubble.body.center && text.rotation == bubble.body.rotation {
            return Some((bubble, Some(Cow::Borrowed(text))));
        }
        let mut text = text.clone();
        text.center = bubble.body.center;
        text.rotation = bubble.body.rotation;
        Some((bubble, Some(Cow::Owned(text))))
    }
    pub(crate) fn speech_bubble_previews(
        &self,
        document: &DocumentModel,
    ) -> Vec<(ElementId, SpeechBubbleData)> {
        self.prepare_speech_bubble_text_cache(document);
        let cache = self.speech_bubble_text_cache.borrow();
        let mut values: Vec<_> = cache
            .needed
            .iter()
            .filter_map(|id| {
                cache
                    .entries
                    .get(id)
                    .filter(|entry| entry.needed)
                    .map(|entry| (*id, entry.bubble.clone()))
            })
            .collect();
        drop(cache);
        for preview in self
            .speech_bubble_selection_previews_for_originals()
            .into_iter()
            .flatten()
        {
            if let Some(bubble) = self.speech_bubble_display_snapshot(document, preview.id) {
                values.retain(|(id, _)| *id != preview.id);
                values.push((preview.id, bubble));
            }
        }
        if let Some(drag) = &self.speech_bubble_tail_drag {
            values.retain(|(id, _)| *id != drag.owner_id);
            values.push((drag.owner_id, drag.preview.clone()));
        }
        if let Some((owner, bubble)) = &self.speech_bubble_draft_geometry {
            values.retain(|(id, _)| id != owner);
            values.push((*owner, bubble.clone()));
        }
        values
    }
    pub(crate) fn speech_bubble_text_previews(
        &self,
        document: &DocumentModel,
    ) -> Vec<(ElementId, TextData)> {
        self.prepare_speech_bubble_text_cache(document);
        let cache = self.speech_bubble_text_cache.borrow();
        let mut values: Vec<_> = cache
            .needed
            .iter()
            .filter_map(|id| {
                cache
                    .entries
                    .get(id)
                    .filter(|entry| entry.needed)
                    .map(|entry| (entry.request.text_id, entry.text.clone()))
            })
            .collect();
        drop(cache);
        for preview in self
            .speech_bubble_selection_previews_for_originals()
            .into_iter()
            .flatten()
        {
            let Some(bubble) = self.speech_bubble_snapshot(document, preview.id) else {
                continue;
            };
            let Some(text_id) = bubble.text_element_id else {
                continue;
            };
            let Some(text) = self.speech_bubble_text_display_snapshot(document, text_id) else {
                continue;
            };
            values.retain(|(id, _)| *id != text_id);
            values.push((text_id, text));
        }
        values
    }
    pub fn append_speech_bubble_text_layouts(
        &self,
        document: &DocumentModel,
        transaction: &mut Transaction,
    ) {
        let mut owners = HashMap::new();
        let mut texts = HashMap::new();
        for op in transaction.operations() {
            match op {
                Operation::InsertElement { id, data, .. }
                | Operation::UpdateElementData { id, data } => match data {
                    ElementData::SpeechBubble(value) => {
                        owners.insert(*id, value.clone());
                    }
                    ElementData::Text(value) => {
                        texts.insert(*id, value.clone());
                        if let Some(owner) = document.speech_bubble_id_for_text(*id)
                            && let Ok(b) = document.speech_bubble(owner)
                        {
                            owners.entry(owner).or_insert_with(|| b.clone());
                        }
                    }
                    _ => {}
                },
                _ => {}
            }
        }
        for (id, bubble) in owners {
            let Some(text_id) = bubble.text_element_id else {
                continue;
            };
            let Some(text) = texts.get(&text_id).or_else(|| document.text(text_id).ok()) else {
                continue;
            };
            let Some(m) = self.speech_bubble_layouts.get(&text_id).filter(|m| {
                m.key
                    == key(
                        &bubble.body,
                        text,
                        self.state.arrow_text_measurement_generation,
                    )
            }) else {
                continue;
            };
            let (bubble, updated) = self.measured_speech_bubble(bubble, text.clone(), *m);
            transaction.update_speech_bubble(id, bubble);
            transaction.update_text(text_id, updated);
        }
    }
    pub fn fit_speech_bubble_draft(
        &mut self,
        document: &DocumentModel,
        owner: ElementId,
        text: &mut TextData,
        minimum_width: f64,
        ink_reserve: f64,
    ) -> Result<(), ErrorCode> {
        let original = document.speech_bubble(owner)?;
        let base = self
            .speech_bubble_draft_geometry
            .as_ref()
            .filter(|(id, _)| *id == owner)
            .map(|(_, v)| v)
            .unwrap_or(original);
        let constraints = speech_bubble_text_constraints(
            base.body,
            minimum_width,
            ink_reserve,
            text.layout.ink_or_wrap().1,
        )?;
        let bubble = speech_bubble_grow_size(base, constraints.body_width, constraints.body_height);
        text.layout = text
            .layout
            .with_wrap(constraints.content_width, constraints.content_height);
        text.center = bubble.body.center;
        text.rotation = bubble.body.rotation;
        text.text_ink_reserve = ink_reserve;
        self.speech_bubble_draft_geometry = Some((owner, bubble));
        Ok(())
    }
    fn selected_speech_bubble_display_snapshot(
        &self,
        document: &DocumentModel,
    ) -> Option<(ElementId, SpeechBubbleData)> {
        let &id = self
            .state
            .selection
            .ids
            .first()
            .filter(|_| self.state.selection.ids.len() == 1)?;
        Some((id, self.speech_bubble_display_snapshot(document, id)?))
    }
    fn resolve_speech_bubble_controls(
        &self,
        bubble: &SpeechBubbleData,
    ) -> Vec<SpeechBubbleControl> {
        let zoom = self.camera().zoom.max(0.0001);
        let padding = selection_frame_padding_for_members(zoom, 1, 0);
        let add = selection_bottom_control_center(&bubble.body, zoom);
        let mut reserved = vec![
            add,
            speech_bubble_local_to_canvas(
                bubble,
                Point::new(
                    0.0,
                    -bubble.body.height / 2.0 - padding - selection_rotation_offset(zoom),
                ),
            ),
        ];
        for (x, y) in [
            (-1.0, -1.0),
            (0.0, -1.0),
            (1.0, -1.0),
            (1.0, 0.0),
            (1.0, 1.0),
            (0.0, 1.0),
            (-1.0, 1.0),
            (-1.0, 0.0),
        ] {
            reserved.push(speech_bubble_local_to_canvas(
                bubble,
                Point::new(
                    x * (bubble.body.width / 2.0 + padding),
                    y * (bubble.body.height / 2.0 + padding),
                ),
            ));
        }
        let rect = snow_draw_engine_document::speech_bubble_body_proxy(bubble);
        if rect.supports_corner_radius() {
            for corner in [
                RectCorner::TopLeft,
                RectCorner::TopRight,
                RectCorner::BottomRight,
                RectCorner::BottomLeft,
            ] {
                reserved.push(speech_bubble_local_to_canvas(
                    bubble,
                    corner_radius_handle_local_point(&rect, zoom, corner),
                ));
            }
        }
        let mut result = vec![SpeechBubbleControl {
            target: SpeechBubbleControlTarget::AddTail,
            center: add,
            anchor: add,
        }];
        let step = 14.0 / zoom;
        for tail in &bubble.tails {
            let bases = speech_bubble_tail_base_points(bubble, tail);
            for (target, local) in [
                (SpeechBubbleControlTarget::TailBase(tail.id), bases[0]),
                (SpeechBubbleControlTarget::TailBase(tail.id), bases[1]),
                (SpeechBubbleControlTarget::TailTip(tail.id), tail.tip),
            ] {
                let anchor = speech_bubble_local_to_canvas(bubble, local);
                let length = local.x.hypot(local.y).max(f64::EPSILON);
                let direction = Point::new(local.x / length, local.y / length);
                let (sin, cos) = bubble.body.rotation.sin_cos();
                let direction = Point::new(
                    direction.x * cos - direction.y * sin,
                    direction.x * sin + direction.y * cos,
                );
                let mut center = anchor;
                for offset in 0..=reserved.len() * 3 + 1 {
                    center = Point::new(
                        anchor.x + direction.x * step * offset as f64,
                        anchor.y + direction.y * step * offset as f64,
                    );
                    if reserved
                        .iter()
                        .all(|existing| point_distance(center, *existing) >= step - 1e-9)
                    {
                        break;
                    }
                }
                reserved.push(center);
                result.push(SpeechBubbleControl {
                    target,
                    center,
                    anchor,
                });
            }
        }
        result
    }
    pub(crate) fn speech_bubble_handles(&self, document: &DocumentModel) -> Vec<(u8, Point<f64>)> {
        let Some((_, bubble)) = self.selected_speech_bubble_display_snapshot(document) else {
            return Vec::new();
        };
        self.resolve_speech_bubble_controls(&bubble)
            .into_iter()
            .map(|control| (control.target.presentation_kind(), control.center))
            .collect()
    }
    pub(crate) fn speech_bubble_handle_leaders(
        &self,
        document: &DocumentModel,
    ) -> Vec<(Point<f64>, Point<f64>)> {
        let Some((_, bubble)) = self.selected_speech_bubble_display_snapshot(document) else {
            return Vec::new();
        };
        self.resolve_speech_bubble_controls(&bubble)
            .into_iter()
            .filter_map(|control| {
                (point_distance(control.center, control.anchor) > 1e-9)
                    .then_some((control.anchor, control.center))
            })
            .collect()
    }
    pub(crate) fn process_speech_bubble_pointer(
        &mut self,
        document: &DocumentModel,
        event: PointerEvent,
    ) -> Result<Option<InteractionOutput>, ErrorCode> {
        if let Some(mut drag) = self.speech_bubble_tail_drag.take() {
            if event.event_type == PointerEventType::Up
                && event
                    .button
                    .is_some_and(|button| button != PointerButton::Primary)
            {
                self.speech_bubble_tail_drag = Some(drag);
                return Ok(Some(Default::default()));
            }
            if drag.pointer_id != event.pointer_id {
                self.speech_bubble_tail_drag = Some(drag);
                return Ok(Some(Default::default()));
            }
            match event.event_type {
                PointerEventType::Move | PointerEventType::Enter | PointerEventType::Up => {
                    let point = view_to_canvas(event.position, &self.camera(), self.surface_size());
                    let local = speech_bubble_canvas_to_local(
                        &drag.preview,
                        Point::new(point.x + drag.offset.x, point.y + drag.offset.y),
                    );
                    if let Some(index) = drag
                        .preview
                        .tails
                        .iter()
                        .position(|t| t.id == drag.initial_tail.id)
                    {
                        if point_distance(
                            Point::new(point.x + drag.offset.x, point.y + drag.offset.y),
                            drag.start_anchor,
                        ) <= 1e-9
                        {
                            // Returning to the captured anchor restores the exact
                            // initial values, including rotated tip coordinates
                            // and capped attachment rays.
                            drag.preview.tails[index] = drag.initial_tail;
                        } else if drag.base {
                            drag.preview.tails[index].base_width =
                                speech_bubble_base_width_from_point(
                                    &drag.preview,
                                    &drag.preview.tails[index],
                                    local,
                                );
                        } else if speech_bubble_body_contains(&drag.preview, local) {
                            drag.preview.tails.remove(index);
                        } else {
                            drag.preview.tails[index].tip = local;
                        }
                    }
                    self.bump_scene_state_revision();
                    self.bump_overlay_state_revision();
                    if event.event_type == PointerEventType::Up {
                        if drag.preview != drag.original {
                            let mut tx = Transaction::new("edit speech bubble tail");
                            tx.update_speech_bubble(drag.owner_id, drag.preview);
                            self.queue_command(EditorCommand::ApplyTransaction(
                                ApplyTransactionCommand::with_history_undo_snapshot(
                                    tx,
                                    self.capture_document_sync_snapshot(document),
                                ),
                            ));
                        }
                        return Ok(Some(InteractionOutput {
                            consumed: true,
                            capture: self.release_capture_command(),
                            cursor: CursorCommand::Set(CursorStyle::Move),
                        }));
                    }
                    self.speech_bubble_tail_drag = Some(drag);
                    return Ok(Some(InteractionOutput {
                        consumed: true,
                        capture: PointerCaptureCommand::NoChange,
                        cursor: CursorCommand::Set(CursorStyle::Move),
                    }));
                }
                PointerEventType::Cancel => {
                    self.bump_scene_state_revision();
                    self.bump_overlay_state_revision();
                    return Ok(Some(InteractionOutput {
                        consumed: true,
                        capture: self.release_capture_command(),
                        cursor: CursorCommand::Set(CursorStyle::Default),
                    }));
                }
                _ => {
                    self.speech_bubble_tail_drag = Some(drag);
                    return Ok(Some(Default::default()));
                }
            }
        }
        let down = event.event_type == PointerEventType::Down
            && event.button == Some(PointerButton::Primary);
        let hover = matches!(
            event.event_type,
            PointerEventType::Move | PointerEventType::Enter
        );
        if !matches!(self.state.interaction, InteractionState::Idle) || (!down && !hover) {
            return Ok(None);
        }
        let Some((id, bubble)) = self.selected_speech_bubble_display_snapshot(document) else {
            return Ok(None);
        };
        if document.element(id)?.meta.locked {
            return Ok(None);
        }
        let point = view_to_canvas(event.position, &self.camera(), self.surface_size());
        let controls = self.resolve_speech_bubble_controls(&bubble);
        let tolerance = selection_handle_hit_size(self.camera().zoom) / 2.0;
        let Some(control) = controls
            .iter()
            .find(|control| point_distance(point, control.center) <= tolerance)
        else {
            return Ok(None);
        };
        if hover {
            return Ok(Some(InteractionOutput {
                consumed: false,
                capture: PointerCaptureCommand::NoChange,
                cursor: CursorCommand::Set(CursorStyle::Move),
            }));
        }
        let mut preview = bubble.clone();
        let (tail_id, base) = match control.target {
            SpeechBubbleControlTarget::AddTail => {
                let local = speech_bubble_canvas_to_local(&preview, control.anchor);
                let tail = speech_bubble_default_tail(&preview, local, self.camera().zoom);
                let id = tail.id;
                preview.tails.push(tail);
                (id, false)
            }
            SpeechBubbleControlTarget::TailBase(id) => (id, true),
            SpeechBubbleControlTarget::TailTip(id) => (id, false),
        };
        let initial_tail = preview
            .tails
            .iter()
            .find(|tail| tail.id == tail_id)
            .copied()
            .ok_or(ErrorCode::InvalidState)?;
        self.speech_bubble_tail_drag = Some(SpeechBubbleTailDrag {
            pointer_id: event.pointer_id,
            owner_id: id,
            // Compare the gesture with its displayed start, so passive layout
            // alone never commits a click or cancelled add-tail gesture.
            original: bubble,
            preview,
            initial_tail,
            base,
            offset: Point::new(control.anchor.x - point.x, control.anchor.y - point.y),
            start_anchor: control.anchor,
        });
        self.bump_scene_state_revision();
        self.bump_overlay_state_revision();
        Ok(Some(InteractionOutput {
            consumed: true,
            capture: self.capture_command_for_start(event.pointer_id),
            cursor: CursorCommand::Set(CursorStyle::Move),
        }))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use snow_draw_engine_core::{ColorRgba8, CornerRadii};
    use snow_draw_engine_document::{FillStyle, RectangleElementKind};
    fn body(shape: HighlightShape) -> RectangleData {
        RectangleData {
            text_element_id: None,
            stroke_rendering_style: Default::default(),
            stroke_rendering_seed: 0,
            rectangle_kind: RectangleElementKind::Rectangle,
            highlight_shape: shape,
            center: Point::default(),
            width: 120.0,
            height: 30.0,
            rotation: 0.0,
            fill: ColorRgba8::default(),
            fill_style: FillStyle::Solid,
            stroke: ColorRgba8::default(),
            stroke_width: 2.0,
            stroke_style: Default::default(),
            corner_radii: CornerRadii {
                top_left: 100.0,
                top_right: 10.0,
                bottom_right: 50.0,
                bottom_left: 1.0,
            },
            opacity: 1.0,
        }
    }
    #[test]
    fn speech_bubble_safe_content_rect_fits_every_contour_and_has_no_height_width_feedback() {
        for shape in [
            HighlightShape::Rectangle,
            HighlightShape::Ellipse,
            HighlightShape::Diamond,
        ] {
            for width in [1.0, 20.0, 100.0, 900.0] {
                for height in [1.0, 30.0, 1000.0] {
                    for minimum in [1.0, 30.0, 110.0] {
                        let b = RectangleData {
                            width,
                            height,
                            ..body(shape)
                        };
                        let c = speech_bubble_text_constraints(b, minimum, 3.0, 75.0).unwrap();
                        assert!(
                            c.content_width + 1e-8 >= minimum && c.content_height + 1e-8 >= 75.0
                        );
                        let grown = SpeechBubbleData::new(RectangleData {
                            width: c.body_width,
                            height: c.body_height,
                            ..b
                        });
                        for x in [-0.5, 0.5] {
                            for y in [-0.5, 0.5] {
                                assert!(speech_bubble_body_contains(
                                    &grown,
                                    Point::new(c.content_width * x, c.content_height * y)
                                ));
                            }
                        }
                        let taller =
                            speech_bubble_text_constraints(b, minimum, 3.0, 600.0).unwrap();
                        assert_eq!(taller.content_width, c.content_width);
                        assert_eq!(taller.body_width, c.body_width);
                    }
                }
            }
        }
    }
    #[test]
    fn speech_bubble_constraints_reject_nonfinite_or_negative_measurements() {
        for invalid in [f64::NAN, f64::INFINITY, -1.0] {
            assert_eq!(
                speech_bubble_text_constraints(body(HighlightShape::Rectangle), invalid, 1.0, 30.0),
                Err(ErrorCode::InvalidArgument)
            );
            assert_eq!(
                speech_bubble_text_constraints(body(HighlightShape::Ellipse), 10.0, invalid, 30.0),
                Err(ErrorCode::InvalidArgument)
            );
        }
    }
}
