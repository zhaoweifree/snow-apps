use super::*;
use snow_draw_engine_document::{ElementMeta, TextLayoutSize, text_with_measured_layout};

/// A bound label being positioned without changing its document or undo history.
#[derive(Clone, Debug, PartialEq)]
pub struct ShapeBoundTextPreview {
    pub owner_id: ElementId,
    pub text_id: ElementId,
    pub text: TextData,
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct ShapeTextDrag {
    pointer_id: u32,
    owner: RectangleData,
    original_text: Option<TextData>,
    start_view_position: Point<f64>,
    initial_center: Point<f64>,
    moved: bool,
    measured: bool,
    preview: ShapeBoundTextPreview,
}

impl Editor {
    fn selected_shape_text_owner(
        &self,
        document: &DocumentModel,
    ) -> Option<(ElementId, RectangleData)> {
        if !matches!(self.active_tool(), ActiveTool::Select | ActiveTool::Shape)
            || self.state.active_text_draft.is_some()
            || self.speech_bubble_tail_drag.is_some()
            || !matches!(self.state.interaction, InteractionState::Idle)
            || self.state.selection.ids.len() != 1
        {
            return None;
        }
        let id = self.state.selection.ids[0];
        let element = document.element(id).ok()?;
        let shape = *document.rectangle(id).ok()?;
        if element.meta.locked
            || !element.meta.visible
            || shape.rectangle_kind != RectangleElementKind::Rectangle
        {
            return None;
        }
        if let Some(text_id) = document.bound_text_id_for_shape(id)
            && document
                .element(text_id)
                .is_ok_and(|element| element.meta.locked || !element.meta.visible)
        {
            return None;
        }
        Some((id, shape))
    }

    pub(crate) fn shape_text_handle(&self, document: &DocumentModel) -> Option<Point<f64>> {
        let (_, shape) = self.selected_shape_text_owner(document)?;
        Some(selection_bottom_control_center(&shape, self.camera().zoom))
    }

    pub(crate) fn shape_bound_text_preview(&self) -> Option<ShapeBoundTextPreview> {
        self.state
            .shape_text_drag
            .as_ref()
            .map(|drag| drag.preview.clone())
    }

    pub(crate) fn shape_text_has_invalid_target(&self, document: &DocumentModel) -> bool {
        self.state.shape_text_drag.as_ref().is_some_and(|drag| {
            self.selected_shape_text_owner(document) != Some((drag.preview.owner_id, drag.owner))
                || drag.original_text.as_ref().is_some_and(|original| {
                    document.text(drag.preview.text_id).ok() != Some(original)
                })
        })
    }

