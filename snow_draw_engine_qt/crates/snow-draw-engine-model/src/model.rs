use std::ops::Deref;

use crate::query::QueryStore;
use snow_draw_engine_core::{ErrorCode, Point, SnapQuery, SnapResult, ViewportQuery};
use snow_draw_engine_document::{
    ApplyResult, BindableElementState, Document, DocumentRevision, ElementId, ElementKind,
    ElementState, RectangleData, Transaction,
};

#[derive(Debug)]
pub struct DocumentModel {
    document: Document,
    queries: QueryStore,
    auto_filter_generation: u64,
}

fn next_region_generation() -> u64 {
    static NEXT: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(1);
    NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed)
}

impl Clone for DocumentModel {
    fn clone(&self) -> Self {
        let document = self.document.clone();
        let queries = QueryStore::new(&document);
        Self {
            document,
            queries,
            auto_filter_generation: next_region_generation(),
        }
    }
}

impl Default for DocumentModel {
    fn default() -> Self {
        let document = Document::new();
        let queries = QueryStore::new(&document);
        Self {
            document,
            queries,
            auto_filter_generation: next_region_generation(),
        }
    }
}

impl DocumentModel {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn from_document(mut document: Document) -> Result<Self, ErrorCode> {
        document.normalize_filter_invariants();
        document.normalize_angle_invariants()?;
        document.validate_session()?;
        document.normalize_distance_label_invariants()?;
        let queries = QueryStore::new(&document);
        Ok(Self {
            document,
            queries,
            auto_filter_generation: next_region_generation(),
        })
    }

    pub fn auto_filter_generation(&self) -> u64 {
        self.auto_filter_generation
    }

    pub fn relation_index_build_count(&self) -> u64 {
        self.queries.relations().rebuild_count()
    }
    pub fn arrow_label_bindings(&self) -> &[(ElementId, ElementId)] {
        self.queries.relations().arrow_label_bindings()
    }
    pub fn arrow_label_owner(&self, text: ElementId) -> Option<ElementId> {
        self.queries.relations().arrow_label_owner(text)
    }
    pub fn arrow_id_for_text(&self, text: ElementId) -> Option<ElementId> {
        self.arrow_label_owner(text)
    }
    pub fn speech_bubble_text_bindings(&self) -> &[(ElementId, ElementId)] {
        self.queries.relations().speech_bubble_text_bindings()
    }
    pub fn speech_bubble_id_for_text(&self, text: ElementId) -> Option<ElementId> {
        self.queries.relations().speech_bubble_id_for_text(text)
    }
    pub fn text_owner_id(&self, text: ElementId) -> Option<ElementId> {
        self.queries.relations().text_owner_id(text)
    }
    pub fn is_text_bound_to_serial_number(&self, text: ElementId) -> bool {
        !self.serials_for_text(text).is_empty()
    }
    pub fn serials_for_text(&self, text: ElementId) -> &[ElementId] {
        self.queries.relations().serials_for_text(text)
    }

    pub fn shapes_for_text(&self, text: ElementId) -> &[ElementId] {
        self.queries.relations().shapes_for_text(text)
    }

