use super::{Engine, MutationResult, ViewportId};
use snow_draw_engine_core::{ErrorCode, Point};
use snow_draw_engine_document::{
    ElementId, TextData, TextHorizontalAlign, TextLayoutSize, TextVerticalAlign,
    resolve_text_layout_rect,
};
use snow_draw_engine_editor::{
    ActiveTextDraftPresentation, SerialNumberStyle, TextDraftCommit, TextLayoutOverride,
    TextResizeMeasurementRequest, TextStyle,
};

#[derive(Clone, Debug, PartialEq)]
pub struct TextElementInfo {
    pub id: ElementId,
    pub arrow_id: Option<ElementId>,
    pub arrow_width: f64,
    pub bubble_id: Option<ElementId>,
    pub bubble_body: Option<snow_draw_engine_document::RectangleData>,
    pub text_ink_reserve: f64,
    pub center: Point<f64>,
    pub width: f64,
    pub height: f64,
    pub content_width: f64,
    pub content_height: f64,
    pub rotation: f64,
    pub text: String,
    pub font_size: f64,
    pub bold: bool,
    pub italic: bool,
    pub font_family: Option<String>,
    pub horizontal_align: TextHorizontalAlign,
    pub vertical_align: TextVerticalAlign,
    pub auto_resize: bool,
    pub measure_natural_width: bool,
}

fn text_element_info_from_resize_request(request: TextResizeMeasurementRequest) -> TextElementInfo {
    TextElementInfo {
        id: request.id,
        arrow_id: None,
        arrow_width: 0.0,
        bubble_id: None,
        bubble_body: None,
        text_ink_reserve: 0.0,
        center: request.center,
        width: request.width,
        height: request.height,
        content_width: 0.0,
        content_height: 0.0,
        rotation: request.rotation,
        text: request.text,
        font_size: request.font_size,
        bold: request.bold,
        italic: request.italic,
        font_family: request.font_family,
        horizontal_align: request.horizontal_align,
        vertical_align: request.vertical_align,
        auto_resize: request.auto_resize,
        measure_natural_width: request.measure_natural_width,
    }
}

fn text_element_info_from_active_draft(draft: &ActiveTextDraftPresentation) -> TextElementInfo {
    TextElementInfo {
        id: draft.existing_id().unwrap_or_default(),
        arrow_id: None,
        arrow_width: 0.0,
        bubble_id: None,
        bubble_body: None,
        text_ink_reserve: 0.0,
        center: draft.text.center,
        width: draft.text.width(),
        height: draft.text.height(),
        content_width: draft.text.layout.ink_or_wrap().0,
        content_height: draft.text.layout.ink_or_wrap().1,
        rotation: draft.text.rotation,
        text: draft.text.text.clone(),
        font_size: draft.text.font_size,
        bold: draft.text.bold,
        italic: draft.text.italic,
        font_family: draft.text.font_family.clone(),
        horizontal_align: draft.text.horizontal_align,
        vertical_align: draft.text.vertical_align,
        auto_resize: draft.text.auto_resize,
        measure_natural_width: draft.text.auto_resize,
    }
}

fn text_style_from_text(text: &TextData) -> TextStyle {
    TextStyle {
        color: text.color,
        font_size: text.font_size,
        bold: text.bold,
        italic: text.italic,
        font_family: text.font_family.clone(),
        fill: text.fill,
        fill_style: text.fill_style,
        stroke: text.stroke,
        stroke_width: text.stroke_width,
        corner_radii: text.corner_radii,
        horizontal_align: text.horizontal_align,
        vertical_align: text.vertical_align,
        opacity: text.opacity,
    }
}

impl Engine {
    pub fn arrow_text_count(&self) -> usize {
        self.model.arrow_label_bindings().len()
    }

    pub fn arrow_text_layout_requests(
        &self,
        viewport: ViewportId,
    ) -> Result<Vec<snow_draw_engine_editor::ArrowTextLayoutRequest>, ErrorCode> {
        self.ensure_viewport(viewport)?;
        Ok(self.editor.arrow_text_layout_requests(&self.model))
    }

    pub fn apply_arrow_text_measurements(
        &mut self,
        viewport: ViewportId,
        layouts: &[(ElementId, u64, TextLayoutSize, f64)],
    ) -> Result<MutationResult, ErrorCode> {
        self.ensure_viewport(viewport)?;
        if self
            .editor
            .apply_arrow_text_measurements(&self.model, layouts)?
        {
            self.refresh_all_viewports()
        } else {
            Ok(MutationResult::default())
        }
    }