    pub(crate) fn process_shape_text_pointer(
        &mut self,
        document: &DocumentModel,
        event: PointerEvent,
    ) -> Result<Option<InteractionOutput>, ErrorCode> {
        if self.shape_text_has_invalid_target(document) {
            self.cancel_interaction();
            return Ok(Some(InteractionOutput {
                consumed: true,
                capture: self.release_capture_command(),
                cursor: CursorCommand::Set(CursorStyle::Default),
            }));
        }
        if let Some(mut drag) = self.state.shape_text_drag.take() {
            if event.pointer_id != drag.pointer_id
                || (event.event_type == PointerEventType::Up
                    && event.button != Some(PointerButton::Primary))
            {
                self.state.shape_text_drag = Some(drag);
                return Ok(Some(InteractionOutput::default()));
            }
            match event.event_type {
                PointerEventType::Move | PointerEventType::Enter | PointerEventType::Up => {
                    drag.moved |= pointer_drag_distance(drag.start_view_position, event.position)
                        >= POINTER_DRAG_THRESHOLD;
                    drag.preview.text.center = if drag.moved {
                        view_to_canvas(event.position, &self.camera(), self.surface_size())
                    } else {
                        drag.initial_center
                    };
                    self.bump_scene_state_revision();
                    self.bump_overlay_state_revision();
                    if event.event_type == PointerEventType::Up {
                        validate_text(&drag.preview.text)?;
                        let text_id = if drag.original_text.is_some() {
                            drag.preview.text_id
                        } else {
                            document.peek_next_element_id()
                        };
                        let mut transaction = Transaction::new("place shape text");
                        if let Some(original) = drag.original_text {
                            if original != drag.preview.text {
                                transaction.update_text(text_id, drag.preview.text);
                            }
                        } else {
                            let mut shape = drag.owner;
                            shape.text_element_id = Some(text_id);
                            transaction.insert_text(
                                text_id,
                                ElementMeta::default(),
                                drag.preview.text,
                            );
                            transaction.update_rectangle(drag.preview.owner_id, shape);
                        }
                        self.cancel_interaction();
                        if !transaction.is_empty() {
                            self.queue_command(EditorCommand::ApplyTransaction(
                                ApplyTransactionCommand::new(transaction),
                            ));
                        }
                        self.state.pending_text_edit = Some(text_id);
                        return Ok(Some(InteractionOutput {
                            consumed: true,
                            capture: self.release_capture_command(),
                            cursor: CursorCommand::Set(CursorStyle::Default),
                        }));
                    }
                    self.state.shape_text_drag = Some(drag);
                    return Ok(Some(InteractionOutput {
                        consumed: true,
                        capture: PointerCaptureCommand::NoChange,
                        cursor: CursorCommand::Set(CursorStyle::Move),
                    }));
                }
                PointerEventType::Cancel => {
                    self.cancel_interaction();
                    self.bump_scene_state_revision();
                    return Ok(Some(InteractionOutput {
                        consumed: true,
                        capture: self.release_capture_command(),
                        cursor: CursorCommand::Set(CursorStyle::Default),
                    }));
                }
                _ => {
                    self.state.shape_text_drag = Some(drag);
                    return Ok(Some(InteractionOutput::default()));
                }
            }
        }

        let down = event.event_type == PointerEventType::Down
            && event.button == Some(PointerButton::Primary);
        let hover = matches!(
            event.event_type,
            PointerEventType::Move | PointerEventType::Enter
        );
        if !down && !hover {
            return Ok(None);
        }
        let Some((owner_id, owner)) = self.selected_shape_text_owner(document) else {
            return Ok(None);
        };
        let center = selection_bottom_control_center(&owner, self.camera().zoom);
        let point = view_to_canvas(event.position, &self.camera(), self.surface_size());
        if point_distance(point, center) > selection_handle_hit_size(self.camera().zoom) / 2.0 {
            return Ok(None);
        }
        if hover {
            return Ok(Some(InteractionOutput {
                consumed: false,
                capture: PointerCaptureCommand::NoChange,
                cursor: CursorCommand::Set(CursorStyle::Move),
            }));
        }
        let existing_id = document.bound_text_id_for_shape(owner_id);
        let original_text = existing_id.and_then(|id| document.text(id).ok().cloned());
        let mut text = match &original_text {
            Some(text) => text.clone(),
            None => crate::text::new_serial_bound_label(&self.state.default_text)?,
        };
        text.center = center;
        self.state.shape_text_generation = self.state.shape_text_generation.wrapping_add(1);
        let text_id = existing_id.unwrap_or(ElementId {
            index: u32::MAX - 2,
            generation: self.state.shape_text_generation,
        });
        self.state.shape_text_drag = Some(ShapeTextDrag {
            pointer_id: event.pointer_id,
            owner,
            measured: original_text.is_some(),
            original_text,
            start_view_position: event.position,
            initial_center: center,
            moved: false,
            preview: ShapeBoundTextPreview {
                owner_id,
                text_id,
                text,
            },
        });
        self.set_hovered_element(None);
        self.bump_scene_state_revision();
        self.bump_overlay_state_revision();
        Ok(Some(InteractionOutput {
            consumed: true,
            capture: self.capture_command_for_start(event.pointer_id),
            cursor: CursorCommand::Set(CursorStyle::Move),
        }))
    }

    pub fn bound_label_layout_request(
        &self,
        document: &DocumentModel,
    ) -> Option<SerialNumberLabelLayoutRequest> {
        if let Some(drag) = &self.state.shape_text_drag {
            return (!drag.measured && !self.shape_text_has_invalid_target(document)).then(|| {
                SerialNumberLabelLayoutRequest {
                    text_id: drag.preview.text_id,
                    font_size: drag.preview.text.font_size,
                    font_family: drag.preview.text.font_family.clone(),
                }
            });
        }
        self.serial_number_label_layout_request(document)
    }

    pub fn apply_bound_label_layout(
        &mut self,
        document: &DocumentModel,
        text_id: ElementId,
        layout: TextLayoutSize,
    ) -> Result<bool, ErrorCode> {
        let layout = validate_text_layout_size(layout)?;
        if self.shape_text_has_invalid_target(document) {
            return Ok(false);
        }
        if let Some(drag) = &mut self.state.shape_text_drag {
            if drag.measured || drag.preview.text_id != text_id {
                return Ok(false);
            }
            // The handle/pointer is the placement anchor. Measurement changes the
            // empty label's size, never its center or the shape's geometry.
            drag.preview.text = text_with_measured_layout(&drag.preview.text, layout)?;
            drag.measured = true;
            self.bump_scene_state_revision();
            self.bump_overlay_state_revision();
            return Ok(true);
        }
        self.apply_serial_number_label_layout(document, text_id, layout)
    }
}
