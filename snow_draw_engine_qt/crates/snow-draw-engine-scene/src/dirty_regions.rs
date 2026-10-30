use snow_draw_engine_core::SurfaceSize;
use snow_draw_engine_display::{DirtyRegion, full_surface_dirty_region};

const DIRTY_REGION_LIMIT: usize = 256;
const DIRTY_BUCKET_AXIS: usize = 16;
const UNION_AREA_FACTOR: f64 = 1.5;

/// Pressure contours can change at several separated points of a looping stroke.
/// Merge only rectangular unions; touching corners must not fill the untouched interior.
pub(crate) fn finalize_sparse_dirty_regions(
    regions: Vec<DirtyRegion>,
    surface: SurfaceSize,
) -> Vec<DirtyRegion> {
    if surface.width == 0 || surface.height == 0 {
        return Vec::new();
    }
    let mut clipped = regions
        .into_iter()
        .filter_map(|region| clip_dirty_region(region, surface))
        .collect::<Vec<_>>();
    if clipped.len() > DIRTY_REGION_LIMIT {
        let cell_width = surface.width as f64 / DIRTY_BUCKET_AXIS as f64;
        let cell_height = surface.height as f64 / DIRTY_BUCKET_AXIS as f64;
        let mut occupied = [false; DIRTY_BUCKET_AXIS * DIRTY_BUCKET_AXIS];
        for region in clipped {
            let left = (region.min_x / cell_width).floor() as usize;
            let top = (region.min_y / cell_height).floor() as usize;
            let right = (region.max_x / cell_width).ceil() as usize;
            let bottom = (region.max_y / cell_height).ceil() as usize;
            for y in top..bottom.min(DIRTY_BUCKET_AXIS) {
                for x in left..right.min(DIRTY_BUCKET_AXIS) {
                    occupied[y * DIRTY_BUCKET_AXIS + x] = true;
                }
            }
        }
        clipped = occupied
            .into_iter()
            .enumerate()
            .filter(|(_, used)| *used)
            .map(|(index, _)| {
                let x = index % DIRTY_BUCKET_AXIS;
                let y = index / DIRTY_BUCKET_AXIS;
                DirtyRegion::new(
                    x as f64 * cell_width,
                    y as f64 * cell_height,
                    (x + 1) as f64 * cell_width,
                    (y + 1) as f64 * cell_height,
                )
            })
            .collect();
    }
    let mut merged = Vec::new();
    for mut next in clipped {
        let mut index = 0;
        while index < merged.len() {
            let existing: DirtyRegion = merged[index];
            let overlap = (existing.max_x.min(next.max_x) - existing.min_x.max(next.min_x))
                .max(0.0)
                * (existing.max_y.min(next.max_y) - existing.min_y.max(next.min_y)).max(0.0);
            let covered = dirty_region_area(existing) + dirty_region_area(next) - overlap;
            if dirty_regions_touch(existing, next)
                && dirty_region_area(existing.union(next)) <= covered + 1e-9
            {
                next = next.union(merged.swap_remove(index));
                index = 0;
            } else {
                index += 1;
            }
        }
        merged.push(next);
    }
    merged
}

pub(crate) fn finalize_dirty_regions(
    regions: Vec<DirtyRegion>,
    surface: SurfaceSize,
) -> Vec<DirtyRegion> {
    if regions.is_empty() || surface.width == 0 || surface.height == 0 {
        return Vec::new();
    }

    let mut clipped = regions
        .into_iter()
        .filter_map(|region| clip_dirty_region(region, surface))
        .collect::<Vec<_>>();
    if clipped.len() > DIRTY_REGION_LIMIT {
        clipped = bucket_dirty_regions(clipped, surface);
    }

    let mut merged = Vec::new();
    for clipped in clipped {
        merge_dirty_region(&mut merged, clipped);
    }

    if merged.is_empty() {
        return merged;
    }

    let union = merged
        .iter()
        .copied()
        .reduce(DirtyRegion::union)
        .expect("merged dirty regions cannot be empty");
    if covers_full_surface(union, surface) {
        return full_surface_dirty_region(surface);
    }
    const EPSILON: f64 = 1e-9;
    let separate_area = merged.iter().copied().map(dirty_region_area).sum::<f64>();
    if dirty_region_area(union) <= separate_area * UNION_AREA_FACTOR + EPSILON {
        return vec![union];
    }

    merged
}

fn dirty_region_area(region: DirtyRegion) -> f64 {
    (region.max_x - region.min_x).max(0.0) * (region.max_y - region.min_y).max(0.0)
}

