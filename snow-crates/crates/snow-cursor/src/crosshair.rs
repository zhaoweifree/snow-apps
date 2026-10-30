use crate::{CursorCompositionMode, CursorShape, platform};

const BASE_SIZE: u32 = 32;
const BASE_HOTSPOT: u32 = 16;

/// Snow Shot's crosshair, scaled from its 32-pixel logical image by nearest neighbor.
///
/// The white, one-pixel-wide plus extends from 6 through 26 on both axes. Its
/// one-pixel black outline and transparent surroundings never invert screen pixels.
pub fn crosshair_cursor_shape(width: u32, height: u32) -> Option<CursorShape> {
    if width == 0 || height == 0 || width > 2048 || height > 2048 {
        return None;
    }
    let mut rgba = vec![0; width as usize * height as usize * 4];
    for y in 0..height {
        let source_y = y * BASE_SIZE / height;
        for x in 0..width {
            let source_x = x * BASE_SIZE / width;
            let white = is_white_pixel(source_x, source_y);
            let outlined = (source_x.abs_diff(BASE_HOTSPOT) <= 1 && (5..=27).contains(&source_y))
                || (source_y.abs_diff(BASE_HOTSPOT) <= 1 && (5..=27).contains(&source_x));
            let offset = (y as usize * width as usize + x as usize) * 4;
            if white {
                rgba[offset..offset + 4].copy_from_slice(&[255; 4]);
            } else if outlined {
                rgba[offset + 3] = 255;
            }
        }
    }
    Some(CursorShape::from_rgba(
        (BASE_HOTSPOT * width).div_ceil(BASE_SIZE).min(width - 1),
        (BASE_HOTSPOT * height).div_ceil(BASE_SIZE).min(height - 1),
        width,
        height,
        CursorCompositionMode::AlphaBlend,
        rgba,
    ))
}

fn is_white_pixel(x: u32, y: u32) -> bool {
    (x == BASE_HOTSPOT && (6..=26).contains(&y)) || (y == BASE_HOTSPOT && (6..=26).contains(&x))
}

/// Replaces only Windows' stock crosshair shape with Snow Shot's alpha crosshair.
///
/// Backends without native cursor handles, such as Desktop Duplication, can match
/// their decoded shape against the system resource at the same physical size.
/// Repeated observations reuse the last match and replacement. Other platforms
/// preserve the supplied shape.
#[derive(Default)]
pub struct WindowsCrosshairNormalizer {
    reference: Option<CursorShape>,
    last_native: Option<CursorShape>,
    last_normalized: Option<CursorShape>,
}

impl WindowsCrosshairNormalizer {
    pub fn new() -> Self {
        Self::default()
    }

    /// Supplies a fixed stock reference for deterministic backend tests.
    #[doc(hidden)]
    pub fn with_system_shape(system_shape: CursorShape) -> Self {
        Self {
            reference: Some(system_shape),
            ..Self::default()
        }
    }