    /// Invalidate host metrics before obtaining and applying replacement layouts.
    /// No intermediate scene is published with the now-missing measurements.
    pub fn invalidate_arrow_text_measurements(
        &mut self,
        viewport: ViewportId,
    ) -> Result<(), ErrorCode> {
        self.ensure_viewport(viewport)?;
        self.speech_bubble_layout_origin = None;
        self.editor.invalidate_arrow_text_measurements();
        Ok(())
    }

    pub fn arrow_text_target(
        &self,
        viewport: ViewportId,
        point: Option<Point<f64>>,
    ) -> Result<Option<(TextElementInfo, TextStyle)>, ErrorCode> {
        self.ensure_viewport(viewport)?;
        if !self.editor.can_begin_arrow_text() {
            return Ok(None);
        }
        let target = if let Some(point) = point {
            let zoom = self.viewport_slot(viewport)?.view.camera.zoom;
            self.model
                .topmost_element_at_with_tolerance(point, 8.0 / zoom.max(0.01))
                .map(|(id, _)| self.model.arrow_id_for_text(id).unwrap_or(id))
        } else {
            let ids = self.editor.selected_ids();
            (ids.len() == 1).then(|| self.model.arrow_id_for_text(ids[0]).unwrap_or(ids[0]))
        };
        let Some(id) = target else {
            return Ok(None);
        };
        let Ok(arrow) = self.model.arrow(id) else {
            return Ok(None);
        };
        let meta = self.model.element(id)?.meta;
        if !arrow.is_regular_arrow() || meta.locked || !meta.visible {
            return Ok(None);
        }
        if let Some(text_id) = arrow.text_element_id {
            return Ok(Some((
                self.text_element_info(text_id)?,
                text_style_from_text(self.model.text(text_id)?),
            )));
        }
        let mut style = self.editor.text_style(&self.model);
        style.horizontal_align = snow_draw_engine_document::TextHorizontalAlign::Center;
        style.vertical_align = snow_draw_engine_document::TextVerticalAlign::Center;
        Ok(Some((
            TextElementInfo {
                id: ElementId::default(),
                arrow_id: Some(id),
                arrow_width: arrow.width,
                bubble_id: None,
                bubble_body: None,
                text_ink_reserve: 0.0,
                center: snow_draw_engine_document::arrow_text_anchor(arrow),
                width: 1.0,
                height: style.font_size,
                content_width: 0.0,
                content_height: 0.0,
                rotation: 0.0,
                text: String::new(),
                font_size: style.font_size,
                bold: style.bold,
                italic: style.italic,
                font_family: style.font_family.clone(),
                horizontal_align: style.horizontal_align,
                vertical_align: style.vertical_align,
                auto_resize: true,
                measure_natural_width: false,
            },
            style,
        )))
    }

    pub fn set_viewport_text_style(
        &mut self,
        id: ViewportId,
        style: TextStyle,
        layouts: &[TextLayoutOverride],
    ) -> Result<MutationResult, ErrorCode> {
        self.set_viewport_text_style_patch(
            id,
            style,
            snow_draw_engine_editor::TEXT_STYLE_ALL_PROPERTIES,
            layouts,
        )
    }

    pub fn set_viewport_text_style_patch(
        &mut self,
        id: ViewportId,
        style: TextStyle,
        properties: u32,
        layouts: &[TextLayoutOverride],
    ) -> Result<MutationResult, ErrorCode> {
        self.ensure_viewport(id)?;
        let before = self.editor.snapshot();
        let command = self
            .editor
            .set_text_style(&self.model, style, properties, layouts)?;
        if let Some(command) = command {
            self.apply_editor_command(id, command)
        } else {
            self.refresh_after_session_mutation(before)
        }
    }

    pub fn set_viewport_text_creation_style(
        &mut self,
        id: ViewportId,
        style: TextStyle,
        properties: u32,
    ) -> Result<MutationResult, ErrorCode> {
        self.ensure_viewport(id)?;
        let before = self.editor.snapshot();
        self.editor.set_text_creation_style(style, properties)?;
        self.refresh_after_session_mutation(before)
    }

