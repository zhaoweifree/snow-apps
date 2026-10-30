//! Cursor geometry and pixels shared by current and compatibility capture adapters.
use crate::{ScreenRecorderError, error::Result};
use snow_macos::cursor::CursorShape;
use snow_recording_model::{CursorShapeCompositionMode, CursorShapeRecord};

pub(super) fn rasterize(
    shape: &CursorShape,
    scale: (f64, f64),
    id: u64,
) -> Result<CursorShapeRecord> {
    let width = (shape.point_width * scale.0).round();
    let height = (shape.point_height * scale.1).round();
    let hotspot_x = (shape.hotspot_x * scale.0).round();
    let hotspot_y = (shape.hotspot_y * scale.1).round();
    if !width.is_finite()
        || !height.is_finite()
        || !(1.0..=2048.0).contains(&width)
        || !(1.0..=2048.0).contains(&height)
        || !hotspot_x.is_finite()
        || !hotspot_y.is_finite()
        || hotspot_x < 0.0
        || hotspot_y < 0.0
        || hotspot_x > width
        || hotspot_y > height
        || shape.width == 0
        || shape.height == 0
        || u64::from(shape.width) * u64::from(shape.height) * 4 != shape.rgba.len() as u64
    {
        return Err(ScreenRecorderError::UnsupportedFeature(
            "editable cursor geometry is unsupported".into(),
        ));
    }
    let (width, height) = (width as u32, height as u32);
    let mut rgba = Vec::with_capacity(width as usize * height as usize * 4);
    for y in 0..height {
        for x in 0..width {
            let sx = u64::from(x) * u64::from(shape.width) / u64::from(width);
            let sy = u64::from(y) * u64::from(shape.height) / u64::from(height);
            let offset = (sy * u64::from(shape.width) + sx) as usize * 4;
            let pixel = &shape.rgba[offset..offset + 4];
            let alpha = u32::from(pixel[3]);
            for value in &pixel[..3] {
                rgba.push(
                    (u32::from(*value) * 255 + alpha / 2)
                        .checked_div(alpha)
                        .unwrap_or(0)
                        .min(255) as u8,
                );
            }
            rgba.push(pixel[3]);
        }
    }
    Ok(CursorShapeRecord {
        shape_id: id,
        width,
        height,
        hotspot_x: (hotspot_x as u32).min(width - 1),
        hotspot_y: (hotspot_y as u32).min(height - 1),
        mode: CursorShapeCompositionMode::AlphaBlend,
        shape_rgba: rgba,
    })
}