    /// Preserves the observed dimensions while replacing matched crosshair pixels.
    pub fn normalize_shape(&mut self, shape: &CursorShape) -> CursorShape {
        if self
            .last_native
            .as_ref()
            .is_some_and(|last| last.shape_id == shape.shape_id)
        {
            return self.last_normalized.as_ref().unwrap().clone();
        }
        let system_shape = self
            .reference
            .clone()
            .or_else(|| platform::system_crosshair_shape(shape.width, shape.height));
        let normalized = if system_shape.as_ref() == Some(shape) {
            crosshair_cursor_shape(shape.width, shape.height).unwrap_or_else(|| shape.clone())
        } else {
            shape.clone()
        };
        self.last_native = Some(shape.clone());
        self.last_normalized = Some(normalized.clone());
        normalized
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn pixel(shape: &CursorShape, x: u32, y: u32) -> &[u8] {
        let offset = (y * shape.width + x) as usize * 4;
        &shape.shape_rgba[offset..offset + 4]
    }

    #[test]
    fn base_crosshair_has_white_fill_black_perimeter_and_transparent_surroundings() {
        let shape = crosshair_cursor_shape(32, 32).unwrap();
        assert_eq!((shape.hotspot_x, shape.hotspot_y), (16, 16));
        assert_eq!(shape.composition_mode, CursorCompositionMode::AlphaBlend);
        for y in 0..32 {
            for x in 0..32 {
                let white =
                    (x == 16 && (6..=26).contains(&y)) || (y == 16 && (6..=26).contains(&x));
                let perimeter = (-1..=1).any(|dy| {
                    (-1..=1).any(|dx| {
                        let nearby_x = x as i32 + dx;
                        let nearby_y = y as i32 + dy;
                        (nearby_x == 16 && (6..=26).contains(&nearby_y))
                            || (nearby_y == 16 && (6..=26).contains(&nearby_x))
                    })
                });
                let expected = if white {
                    [255; 4]
                } else if perimeter {
                    [0, 0, 0, 255]
                } else {
                    [0; 4]
                };
                assert_eq!(pixel(&shape, x, y), expected, "pixel ({x}, {y})");
            }
        }
    }

    #[test]
    fn scaling_preserves_pixels_hotspot_and_stable_shape_identity() {
        let base = crosshair_cursor_shape(32, 32).unwrap();
        for (width, height) in [(32, 32), (40, 40), (48, 48), (64, 64), (48, 64)] {
            let scaled = crosshair_cursor_shape(width, height).unwrap();
            assert_eq!(
                (scaled.hotspot_x, scaled.hotspot_y),
                (width / 2, height / 2)
            );
            for y in 0..height {
                for x in 0..width {
                    assert_eq!(
                        pixel(&scaled, x, y),
                        pixel(&base, x * 32 / width, y * 32 / height)
                    );
                }
            }
            assert_eq!(
                scaled.shape_id,
                crosshair_cursor_shape(width, height).unwrap().shape_id
            );
        }
        for (width, height) in [(0, 32), (32, 0), (2049, 32), (32, u32::MAX)] {
            assert!(crosshair_cursor_shape(width, height).is_none());
        }
    }

    #[test]
    fn odd_sized_scaling_keeps_the_hotspot_on_the_white_center() {
        let physical_size = (32_f64 * 1.1).round() as u32;
        assert_eq!(physical_size, 35);
        let shape = crosshair_cursor_shape(physical_size, physical_size).unwrap();
        assert_eq!((shape.hotspot_x, shape.hotspot_y), (18, 18));
        assert_eq!(pixel(&shape, shape.hotspot_x, shape.hotspot_y), [255; 4]);
        assert_eq!(pixel(&shape, 17, 17), [0, 0, 0, 255]);
        let rectangular = crosshair_cursor_shape(35, 48).unwrap();
        assert_eq!((rectangular.hotspot_x, rectangular.hotspot_y), (18, 24));
        assert_eq!(pixel(&rectangular, 18, 24), [255; 4]);
    }

    #[test]
    fn matcher_replaces_exact_stock_shape_and_preserves_near_matches() {
        let stock = CursorShape::from_rgba(
            16,
            16,
            32,
            32,
            CursorCompositionMode::MaskedColor,
            vec![255; 4096],
        );
        let mut matcher = WindowsCrosshairNormalizer::with_system_shape(stock.clone());
        let expected = crosshair_cursor_shape(32, 32).unwrap();
        assert_eq!(matcher.normalize_shape(&stock), expected);
        assert_eq!(matcher.normalize_shape(&stock), expected);

        let custom = CursorShape::from_rgba(
            15,
            16,
            32,
            32,
            CursorCompositionMode::MaskedColor,
            vec![255; 4096],
        );
        assert_eq!(matcher.normalize_shape(&custom), custom);
        let custom = CursorShape::from_rgba(
            16,
            16,
            32,
            32,
            CursorCompositionMode::AlphaBlend,
            vec![255; 4096],
        );
        assert_eq!(matcher.normalize_shape(&custom), custom);
        let mut changed_pixels = vec![255; 4096];
        changed_pixels[4] = 0;
        let custom = CursorShape::from_rgba(
            16,
            16,
            32,
            32,
            CursorCompositionMode::MaskedColor,
            changed_pixels,
        );
        assert_eq!(matcher.normalize_shape(&custom), custom);
        assert_eq!(matcher.normalize_shape(&stock), expected);
    }
}