    pub fn set_viewport_serial_number_style(
        &mut self,
        id: ViewportId,
        style: SerialNumberStyle,
    ) -> Result<MutationResult, ErrorCode> {
        self.ensure_viewport(id)?;
        let before = self.editor.snapshot();
        let command = self.editor.set_serial_number_style(&self.model, style)?;
        if let Some(command) = command {
            self.apply_editor_command(id, command)
        } else {
            self.refresh_after_session_mutation(before)
        }
    }

    pub fn set_viewport_serial_number_style_patch(
        &mut self,
        id: ViewportId,
        style: SerialNumberStyle,
        properties: u32,
    ) -> Result<MutationResult, ErrorCode> {
        self.ensure_viewport(id)?;
        let before = self.editor.snapshot();
        let command = self
            .editor
            .set_serial_number_style_patch(&self.model, style, properties)?;
        if let Some(command) = command {
            self.apply_editor_command(id, command)
        } else {
            self.refresh_after_session_mutation(before)
        }
    }

    pub fn create_text_with_viewport_changes(
        &mut self,
        source_viewport_id: ViewportId,
        center: snow_draw_engine_core::Point<f64>,
        text_content: impl Into<String>,
        layout: TextLayoutSize,
    ) -> Result<MutationResult, ErrorCode> {
        self.ensure_viewport(source_viewport_id)?;
        let before = self.editor.snapshot();
        let command =
            self.editor
                .queue_create_text_element(&self.model, center, text_content, layout)?;
        if let Some(command) = command {
            self.apply_editor_command(source_viewport_id, command)
        } else {
            self.refresh_after_session_mutation(before)
        }
    }

    pub fn hit_text_at(
        &self,
        source_viewport_id: ViewportId,
        point: Point<f64>,
    ) -> Result<Option<ElementId>, ErrorCode> {
        self.ensure_viewport(source_viewport_id)?;
        Ok(self.editor.hit_text_at(&self.model, point))
    }

    pub fn text_element_info(&self, id: ElementId) -> Result<TextElementInfo, ErrorCode> {
        let displayed_text = self
            .editor
            .speech_bubble_text_display_snapshot(&self.model, id);
        let text = displayed_text.as_ref().unwrap_or(self.model.text(id)?);
        let layout = resolve_text_layout_rect(text);
        Ok(TextElementInfo {
            id,
            bubble_id: self.model.speech_bubble_id_for_text(id),
            bubble_body: self.model.speech_bubble_id_for_text(id).and_then(|owner| {
                self.editor
                    .speech_bubble_display_snapshot(&self.model, owner)
                    .map(|bubble| bubble.body)
            }),
            text_ink_reserve: text.text_ink_reserve,
            arrow_id: self.model.arrow_id_for_text(id),
            arrow_width: self
                .model
                .arrow_id_for_text(id)
                .and_then(|id| self.model.arrow(id).ok())
                .map_or(0.0, |arrow| arrow.width),
            center: layout.center,
            width: layout.width,
            height: layout.height,
            content_width: text.layout.ink_or_wrap().0,
            content_height: text.layout.ink_or_wrap().1,
            rotation: layout.rotation,
            text: text.text.clone(),
            font_size: text.font_size,
            bold: text.bold,
            italic: text.italic,
            font_family: text.font_family.clone(),
            horizontal_align: text.horizontal_align,
            vertical_align: text.vertical_align,
            auto_resize: text.auto_resize,
            measure_natural_width: false,
        })
    }

    pub fn commit_text_draft_with_viewport_changes(
        &mut self,
        source_viewport_id: ViewportId,
        draft: TextDraftCommit,
    ) -> Result<MutationResult, ErrorCode> {
        self.ensure_viewport(source_viewport_id)?;
        let before = self.editor.snapshot();
        let command = self.editor.commit_text_draft(&self.model, draft)?;
        if let Some(command) = command {
            self.apply_editor_command(source_viewport_id, command)
        } else {
            self.refresh_after_session_mutation(before)
        }
    }

    pub fn selected_text_count(&self) -> usize {
        self.editor
            .selected_ids()
            .into_iter()
            .filter(|id| self.model.text(*id).is_ok())
            .count()
    }

    pub fn selected_text_elements(&self) -> Vec<TextElementInfo> {
        self.editor
            .selected_ids()
            .into_iter()
            .filter_map(|id| self.text_element_info(id).ok())
            .collect()
    }