    pub fn connector_text_ids(&self) -> impl Iterator<Item = ElementId> + '_ {
        self.queries.relations().connector_text_ids()
    }

    pub fn document(&self) -> &Document {
        &self.document
    }

    pub fn document_revision(&self) -> DocumentRevision {
        self.document.document_revision()
    }

    pub fn allocate_element_id(&mut self) -> ElementId {
        self.document.allocate_element_id()
    }

    pub fn peek_next_element_id(&self) -> ElementId {
        self.document.peek_next_element_id()
    }

    pub fn apply_transaction(
        &mut self,
        transaction: Transaction,
    ) -> Result<ApplyResult, ErrorCode> {
        self.apply_transaction_ref(&transaction)
    }

    pub fn apply_history_transaction(
        &mut self,
        transaction: &Transaction,
    ) -> Result<ApplyResult, ErrorCode> {
        self.apply_transaction_ref(transaction)
    }

    fn apply_transaction_ref(
        &mut self,
        transaction: &Transaction,
    ) -> Result<ApplyResult, ErrorCode> {
        let apply_result = self.document.apply(transaction)?;
        if transaction.operations().iter().any(|op| {
            matches!(
                op,
                snow_draw_engine_document::Operation::UpdateAutoFilterRegions { .. }
            )
        }) {
            self.auto_filter_generation = next_region_generation();
        }
        self.queries.refresh(&self.document, &apply_result.changes);
        Ok(apply_result)
    }

    pub fn visible_scene_item_count(&self, query: ViewportQuery) -> u32 {
        self.queries.visible_scene_item_count(&self.document, query)
    }

    pub fn visible_element_ids(&self, query: ViewportQuery) -> Vec<ElementId> {
        self.queries.visible_element_ids(&self.document, query)
    }

    pub fn paint_rank(&self, id: ElementId) -> Option<u32> {
        self.queries.paint_rank(id)
    }

    /// Use the maintained paint ranks instead of searching the document order
    /// each time a scene cache refreshes an individual element.
    pub fn element_state(&self, id: ElementId) -> Result<ElementState<'_>, ErrorCode> {
        let element = self.document.element(id)?;
        let z_index = self.queries.paint_rank(id).ok_or(ErrorCode::NotFound)? as usize;
        Ok(ElementState {
            id,
            rect: element.data.state_rect(),
            rotation: element.data.rotation(),
            opacity: element.data.opacity(),
            z_index,
            visible: element.meta.visible,
            locked: element.meta.locked,
            data: &element.data,
        })
    }

    pub fn for_each_visible_scene_rect(
        &self,
        query: ViewportQuery,
        f: impl FnMut(ElementId, &RectangleData),
    ) {
        self.queries
            .for_each_visible_scene_rect(&self.document, query, f)
    }

    pub fn topmost_element_at_with_tolerance(
        &self,
        point: Point<f64>,
        hit_tolerance: f64,
    ) -> Option<(ElementId, ElementKind)> {
        self.queries
            .topmost_element_at_with_tolerance(&self.document, point, hit_tolerance)
    }

    pub fn elements_at_with_tolerance(
        &self,
        point: Point<f64>,
        hit_tolerance: f64,
    ) -> Vec<(ElementId, ElementKind)> {
        self.queries
            .elements_at_with_tolerance(&self.document, point, hit_tolerance)
    }

    pub fn topmost_element_matching_at_with_tolerance(
        &self,
        point: Point<f64>,
        hit_tolerance: f64,
        include: impl FnMut(ElementId, ElementKind) -> bool,
    ) -> Option<(ElementId, ElementKind)> {
        self.queries.topmost_element_matching_at_with_tolerance(
            &self.document,
            point,
            hit_tolerance,
            include,
        )
    }

    pub fn bound_arrow_ids(&self, bindable_id: ElementId) -> &[ElementId] {
        self.queries.bound_arrow_ids(bindable_id)
    }

    pub fn bound_bindable_ids(&self, arrow_id: ElementId) -> &[ElementId] {
        self.queries.bound_bindable_ids(arrow_id)
    }

    pub fn bindable_element_states_with_overrides(
        &self,
        overrides: &[(ElementId, RectangleData)],
    ) -> Vec<BindableElementState> {
        let overrides_by_id = overrides
            .iter()
            .copied()
            .collect::<std::collections::HashMap<_, _>>();

        self.document
            .element_states()
            .filter_map(|state| {
                if !state.visible
                    || state.data.is_background_restore()
                    || self.text_owner_id(state.id).is_some()
                {
                    return None;
                }
                let rect = overrides_by_id
                    .get(&state.id)
                    .copied()
                    .or_else(|| self.document.element_rect_proxy(state.id))?;
                Some(BindableElementState::rectangle_proxy(
                    state.id,
                    rect,
                    state.z_index as u32,
                    state.locked,
                ))
            })
            .collect()
    }

    pub fn snap_point(&self, query: &SnapQuery) -> SnapResult {
        self.queries.snap_point(&self.document, query)
    }

    pub fn snap_point_filtered(
        &self,
        query: &SnapQuery,
        include_candidate: impl FnMut(ElementId, &RectangleData) -> bool,
    ) -> SnapResult {
        self.queries
            .snap_point_filtered(&self.document, query, include_candidate)
    }
}

impl Deref for DocumentModel {
    type Target = Document;

