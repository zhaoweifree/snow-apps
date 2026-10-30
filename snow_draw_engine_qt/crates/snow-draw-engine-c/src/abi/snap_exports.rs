use crate::abi::{handles::*, types::*};
use snow_draw_engine::{SnapConfig, SnapGuideDisplayItem};
use snow_draw_engine_core::{
    DrawRect, OBJECT_SNAP_SERVICE, ObjectSnapMoveConstraints, ObjectSnapRectRequest, SnapAxisAnchor,
};

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct SnowSnapRect {
    pub min_x: f64,
    pub min_y: f64,
    pub max_x: f64,
    pub max_y: f64,
}

impl SnowSnapRect {
    fn valid(self) -> bool {
        [self.min_x, self.min_y, self.max_x, self.max_y]
            .iter()
            .all(|v| v.is_finite())
            && self.max_x > self.min_x
            && self.max_y > self.min_y
    }

    fn rect(self) -> DrawRect {
        DrawRect::new(self.min_x, self.min_y, self.max_x, self.max_y)
    }
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct SnowSnapMoveConstraints {
    pub bounds: SnowSnapRect,
    pub position_step: f64,
}

impl SnowSnapMoveConstraints {
    fn valid(self, target: DrawRect) -> bool {
        if !self.bounds.valid() || !self.position_step.is_finite() || self.position_step <= 0.0 {
            return false;
        }
        // At least one lattice position must fit the entire target on each axis.
        [
            (self.bounds.min_x, self.bounds.max_x - target.width()),
            (self.bounds.min_y, self.bounds.max_y - target.height()),
        ]
        .iter()
        .all(|(min, max)| {
            let first = (min / self.position_step).ceil();
            let last = (max / self.position_step).floor();
            first.is_finite() && last.is_finite() && first <= last
        })
    }
}

/// Owns the computed result, so reading its variable-length guides never repeats snapping.
pub struct SnowMoveSnap {
    correction: SnowArrowPoint,
    guides: Vec<SnowOverlayDisplayItem>,
}

/// Explicit object snapping; persistent enabled is ignored. Null constraints leaves
/// editor geometry continuous. On success the caller owns output and must free it.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_snap_move(
    target: *const SnowSnapRect,
    references: *const SnowSnapRect,
    reference_count: usize,
    config: *const SnowSnapConfig,
    canvas_to_view_scale: f64,
    constraints: *const SnowSnapMoveConstraints,
    output: *mut *mut SnowMoveSnap,
) -> SnowError {
    ffi_error(|| {
        if output.is_null() {
            return SnowError::InvalidArgument;
        }
        write_out(output, std::ptr::null_mut());
        if target.is_null()
            || config.is_null()
            || (references.is_null() && reference_count != 0)
            || reference_count > 65536
            || !canvas_to_view_scale.is_finite()
            || canvas_to_view_scale <= 0.0
        {
            return SnowError::InvalidArgument;
        }
        let target = unsafe { *target };
        let config = unsafe { *config };
        let references = if reference_count == 0 {
            &[][..]
        } else {
            unsafe { std::slice::from_raw_parts(references, reference_count) }
        };
        let distance = config.distance / canvas_to_view_scale.max(0.0001);
        if !target.valid()
            || references.iter().any(|r| !r.valid())
            || !distance.is_finite()
            || distance < 0.0
        {
            return SnowError::InvalidArgument;
        }
        let limits = if constraints.is_null() {
            None
        } else {
            let value = unsafe { *constraints };
            if !value.valid(target.rect()) {
                return SnowError::InvalidArgument;
            }
            Some(ObjectSnapMoveConstraints {
                bounds: value.bounds.rect(),
                position_step: value.position_step,
            })
        };
        let references: Vec<_> = references.iter().map(|r| r.rect()).collect();
        let anchors = [
            SnapAxisAnchor::Start,
            SnapAxisAnchor::Center,
            SnapAxisAnchor::End,
        ];
        let result = OBJECT_SNAP_SERVICE.snap_rect_with_move_constraints(
            ObjectSnapRectRequest {
                target_rect: target.rect(),
                reference_rects: &references,
                snap_distance: distance,
                target_anchors_x: &anchors,
                target_anchors_y: &anchors,
                enable_point_snaps: config.enable_point_snaps != 0,
                enable_gap_snaps: config.enable_gap_snaps != 0,
            },
            limits,
        );
        let style: SnapConfig = config.into();
        let guides = if config.show_guides != 0 {
            result
                .guides
                .iter()
                .map(|guide| (&SnapGuideDisplayItem::from_guide(guide, style)).into())
                .collect()
        } else {
            Vec::new()
        };
        write_out(
            output,
            Box::into_raw(Box::new(SnowMoveSnap {
                correction: SnowArrowPoint {
                    x: result.dx,
                    y: result.dy,
                },
                guides,
            })),
        );
        SnowError::Ok
    })
}