    pub fn active_text_resize_measurement_request(
        &self,
        source_viewport_id: ViewportId,
    ) -> Result<Option<TextElementInfo>, ErrorCode> {
        self.ensure_viewport(source_viewport_id)?;
        Ok(self
            .editor
            .active_text_resize_measurement_request(&self.model)
            .map(text_element_info_from_resize_request))
    }

    pub fn active_text_draft_presentation(
        &self,
        source_viewport_id: ViewportId,
    ) -> Result<Option<(TextElementInfo, TextStyle)>, ErrorCode> {
        self.ensure_viewport(source_viewport_id)?;
        Ok(self
            .editor
            .active_text_draft_display_presentation()
            .map(|draft| {
                let style = text_style_from_text(&draft.text);
                let mut info = text_element_info_from_active_draft(&draft);
                info.arrow_id = match draft.target {
                    snow_draw_engine_editor::ActiveTextDraftTarget::NewArrow(id) => Some(id),
                    _ => draft
                        .existing_id()
                        .and_then(|id| self.model.arrow_id_for_text(id)),
                };
                info.arrow_width = info
                    .arrow_id
                    .and_then(|id| self.model.arrow(id).ok())
                    .map_or(0.0, |arrow| arrow.width);
                info.bubble_id = match draft.target {
                    snow_draw_engine_editor::ActiveTextDraftTarget::NewSpeechBubble(id) => Some(id),
                    _ => draft
                        .existing_id()
                        .and_then(|id| self.model.speech_bubble_id_for_text(id)),
                };
                info.bubble_body = info
                    .bubble_id
                    .and_then(|id| self.editor.speech_bubble_draft_body(&self.model, id));
                info.text_ink_reserve = draft.text.text_ink_reserve;
                (info, style)
            }))
    }

    pub fn set_active_text_draft_presentation_with_viewport_changes(
        &mut self,
        source_viewport_id: ViewportId,
        mut draft: ActiveTextDraftPresentation,
    ) -> Result<MutationResult, ErrorCode> {
        self.ensure_viewport(source_viewport_id)?;
        let revision = self
            .editor
            .active_text_draft_presentation()
            .map_or(1, |current| {
                if current.target == draft.target && current.text == draft.text {
                    current.revision
                } else {
                    current.revision.wrapping_add(1)
                }
            });
        draft.revision = revision;
        let before = self.editor.snapshot();
        let changed = self
            .editor
            .set_active_text_draft_presentation(&self.model, draft)?;
        if changed {
            self.refresh_after_session_mutation(before)
        } else {
            Ok(MutationResult::default())
        }
    }

    pub fn clear_active_text_draft_presentation_with_viewport_changes(
        &mut self,
        source_viewport_id: ViewportId,
    ) -> Result<MutationResult, ErrorCode> {
        self.ensure_viewport(source_viewport_id)?;
        let before = self.editor.snapshot();
        if self.editor.clear_active_text_draft_presentation() {
            self.refresh_after_session_mutation(before)
        } else {
            Ok(MutationResult::default())
        }
    }

    pub fn apply_active_text_resize_measurement_with_viewport_changes(
        &mut self,
        source_viewport_id: ViewportId,
        layout: TextLayoutSize,
    ) -> Result<MutationResult, ErrorCode> {
        self.ensure_viewport(source_viewport_id)?;
        let before = self.editor.snapshot();
        let applied = self
            .editor
            .apply_active_text_resize_measurement(&self.model, layout)?;
        if applied {
            self.refresh_after_session_mutation(before)
        } else {
            Ok(MutationResult::default())
        }
    }

    pub fn is_text_bound_to_serial_number(&self, id: ElementId) -> bool {
        self.model.is_text_bound_to_serial_number(id)
    }

    pub fn take_text_edit_request(
        &mut self,
        viewport: ViewportId,
    ) -> Result<Option<ElementId>, ErrorCode> {
        self.ensure_viewport(viewport)?;
        Ok(self.editor.take_text_edit_request())
    }

    pub fn take_new_text_draft_request(&mut self, viewport: ViewportId) -> Result<bool, ErrorCode> {
        self.ensure_viewport(viewport)?;
        Ok(self.editor.take_new_text_draft_request())
    }

    pub fn serial_number_label_layout_request(
        &self,
        viewport: ViewportId,
    ) -> Result<Option<snow_draw_engine_editor::SerialNumberLabelLayoutRequest>, ErrorCode> {
        self.ensure_viewport(viewport)?;
        Ok(self.editor.serial_number_label_layout_request(&self.model))
    }