    fn deref(&self) -> &Self::Target {
        &self.document
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use snow_draw_engine_core::{Camera, ColorRgba8, CornerRadii, SurfaceSize};
    use snow_draw_engine_document::{ElementMeta, FillStyle};

    fn rect(center_x: f64) -> RectangleData {
        RectangleData {
            text_element_id: None,
            stroke_rendering_style: Default::default(),
            stroke_rendering_seed: 0,
            rectangle_kind: snow_draw_engine_document::RectangleElementKind::Rectangle,
            highlight_shape: snow_draw_engine_document::HighlightShape::Rectangle,
            center: Point::new(center_x, 0.0),
            width: 40.0,
            height: 40.0,
            rotation: 0.0,
            fill: ColorRgba8::default(),
            fill_style: FillStyle::Solid,
            stroke: ColorRgba8::default(),
            stroke_width: 0.0,
            stroke_style: snow_draw_engine_document::StrokeStyle::Solid,
            corner_radii: CornerRadii::default(),
            opacity: 1.0,
        }
    }

    fn viewport_query() -> ViewportQuery {
        ViewportQuery {
            camera: Camera {
                center: Point::new(0.0, 0.0),
                zoom: 1.0,
            },
            surface: SurfaceSize {
                width: 400,
                height: 300,
            },
        }
    }

    #[test]
    fn visible_element_ids_are_spatially_filtered_and_paint_ordered() {
        let first = ElementId {
            index: 0,
            generation: 1,
        };
        let second = ElementId {
            index: 1,
            generation: 1,
        };
        let offscreen = ElementId {
            index: 2,
            generation: 1,
        };
        let mut model = DocumentModel::new();
        let mut transaction = Transaction::new("visible candidates");
        transaction.insert_rectangle(first, ElementMeta::default(), rect(-50.0));
        transaction.insert_rectangle(second, ElementMeta::default(), rect(50.0));
        transaction.insert_rectangle(offscreen, ElementMeta::default(), rect(1000.0));
        model.apply_transaction(transaction).unwrap();

        assert_eq!(
            model.visible_element_ids(viewport_query()),
            vec![first, second]
        );

        let mut reorder = Transaction::new("reorder visible candidates");
        reorder.reorder_elements(vec![second], 0);
        model.apply_transaction(reorder).unwrap();
        assert_eq!(
            model.visible_element_ids(viewport_query()),
            vec![second, first]
        );
    }

    #[test]
    fn element_state_uses_cached_paint_ranks_after_reorder_delete_restore_and_generation_changes() {
        let mut model = DocumentModel::new();
        let ids = [
            model.allocate_element_id(),
            model.allocate_element_id(),
            model.allocate_element_id(),
        ];
        let mut insert = Transaction::new("insert state fixtures");
        for (index, id) in ids.into_iter().enumerate() {
            let mut rectangle = rect(index as f64 * 100.0);
            rectangle.rotation = index as f64 * 0.25;
            rectangle.opacity = 0.25 + index as f64 * 0.25;
            insert.insert_rectangle(
                id,
                ElementMeta {
                    visible: index != 1,
                    locked: index == 1,
                },
                rectangle,
            );
        }
        model.apply_transaction(insert).unwrap();
        let assert_states_match = |model: &DocumentModel| {
            for id in model.paint_order() {
                assert_eq!(
                    model.element_state(*id),
                    model.document().element_state(*id)
                );
            }
        };
        assert_states_match(&model);

        let mut reorder = Transaction::new("reorder state fixtures");
        reorder.reorder_elements(vec![ids[2], ids[0]], 0);
        model.apply_transaction(reorder).unwrap();
        assert_states_match(&model);
        assert_eq!(model.element_state(ids[2]).unwrap().z_index, 0);
        assert_eq!(model.element_state(ids[1]).unwrap().z_index, 2);

        let mut remove = Transaction::new("remove state fixture");
        remove.remove_element(ids[0]);
        let removed = model.apply_transaction(remove).unwrap();
        assert_eq!(model.element_state(ids[0]), Err(ErrorCode::NotFound));
        assert_states_match(&model);
        model.apply_history_transaction(&removed.inverse).unwrap();
        assert_states_match(&model);

        let stale = ElementId {
            generation: ids[0].generation + 1,
            ..ids[0]
        };
        assert_eq!(model.element_state(stale), Err(ErrorCode::NotFound));
        assert_eq!(model.paint_rank(stale), None);
        let mut remove = Transaction::new("replace state fixture generation");
        remove.remove_element(ids[0]);
        model.apply_transaction(remove).unwrap();
        let mut replace = Transaction::new("insert next generation");
        replace.insert_rectangle(stale, ElementMeta::default(), rect(300.0));
        model.apply_transaction(replace).unwrap();
        assert_eq!(model.element_state(ids[0]), Err(ErrorCode::NotFound));
        assert_eq!(model.paint_rank(ids[0]), None);
        assert_states_match(&model);
        assert_states_match(&model.clone());
    }

    #[test]
    fn shape_text_relations_refresh_and_undo_without_internal_label_ownership() {
        use snow_draw_engine_document::TextData;
        let mut model = DocumentModel::new();
        let owner = model.allocate_element_id();
        let label = model.allocate_element_id();
        let mut shape = rect(100.0);
        shape.text_element_id = Some(label);
        let mut insert = Transaction::new("shape binding");
        insert.insert_rectangle(owner, ElementMeta::default(), shape);
        insert.insert_text(label, ElementMeta::default(), TextData::default());
        model.apply_transaction(insert).unwrap();
        assert_eq!(model.shapes_for_text(label), &[owner]);
        assert_eq!(model.connector_text_ids().collect::<Vec<_>>(), vec![label]);
        assert_eq!(model.text_owner_id(label), None);
        let builds = model.relation_index_build_count();
        shape.center.x += 20.0;
        let mut move_shape = Transaction::new("move shape");
        move_shape.update_rectangle(owner, shape);
        model.apply_transaction(move_shape).unwrap();
        assert_eq!(
            model.text(label).unwrap().center,
            TextData::default().center
        );
        assert_eq!(model.relation_index_build_count(), builds);
        shape.text_element_id = None;
        let mut unlink = Transaction::new("unlink shape text");
        unlink.update_rectangle(owner, shape);
        let removed = model.apply_transaction(unlink).unwrap();
        assert!(model.shapes_for_text(label).is_empty());
        assert!(model.connector_text_ids().next().is_none());
        model.apply_history_transaction(&removed.inverse).unwrap();
        assert_eq!(model.shapes_for_text(label), &[owner]);
    }

    #[test]
    fn speech_bubble_relations_and_tail_spatial_bounds_refresh_locally() {
        use snow_draw_engine_document::{SpeechBubbleData, TextData, speech_bubble_default_tail};
        let mut model = DocumentModel::new();
        let owner = model.allocate_element_id();
        let label = model.allocate_element_id();
        let mut body = rect(500.0);
        body.fill.a = 255;
        let mut bubble = SpeechBubbleData::new(body);
        bubble.text_element_id = Some(label);
        bubble.tails.push(speech_bubble_default_tail(
            &bubble,
            Point::new(-450.0, 0.0),
            1.0,
        ));
        let mut insert = Transaction::new("bubble fixture");
        insert.insert_speech_bubble(owner, ElementMeta::default(), bubble.clone());
        insert.insert_text(label, ElementMeta::default(), TextData::default());
        model.apply_transaction(insert).unwrap();
        assert_eq!(model.speech_bubble_text_bindings(), &[(owner, label)]);
        assert_eq!(model.text_owner_id(label), Some(owner));
        assert_eq!(model.speech_bubble_id_for_text(label), Some(owner));
        assert!(model.visible_element_ids(viewport_query()).contains(&owner));
        assert_eq!(
            model.topmost_element_at_with_tolerance(Point::new(50.0, 0.0), 1.0),
            Some((owner, ElementKind::SpeechBubble))
        );
        assert!(
            !model
                .bindable_element_states_with_overrides(&[])
                .iter()
                .any(|state| state.id() == label)
        );
        let snapped = model.snap_point(&SnapQuery {
            point: Point::new(519.5, 19.5),
            threshold: 1.0,
            include_grid: false,
            grid_size: 10.0,
        });
        assert_eq!(snapped.point, Point::new(520.0, 20.0));
        let builds = model.relation_index_build_count();
        bubble.body.fill.a = 130;
        let mut style = Transaction::new("bubble appearance");
        style.update_speech_bubble(owner, bubble.clone());
        model.apply_transaction(style).unwrap();
        assert_eq!(model.relation_index_build_count(), builds);
        bubble.tails.clear();
        let mut remove_tail = Transaction::new("remove tail");
        remove_tail.update_speech_bubble(owner, bubble);
        model.apply_transaction(remove_tail).unwrap();
        assert!(!model.visible_element_ids(viewport_query()).contains(&owner));
        assert_eq!(model.relation_index_build_count(), builds);
        let mut remove = Transaction::new("remove bubble and label");
        remove.remove_element(owner);
        remove.remove_element(label);
        let removed = model.apply_transaction(remove).unwrap();
        assert_eq!(model.text_owner_id(label), None);
        model.apply_history_transaction(&removed.inverse).unwrap();
        assert_eq!(model.text_owner_id(label), Some(owner));
    }
}