/// Borrows immutable guides until result is freed. Empty results have a null data pointer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_move_snap_read(
    result: *const SnowMoveSnap,
    correction: *mut SnowArrowPoint,
    guides: *mut *const SnowOverlayDisplayItem,
    count: *mut usize,
) -> SnowError {
    ffi_error(|| {
        if result.is_null() || correction.is_null() || guides.is_null() || count.is_null() {
            return SnowError::InvalidArgument;
        }
        let result = unsafe { &*result };
        write_out(correction, result.correction);
        write_out(
            guides,
            if result.guides.is_empty() {
                std::ptr::null()
            } else {
                result.guides.as_ptr()
            },
        );
        write_out(count, result.guides.len());
        SnowError::Ok
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_move_snap_free(result: *mut SnowMoveSnap) {
    ffi_void(|| {
        if !result.is_null() {
            drop(unsafe { Box::from_raw(result) });
        }
    });
}

#[cfg(test)]
mod tests {
    use super::*;
    use snow_draw_engine_core::{SnapGuideAxis, SnapGuideKind};

    fn rect(x: f64, y: f64, width: f64, height: f64) -> SnowSnapRect {
        SnowSnapRect {
            min_x: x,
            min_y: y,
            max_x: x + width,
            max_y: y + height,
        }
    }

    #[test]
    fn move_bridge_matches_rectangle_service() {
        let references = [rect(20.0, 30.0, 40.0, 40.0), rect(100.0, 30.0, 40.0, 40.0)];
        for scale in [0.5, 1.0, 1.5, 2.0] {
            for x in [-100.0, 18.0, 57.0, 83.5, 102.0, 176.0, 190.0] {
                for (points, gaps) in [(1, 1), (0, 1), (1, 0), (0, 0)] {
                    let target = rect(x, 32.0, 40.0, 40.0);
                    let config = SnowSnapConfig {
                        enable_point_snaps: points,
                        enable_gap_snaps: gaps,
                        show_gap_size: 1,
                        ..Default::default()
                    };
                    let expected = OBJECT_SNAP_SERVICE.snap_move(
                        target.rect(),
                        &references.map(SnowSnapRect::rect),
                        config.distance / scale,
                        points != 0,
                        gaps != 0,
                    );
                    let mut result = std::ptr::null_mut();
                    unsafe {
                        assert_eq!(
                            snow_snap_move(
                                &target,
                                references.as_ptr(),
                                2,
                                &config,
                                scale,
                                std::ptr::null(),
                                &mut result
                            ),
                            SnowError::Ok
                        );
                        let mut correction = SnowArrowPoint::default();
                        let mut guides = std::ptr::null();
                        let mut count = 0;
                        assert_eq!(
                            snow_move_snap_read(result, &mut correction, &mut guides, &mut count),
                            SnowError::Ok
                        );
                        assert_eq!((correction.x, correction.y), (expected.dx, expected.dy));
                        assert_eq!(count, expected.guides.len());
                        let actual = if count == 0 {
                            &[][..]
                        } else {
                            std::slice::from_raw_parts(guides, count)
                        };
                        for (actual, expected) in actual.iter().zip(&expected.guides) {
                            assert_eq!(
                                (actual.snap_start_x, actual.snap_start_y),
                                (expected.start.x, expected.start.y)
                            );
                            assert_eq!(
                                (actual.snap_end_x, actual.snap_end_y),
                                (expected.end.x, expected.end.y)
                            );
                            assert_eq!(
                                actual.snap_marker_count as usize,
                                expected.markers.len().min(2)
                            );
                            assert_eq!(actual.snap_has_label != 0, expected.label.is_some());
                            assert_eq!(actual.snap_label, expected.label.unwrap_or(0.0));
                            assert_eq!(
                                actual.snap_guide_kind == SnowSnapGuideKind::Gap,
                                expected.kind == SnapGuideKind::Gap
                            );
                            assert_eq!(
                                actual.snap_guide_axis == SnowSnapGuideAxis::Vertical,
                                expected.axis == SnapGuideAxis::Vertical
                            );
                            for (index, marker) in expected.markers.iter().take(2).enumerate() {
                                let point = if index == 0 {
                                    (actual.snap_marker0_x, actual.snap_marker0_y)
                                } else {
                                    (actual.snap_marker1_x, actual.snap_marker1_y)
                                };
                                assert_eq!(point, (marker.x, marker.y));
                            }
                            assert_eq!(actual.snap_line_width, config.line_width);
                            assert_eq!(actual.snap_marker_size, config.marker_size);
                        }
                        // Multiple reads borrow the same immutable storage.
                        let first = guides;
                        assert_eq!(
                            snow_move_snap_read(result, &mut correction, &mut guides, &mut count),
                            SnowError::Ok
                        );
                        assert_eq!(first, guides);
                        snow_move_snap_free(result);
                    }
                }
            }
        }
    }

    #[test]
    fn move_bridge_validates_inputs_and_owns_results() {
        let target = rect(0.0, 0.0, 40.0, 40.0);
        let config = SnowSnapConfig::default();
        let mut output = std::ptr::null_mut();
        unsafe {
            for scale in [0.0, -1.0, f64::NAN, f64::INFINITY] {
                assert_eq!(
                    snow_snap_move(
                        &target,
                        &target,
                        1,
                        &config,
                        scale,
                        std::ptr::null(),
                        &mut output
                    ),
                    SnowError::InvalidArgument
                );
                assert!(output.is_null());
            }
            let invalid = rect(f64::NAN, 0.0, 40.0, 40.0);
            for (moving, refs, count) in [
                (std::ptr::null(), &target as *const _, 1),
                (&invalid, &target, 1),
                (&target, &invalid, 1),
                (&target, std::ptr::null(), 1),
                (&target, &target, 65537),
            ] {
                assert_eq!(
                    snow_snap_move(
                        moving,
                        refs,
                        count,
                        &config,
                        1.0,
                        std::ptr::null(),
                        &mut output
                    ),
                    SnowError::InvalidArgument
                );
            }
            for limits in [
                SnowSnapMoveConstraints {
                    bounds: target,
                    position_step: 0.0,
                },
                SnowSnapMoveConstraints {
                    bounds: invalid,
                    position_step: 1.0,
                },
                SnowSnapMoveConstraints {
                    bounds: rect(0.5, 0.5, 40.0, 40.0),
                    position_step: 1.0,
                },
                SnowSnapMoveConstraints {
                    bounds: rect(0.0, 0.0, 20.0, 20.0),
                    position_step: 1.0,
                },
            ] {
                assert_eq!(
                    snow_snap_move(&target, &target, 1, &config, 1.0, &limits, &mut output),
                    SnowError::InvalidArgument
                );
            }
            let mut correction = SnowArrowPoint::default();
            let mut guides = std::ptr::null();
            let mut count = 99;
            assert_eq!(
                snow_move_snap_read(std::ptr::null(), &mut correction, &mut guides, &mut count),
                SnowError::InvalidArgument
            );
            assert_eq!(
                snow_snap_move(
                    &target,
                    std::ptr::null(),
                    0,
                    &config,
                    1.0,
                    std::ptr::null(),
                    &mut output
                ),
                SnowError::Ok
            );
            assert_eq!(
                snow_move_snap_read(output, &mut correction, &mut guides, &mut count),
                SnowError::Ok
            );
            assert_eq!(count, 0);
            assert!(guides.is_null());
            assert_eq!((correction.x, correction.y), (0.0, 0.0));
            // An independent result cannot invalidate the first result's storage.
            let first = output;
            let hidden = SnowSnapConfig {
                show_guides: 0,
                ..config
            };
            assert_eq!(
                snow_snap_move(
                    &target,
                    &target,
                    1,
                    &hidden,
                    1.0,
                    std::ptr::null(),
                    &mut output
                ),
                SnowError::Ok
            );
            assert_eq!(
                snow_move_snap_read(first, &mut correction, &mut guides, &mut count),
                SnowError::Ok
            );
            assert_eq!(
                snow_move_snap_read(output, &mut correction, &mut guides, &mut count),
                SnowError::Ok
            );
            assert_eq!(count, 0);
            snow_move_snap_free(first);
            snow_move_snap_free(output);
            snow_move_snap_free(std::ptr::null_mut());
        }
    }
}