    pub fn bound_label_layout_request(
        &self,
        viewport: ViewportId,
    ) -> Result<Option<snow_draw_engine_editor::SerialNumberLabelLayoutRequest>, ErrorCode> {
        self.ensure_viewport(viewport)?;
        Ok(self.editor.bound_label_layout_request(&self.model))
    }

    pub fn apply_bound_label_layout(
        &mut self,
        viewport: ViewportId,
        text_id: ElementId,
        layout: TextLayoutSize,
    ) -> Result<MutationResult, ErrorCode> {
        self.ensure_viewport(viewport)?;
        let before = self.editor.snapshot();
        self.editor
            .apply_bound_label_layout(&self.model, text_id, layout)?;
        self.refresh_after_session_mutation(before)
    }

    pub fn apply_serial_number_label_layout(
        &mut self,
        viewport: ViewportId,
        text_id: ElementId,
        layout: TextLayoutSize,
    ) -> Result<MutationResult, ErrorCode> {
        self.ensure_viewport(viewport)?;
        let before = self.editor.snapshot();
        self.editor
            .apply_serial_number_label_layout(&self.model, text_id, layout)?;
        self.refresh_after_session_mutation(before)
    }

    pub fn create_serial_number_text_with_viewport_changes(
        &mut self,
        source_viewport_id: ViewportId,
        layout: TextLayoutSize,
    ) -> Result<(MutationResult, Option<ElementId>), ErrorCode> {
        self.ensure_viewport(source_viewport_id)?;
        let before = self.editor.snapshot();
        let operation = self
            .editor
            .create_serial_number_text_elements(&self.model, layout)?;
        let result = if let Some(command) = operation.command {
            self.apply_editor_command(source_viewport_id, command)?
        } else {
            self.refresh_after_session_mutation(before)?
        };
        if let Some(text_id) = operation.single_text_id {
            let before_select = self.editor.snapshot();
            if self.editor.select_element(&self.model, text_id).is_ok() {
                let selection_result = self.refresh_after_session_mutation(before_select)?;
                let mut changed_viewports = result.changed_viewports;
                changed_viewports.extend(selection_result.changed_viewports);
                changed_viewports.sort_by_key(|id| id.0);
                changed_viewports.dedup();
                return Ok((MutationResult { changed_viewports }, Some(text_id)));
            }
        }
        Ok((result, operation.single_text_id))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::ViewportConfig;
    use snow_draw_engine_editor::ActiveTextDraftTarget;

    #[test]
    fn text_presentation_preserves_resized_content_bounds() {
        let mut engine = Engine::default();
        let viewport = engine.create_viewport(ViewportConfig::default()).unwrap();
        let id = engine.model.peek_next_element_id();
        engine
            .create_text_with_viewport_changes(
                viewport,
                Point::new(0.0, 0.0),
                "resized text",
                TextLayoutSize::with_content(432.0, 92.0, 428.0, 92.0),
            )
            .unwrap();
        let stored = engine.text_element_info(id).unwrap();
        assert_eq!((stored.content_width, stored.content_height), (428.0, 92.0));
        let mut text = engine.model.text(id).unwrap().clone();
        text.layout = TextLayoutSize::with_content(1354.0, 287.0, 1340.0, 287.0);
        text.font_size = 226.0;
        text.rotation = -0.644;
        engine
            .set_active_text_draft_presentation_with_viewport_changes(
                viewport,
                ActiveTextDraftPresentation {
                    target: ActiveTextDraftTarget::Existing(id),
                    revision: 0,
                    text,
                },
            )
            .unwrap();
        let (draft, _) = engine
            .active_text_draft_presentation(viewport)
            .unwrap()
            .unwrap();
        assert_eq!((draft.width, draft.height), (1354.0, 287.0));
        assert_eq!((draft.content_width, draft.content_height), (1340.0, 287.0));
    }
}

#[cfg(test)]
mod emphasis_tests {
    use super::*;