fn bucket_dirty_regions(regions: Vec<DirtyRegion>, surface: SurfaceSize) -> Vec<DirtyRegion> {
    let mut buckets = vec![None::<DirtyRegion>; DIRTY_BUCKET_AXIS * DIRTY_BUCKET_AXIS];
    let width = (surface.width as f64).max(1.0);
    let height = (surface.height as f64).max(1.0);
    for region in regions {
        let center_x = (region.min_x + region.max_x) * 0.5;
        let center_y = (region.min_y + region.max_y) * 0.5;
        let x = ((center_x / width) * DIRTY_BUCKET_AXIS as f64)
            .floor()
            .clamp(0.0, (DIRTY_BUCKET_AXIS - 1) as f64) as usize;
        let y = ((center_y / height) * DIRTY_BUCKET_AXIS as f64)
            .floor()
            .clamp(0.0, (DIRTY_BUCKET_AXIS - 1) as f64) as usize;
        let bucket = &mut buckets[y * DIRTY_BUCKET_AXIS + x];
        *bucket = Some(bucket.map_or(region, |existing| existing.union(region)));
    }
    buckets.into_iter().flatten().collect()
}

fn merge_dirty_region(regions: &mut Vec<DirtyRegion>, mut next: DirtyRegion) {
    let mut index = 0;
    while index < regions.len() {
        if dirty_regions_touch(regions[index], next) {
            next = next.union(regions.swap_remove(index));
            index = 0;
        } else {
            index += 1;
        }
    }
    regions.push(next);
}

fn dirty_regions_touch(left: DirtyRegion, right: DirtyRegion) -> bool {
    left.max_x >= right.min_x
        && right.max_x >= left.min_x
        && left.max_y >= right.min_y
        && right.max_y >= left.min_y
}

pub(crate) fn clip_dirty_region(region: DirtyRegion, surface: SurfaceSize) -> Option<DirtyRegion> {
    let clipped = DirtyRegion::new(
        region.min_x.clamp(0.0, surface.width as f64),
        region.min_y.clamp(0.0, surface.height as f64),
        region.max_x.clamp(0.0, surface.width as f64),
        region.max_y.clamp(0.0, surface.height as f64),
    );
    (!clipped.is_empty()).then_some(clipped)
}

fn covers_full_surface(region: DirtyRegion, surface: SurfaceSize) -> bool {
    region.min_x <= 0.0
        && region.min_y <= 0.0
        && region.max_x >= surface.width as f64
        && region.max_y >= surface.height as f64
}

#[cfg(test)]
mod tests {
    use super::*;

    fn surface() -> SurfaceSize {
        SurfaceSize {
            width: 1000,
            height: 1000,
        }
    }

    #[test]
    fn sparse_dirty_regions_remain_separate() {
        let regions = finalize_dirty_regions(
            vec![
                DirtyRegion::new(10.0, 10.0, 30.0, 30.0),
                DirtyRegion::new(900.0, 900.0, 920.0, 920.0),
            ],
            surface(),
        );
        assert_eq!(regions.len(), 2);
    }

    #[test]
    fn nearby_dirty_regions_use_their_inexpensive_union() {
        let regions = finalize_dirty_regions(
            vec![
                DirtyRegion::new(10.0, 10.0, 30.0, 30.0),
                DirtyRegion::new(32.0, 10.0, 52.0, 30.0),
            ],
            surface(),
        );
        assert_eq!(regions, vec![DirtyRegion::new(10.0, 10.0, 52.0, 30.0)]);
    }

    #[test]
    fn large_sparse_sets_are_spatially_bounded() {
        let regions = (0..400)
            .map(|index| {
                let x = (index % 20) as f64 * 45.0;
                let y = (index / 20) as f64 * 45.0;
                DirtyRegion::new(x, y, x + 4.0, y + 4.0)
            })
            .collect();
        let finalized = finalize_dirty_regions(regions, surface());
        assert!(finalized.len() <= DIRTY_REGION_LIMIT);
        assert!(finalized.len() > 1);
    }

    #[test]
    fn pressure_sparse_edges_preserve_the_untouched_screen_interior() {
        let regions = finalize_sparse_dirty_regions(
            vec![
                DirtyRegion::new(0.0, 0.0, 1000.0, 12.0),
                DirtyRegion::new(0.0, 988.0, 1000.0, 1000.0),
                DirtyRegion::new(0.0, 0.0, 12.0, 1000.0),
                DirtyRegion::new(988.0, 0.0, 1000.0, 1000.0),
            ],
            surface(),
        );
        assert_eq!(regions.len(), 4);
        assert!(regions.iter().all(|region| !(region.min_x < 500.0
            && region.max_x > 500.0
            && region.min_y < 500.0
            && region.max_y > 500.0)));
        assert!(regions.iter().copied().map(dirty_region_area).sum::<f64>() < 50_000.0);
    }

    #[test]
    fn pressure_large_sparse_sets_are_bounded_without_joining_distant_sides() {
        let regions = (0..400)
            .map(|index| {
                let y = (index % 200) as f64 * 4.0;
                let x = if index < 200 { 5.0 } else { 990.0 };
                DirtyRegion::new(x, y, x + 4.0, y + 4.0)
            })
            .collect();
        let finalized = finalize_sparse_dirty_regions(regions, surface());
        assert!(finalized.len() <= DIRTY_REGION_LIMIT);
        assert!(
            finalized
                .iter()
                .all(|region| region.max_x <= 62.5 || region.min_x >= 937.5)
        );
        assert!(finalized.iter().any(|region| region.min_x <= 5.0));
        assert!(finalized.iter().any(|region| region.max_x >= 994.0));
    }
}