    #[test]
    fn text_emphasis_serialization_accepts_legacy_styles_and_documents() {
        let text = TextData {
            bold: true,
            italic: true,
            ..TextData::default()
        };
        let style = text_style_from_text(&text);
        let serialized = serde_json::to_value(&style).unwrap();
        assert_eq!(
            serde_json::from_value::<TextStyle>(serialized.clone()).unwrap(),
            style
        );
        let mut legacy = serialized;
        legacy.as_object_mut().unwrap().remove("bold");
        legacy.as_object_mut().unwrap().remove("italic");
        let restored: TextStyle = serde_json::from_value(legacy).unwrap();
        assert!(!restored.bold && !restored.italic);

        let mut serialized = serde_json::to_value(&text).unwrap();
        assert_eq!(
            serde_json::from_value::<TextData>(serialized.clone()).unwrap(),
            text
        );
        serialized.as_object_mut().unwrap().remove("bold");
        serialized.as_object_mut().unwrap().remove("italic");
        let restored: TextData = serde_json::from_value(serialized).unwrap();
        assert!(!restored.bold && !restored.italic);
    }
}

impl Engine {
    pub fn speech_bubble_text_request_build_count(&self) -> u64 {
        self.editor.speech_bubble_text_request_build_count()
    }
    pub fn speech_bubble_text_count(&self) -> usize {
        self.model.speech_bubble_text_bindings().len()
    }
    pub fn speech_bubble_text_layout_requests(
        &self,
        viewport: ViewportId,
    ) -> Result<Vec<snow_draw_engine_editor::SpeechBubbleTextLayoutRequest>, ErrorCode> {
        self.ensure_viewport(viewport)?;
        Ok(self.editor.speech_bubble_text_layout_requests(&self.model))
    }
    pub fn apply_speech_bubble_text_layouts(
        &mut self,
        viewport: ViewportId,
        layouts: &[(
            ElementId,
            snow_draw_engine_editor::SpeechBubbleTextMeasurement,
        )],
    ) -> Result<MutationResult, ErrorCode> {
        self.ensure_viewport(viewport)?;
        let accepted = self
            .editor
            .apply_speech_bubble_text_layouts(&self.model, layouts)?;
        if !accepted {
            return Ok(MutationResult::default());
        }
        let Some(mut origin) = self.speech_bubble_layout_origin.take() else {
            return self.refresh_all_viewports();
        };
        if origin.revision != self.model.document_revision()
            || !self.history.can_amend_latest(origin.history_generation)
            || self.editor.has_pointer_capture()
            || self.editor.active_text_draft_presentation().is_some()
        {
            return self.refresh_all_viewports();
        }
        let submitted_keys: std::collections::HashSet<_> = layouts
            .iter()
            .map(|(id, measurement)| (*id, measurement.key))
            .collect();
        let mut amendment =
            snow_draw_engine_document::Transaction::new("speech bubble text layout");
        let mut completed = Vec::new();
        for (&owner, &(text_id, expected_key)) in &origin.owners {
            if !submitted_keys.contains(&(text_id, expected_key))
                || self
                    .editor
                    .speech_bubble_document_layout_key(&self.model, owner)
                    != Some((text_id, expected_key))
            {
                continue;
            }
            let Some((bubble, text)) = self
                .editor
                .speech_bubble_measured_document_values(&self.model, owner)
            else {
                continue;
            };
            if self.model.speech_bubble(owner)? != &bubble {
                amendment.update_speech_bubble(owner, bubble);
            }
            if self.model.text(text_id)? != &text {
                amendment.update_text(text_id, text);
            }
            completed.push(owner);
        }
        for owner in completed {
            origin.owners.remove(&owner);
        }
        let result = if amendment.is_empty() {
            self.refresh_all_viewports()?
        } else {
            let snapshot = self.capture_session_snapshot();
            let applied = self.model.apply_transaction(amendment.clone())?;
            let result = self.finish_document_change(&snapshot, &applied.changes);
            let redo_snapshot = self.capture_session_snapshot();
            let amended = self.history.amend_latest(
                origin.history_generation,
                &amendment,
                &applied.inverse,
                redo_snapshot,
            );
            debug_assert!(
                amended,
                "exclusive runtime access preserves the checked history entry"
            );
            origin.revision = self.model.document_revision();
            origin.history_generation = self.history.mutation_generation();
            result
        };
        if !origin.owners.is_empty() {
            self.speech_bubble_layout_origin = Some(origin);
        }
        Ok(result)
    }
    pub fn take_speech_bubble_text_edit_request(
        &mut self,
        viewport: ViewportId,
    ) -> Result<Option<ElementId>, ErrorCode> {
        self.ensure_viewport(viewport)?;
        Ok(self.editor.take_speech_bubble_text_edit_request())
    }
    pub fn speech_bubble_text_target(
        &self,
        viewport: ViewportId,
        point: Option<Point<f64>>,
    ) -> Result<Option<(TextElementInfo, TextStyle)>, ErrorCode> {
        self.ensure_viewport(viewport)?;
        if !self.editor.can_begin_arrow_text()
            || point.is_some_and(|point| self.editor.speech_bubble_control_at(&self.model, point))
        {
            return Ok(None);
        }
        let target = if let Some(point) = point {
            let zoom = self.viewport_slot(viewport)?.view.camera.zoom;
            let hit = self
                .model
                .topmost_element_at_with_tolerance(point, 8.0 / zoom.max(0.01))
                .map(|(id, _)| id);
            self.model
                .paint_order()
                .iter()
                .rev()
                .copied()
                .find(|id| {
                    let Ok(element) = self.model.element(*id) else {
                        return false;
                    };
                    if !element.meta.visible {
                        return false;
                    }
                    if let Some(bubble) =
                        self.editor.speech_bubble_display_snapshot(&self.model, *id)
                    {
                        snow_draw_engine_document::speech_bubble_body_contains(
                            &bubble,
                            snow_draw_engine_document::speech_bubble_canvas_to_local(
                                &bubble, point,
                            ),
                        ) || snow_draw_engine_document::speech_bubble_hit_test(
                            &bubble,
                            point,
                            8.0 / zoom.max(0.01),
                        )
                    } else if let Some(text) = self
                        .editor
                        .speech_bubble_text_display_snapshot(&self.model, *id)
                    {
                        snow_draw_engine_document::text_hit_test(&text, point, 8.0 / zoom.max(0.01))
                    } else {
                        hit == Some(*id)
                    }
                })
                .map(|id| self.model.speech_bubble_id_for_text(id).unwrap_or(id))
        } else {
            let ids = self.editor.selected_ids();
            (ids.len() == 1).then(|| {
                self.model
                    .speech_bubble_id_for_text(ids[0])
                    .unwrap_or(ids[0])
            })
        };
        let Some(owner) = target else {
            return Ok(None);
        };
        let Some(bubble) = self
            .editor
            .speech_bubble_display_snapshot(&self.model, owner)
        else {
            return Ok(None);
        };
        let meta = self.model.element(owner)?.meta;
        if meta.locked || !meta.visible {
            return Ok(None);
        }
        if let Some(text) = bubble.text_element_id {
            return Ok(Some((
                self.text_element_info(text)?,
                text_style_from_text(self.model.text(text)?),
            )));
        }
        let mut style = self.editor.text_style(&self.model);
        style.horizontal_align = TextHorizontalAlign::Center;
        style.vertical_align = TextVerticalAlign::Center;
        let text = TextData {
            center: bubble.body.center,
            rotation: bubble.body.rotation,
            font_size: style.font_size,
            font_family: style.font_family.clone(),
            bold: style.bold,
            italic: style.italic,
            horizontal_align: style.horizontal_align,
            vertical_align: style.vertical_align,
            ..Default::default()
        };
        let draft = ActiveTextDraftPresentation {
            target: snow_draw_engine_editor::ActiveTextDraftTarget::NewSpeechBubble(owner),
            revision: 0,
            text,
        };
        let mut info = text_element_info_from_active_draft(&draft);
        info.bubble_id = Some(owner);
        info.bubble_body = Some(bubble.body);
        info.center = bubble.body.center;
        let c = snow_draw_engine_editor::speech_bubble_text_constraints(
            bubble.body,
            1.0,
            0.0,
            style.font_size,
        )?;
        info.width = c.content_width;
        info.height = c.content_height;
        info.auto_resize = true;
        Ok(Some((info, style)))
    }
    pub fn set_speech_bubble_aware_text_draft(
        &mut self,
        viewport: ViewportId,
        mut draft: ActiveTextDraftPresentation,
        minimum: f64,
    ) -> Result<MutationResult, ErrorCode> {
        self.ensure_viewport(viewport)?;
        self.editor
            .set_speech_bubble_draft_minimum(&self.model, &mut draft, minimum)?;
        self.set_active_text_draft_presentation_with_viewport_changes(viewport, draft)
    }
}
