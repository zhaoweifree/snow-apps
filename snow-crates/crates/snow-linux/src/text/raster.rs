//! Offscreen keycap rasterization.
//!
//! The Windows and macOS adapters hand the whole job to their compositor
//! (`Direct2D` + `DirectWrite`, Quartz + CoreText). This module does the same
//! work with library-free arithmetic: glyph outlines come from `ab_glyph`
//! (pure Rust, no fontconfig and no FreeType), and the rounded background is
//! filled by evaluating coverage directly.
//!
//! The output contract matches the other adapters, because the overlay
//! composites the result without knowing which platform produced it:
//!
//! * the bitmap is `ceil(width * scale)` by `round(HEIGHT * scale)`, where
//!   `width` comes from [`snow_core::keycap_layout::fit`];
//! * pixels are premultiplied RGBA;
//! * the background is a rounded rectangle inset by `scale`, drawn over a
//!   border of the same shape;
//! * text is centred both ways on the same raster origin that the measured
//!   advance implies, so every legend in the stack shares one baseline.

use ab_glyph::{Font as _, FontVec, Glyph, PxScale, ScaleFont as _, point};

use super::TextError;
use super::font::{FaceInfo, FaceSource, FontRequest};

/// Colours the overlay asks for, in straight-alpha RGBA.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct KeycapStyle {
    pub background_rgba: [u8; 4],
    pub text_rgba: [u8; 4],
    pub border_rgba: [u8; 4],
}

/// A rasterized keycap: premultiplied RGBA, rounded background included.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct KeycapBitmap {
    pub width: u32,
    pub height: u32,
    pub pixels: Vec<u8>,
}

/// Rounded-rectangle corner radius at unit scale, matching the macOS adapter.
const CORNER_RADIUS: f32 = 10.0;
/// Coverage samples per axis. Sixteen sub-samples per pixel is what makes a
/// rounded corner and a glyph stem look equally smooth without an analytic
/// distance field.
const SUBSAMPLES: u32 = 4;
/// Upper bound on a keycap legend, mirroring the macOS adapter's guard.
const MAX_LABEL_CHARS: usize = 1024;
const MAX_BITMAP_WIDTH: f32 = 4096.0;

struct FontStack {
    primary: FontVec,
    cjk: Option<FontVec>,
}

impl FontStack {
    /// Index of the face that should draw `character`.
    ///
    /// `glyph_id` returns 0 for a character the face has no outline for, which
    /// is exactly the `.notdef` condition the other adapters hand to their
    /// font-fallback cascade.
    fn index_for(&self, character: char) -> usize {
        if self.primary.glyph_id(character).0 != 0 {
            return 0;
        }
        if let Some(cjk) = &self.cjk {
            if cjk.glyph_id(character).0 != 0 {
                return 1;
            }
        }
        // Nothing can draw it. The primary face keeps the layout deterministic
        // by advancing the notdef width the same way the OS adapters do.
        0
    }

    fn face(&self, index: usize) -> &FontVec {
        match index {
            1 => self.cjk.as_ref().expect("cjk face index"),
            _ => &self.primary,
        }
    }

    /// Advance width of `label` at the layout font size.
    ///
    /// Kerning is deliberately skipped. The two faces in the stack can differ,
    /// and a pair kern across faces has no meaning; within one face the effect
    /// on a keycap legend is under a pixel, while consulting it would need the
    /// previous glyph's face as well as its id.
    fn measure(&self, label: &str, font_size: f32) -> f32 {
        let mut width = 0.0;
        for character in label.chars() {
            let face = self.face(self.index_for(character));
            let scaled = face.as_scaled(PxScale::from(font_size));
            width += scaled.h_advance(face.glyph_id(character));
        }
        width
    }
}

/// Renderer for one font selection and colour set.
pub struct KeycapRenderer {
    stack: FontStack,
    style: KeycapStyle,
}

impl KeycapRenderer {
    /// Load a selection from an explicit face list.
    ///
    /// Taking the face list rather than consulting the filesystem is what lets
    /// the rendering path be tested on a host that is not running Linux.
    pub fn from_faces(
        faces: &[FaceInfo],
        request: &FontRequest<'_>,
        style: KeycapStyle,
    ) -> Result<Self, TextError> {
        let (primary, cjk) = super::font::resolve(request, faces).ok_or_else(|| {
            TextError::FontUnavailable {
                family: if request.family.trim().is_empty() {
                    "<default>".into()
                } else {
                    request.family.to_string()
                },
            }
        })?;
        Self::load_sources(primary, cjk, style)
    }

    /// Load a selection from the faces installed on this machine.
    pub fn load(request: &FontRequest<'_>, style: KeycapStyle) -> Result<Self, TextError> {
        Self::from_faces(super::font::installed_faces(), request, style)
    }

    fn load_sources(
        primary: FaceSource,
        cjk: Option<FaceSource>,
        style: KeycapStyle,
    ) -> Result<Self, TextError> {
        let primary = read_face(&primary)?;
        let cjk = match cjk {
            Some(source) => Some(read_face(&source)?),
            None => None,
        };
        let mut renderer = Self {
            stack: FontStack { primary, cjk },
            style,
        };
        // Prove the whole path before the recorder reports that it started,
        // exactly as the other adapters do with their probe glyph.
        renderer.render("M", 1.0)?;
        Ok(renderer)
    }

    /// Rasterize `label` at `scale`, relative to the 64-pixel keycap height.
    pub fn render(&mut self, label: &str, scale: f32) -> Result<KeycapBitmap, TextError> {
        if !scale.is_finite() || !(0.25..=4.0).contains(&scale) {
            return Err(TextError::InvalidConfig("keycap scale out of range"));
        }
        if label.chars().count() > MAX_LABEL_CHARS {
            return Err(TextError::InvalidConfig("keycap label is too long"));
        }

        let measured = self.stack.measure(label, snow_core::keycap_layout::FONT_SIZE);
        let (logical_width, font_size) = snow_core::keycap_layout::fit(measured);
        let width = (logical_width as f32 * scale).ceil().clamp(1.0, MAX_BITMAP_WIDTH) as u32;
        let height = (snow_core::keycap_layout::HEIGHT as f32 * scale).round() as u32;
        let size = font_size * scale;

        let mut bitmap = KeycapBitmap {
            width,
            height,
            pixels: vec![0; width as usize * height as usize * 4],
        };

        // Border first, then the background on top of it inset by one scale
        // unit: the ring the background does not cover is the border. This is
        // the same construction the macOS adapter uses.
        fill_rounded_rect(
            &mut bitmap,
            CORNER_RADIUS * scale,
            0.0,
            self.style.border_rgba,
        );
        fill_rounded_rect(
            &mut bitmap,
            CORNER_RADIUS * scale,
            scale,
            self.style.background_rgba,
        );

        self.draw_label(&mut bitmap, label, size, measured, scale);
        Ok(bitmap)
    }

    fn draw_label(
        &self,
        bitmap: &mut KeycapBitmap,
        label: &str,
        size: f32,
        measured: f32,
        scale: f32,
    ) {
        let primary = self.stack.face(0);
        let scaled = primary.as_scaled(PxScale::from(size));
        let ascent = scaled.ascent();
        // `descent` is negative in font units, so it is flipped to a height.
        let descent = -scaled.descent();
        let baseline = (bitmap.height as f32 - (ascent + descent)) / 2.0 + ascent;
        let mut pen = (bitmap.width as f32 - measured * scale) / 2.0;

        for character in label.chars() {
            let index = self.stack.index_for(character);
            let face = self.stack.face(index);
            let id = face.glyph_id(character);
            let scaled = face.as_scaled(PxScale::from(size));
            let glyph = Glyph {
                id,
                scale: PxScale::from(size),
                position: point(pen, baseline),
            };
            if let Some(outlined) = face.outline_glyph(glyph) {
                let bounds = outlined.px_bounds();
                outlined.draw(|x, y, coverage| {
                    let px = i64::from(bounds.min.x as i32) + i64::from(x);
                    let py = i64::from(bounds.min.y as i32) + i64::from(y);
                    blend(bitmap, px, py, self.style.text_rgba, coverage);
                });
            }
            pen += scaled.h_advance(id);
        }
    }
}

fn read_face(source: &FaceSource) -> Result<FontVec, TextError> {
    let data = std::fs::read(&source.path).map_err(|_| TextError::FontUnreadable {
        path: source.path.display().to_string(),
    })?;
    FontVec::try_from_vec_and_index(data, source.index).map_err(|_| TextError::MalformedFont {
        path: source.path.display().to_string(),
    })
}

/// Fill a rounded rectangle of `radius` corners, inset by `inset` pixels.
///
/// Coverage is sampled on a `SUBSAMPLES x SUBSAMPLES` grid per pixel, which is
/// what keeps a corner and a diagonal border edge from looking like a staircase.
fn fill_rounded_rect(bitmap: &mut KeycapBitmap, radius: f32, inset: f32, color: [u8; 4]) {
    if bitmap.width == 0 || bitmap.height == 0 || color[3] == 0 {
        return;
    }
    let left = inset;
    let top = inset;
    let right = bitmap.width as f32 - inset;
    let bottom = bitmap.height as f32 - inset;
    if right <= left || bottom <= top {
        return;
    }
    // A radius larger than the shape's half-extent degenerates; clamping keeps
    // the corner geometry valid for very small keycaps.
    let radius = radius
        .min((right - left) / 2.0)
        .min((bottom - top) / 2.0)
        .max(0.0);

    let step = 1.0 / SUBSAMPLES as f32;
    let offset = step / 2.0;
    for y in 0..bitmap.height {
        for x in 0..bitmap.width {
            let mut covered = 0u32;
            for sy in 0..SUBSAMPLES {
                for sx in 0..SUBSAMPLES {
                    let point_x = x as f32 + offset + sx as f32 * step;
                    let point_y = y as f32 + offset + sy as f32 * step;
                    if inside_rounded_rect(point_x, point_y, left, top, right, bottom, radius) {
                        covered += 1;
                    }
                }
            }
            if covered == 0 {
                continue;
            }
            let coverage = covered as f32 / (SUBSAMPLES * SUBSAMPLES) as f32;
            blend(bitmap, i64::from(x), i64::from(y), color, coverage);
        }
    }
}

fn inside_rounded_rect(
    x: f32,
    y: f32,
    left: f32,
    top: f32,
    right: f32,
    bottom: f32,
    radius: f32,
) -> bool {
    // Fold the point back into the rectangle that remains after the corners are
    // cut away; the distance to that folded point is the distance to the
    // rounded outline, so a comparison against the radius decides membership.
    let nearest_x = x.clamp(left + radius, right - radius);
    let nearest_y = y.clamp(top + radius, bottom - radius);
    let dx = x - nearest_x;
    let dy = y - nearest_y;
    dx * dx + dy * dy <= radius * radius
}

/// Source-over composite of `color` at `coverage` into a premultiplied buffer.
///
/// Both operands are premultiplied, which is the form the overlay's compositor
/// expects, so the blend is `src + dst * (1 - src_alpha)` on every channel.
fn blend(bitmap: &mut KeycapBitmap, x: i64, y: i64, color: [u8; 4], coverage: f32) {
    if coverage <= 0.0
        || x < 0
        || y < 0
        || x >= i64::from(bitmap.width)
        || y >= i64::from(bitmap.height)
    {
        return;
    }
    let source_alpha = f32::from(color[3]) / 255.0 * coverage.clamp(0.0, 1.0);
    if source_alpha <= 0.0 {
        return;
    }
    let inverse = 1.0 - source_alpha;
    let index = (y as usize * bitmap.width as usize + x as usize) * 4;
    let pixel = &mut bitmap.pixels[index..index + 4];
    for channel in 0..3 {
        let source = f32::from(color[channel]) * source_alpha;
        pixel[channel] = (source + f32::from(pixel[channel]) * inverse)
            .round()
            .clamp(0.0, 255.0) as u8;
    }
    let alpha = source_alpha * 255.0;
    pixel[3] = (alpha + f32::from(pixel[3]) * inverse)
        .round()
        .clamp(0.0, 255.0) as u8;
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::text::font::scan_faces;
    use std::path::Path;

    const SANS: &str = "Snow Recording Test Sans";
    const HAN: &str = "Snow Recording Test Han";
    const TEXT: [u8; 3] = [255, 255, 255];
    const BORDER: [u8; 3] = [255, 0, 0];

    fn fixture_faces() -> Vec<FaceInfo> {
        scan_faces(&[Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../test-support/fonts")])
    }

    fn renderer(family: &str, cjk: &str, weight: u32) -> KeycapRenderer {
        let request = FontRequest {
            family,
            cjk_family: cjk,
            weight,
        };
        KeycapRenderer::from_faces(
            &fixture_faces(),
            &request,
            KeycapStyle {
                background_rgba: [0, 0, 0, 255],
                text_rgba: [255, 255, 255, 255],
                border_rgba: [255, 0, 0, 255],
            },
        )
        .unwrap()
    }

    fn alpha_at(bitmap: &KeycapBitmap, x: u32, y: u32) -> u8 {
        bitmap.pixels[(y as usize * bitmap.width as usize + x as usize) * 4 + 3]
    }

    fn rgb_at(bitmap: &KeycapBitmap, x: u32, y: u32) -> [u8; 3] {
        let index = (y as usize * bitmap.width as usize + x as usize) * 4;
        [
            bitmap.pixels[index],
            bitmap.pixels[index + 1],
            bitmap.pixels[index + 2],
        ]
    }

    fn count_opaque(bitmap: &KeycapBitmap, color: [u8; 3]) -> usize {
        bitmap
            .pixels
            .chunks_exact(4)
            .filter(|pixel| pixel[3] > 128 && pixel[..3] == color)
            .count()
    }

    #[test]
    fn geometry_matches_the_shared_layout_contract() {
        let mut rasterizer = renderer(SANS, HAN, 400);
        let base = rasterizer.render("M", 1.0).unwrap();
        assert_eq!(base.height, snow_core::keycap_layout::HEIGHT);
        // The width comes from `keycap_layout::fit`: never narrower than the
        // padding it reserves, never wider than the square default.
        assert!((40..=64).contains(&base.width), "{}", base.width);
        assert_eq!(base.pixels.len(), (base.width * base.height * 4) as usize);

        // Scale multiplies the logical width before it is rounded up, and that
        // width is already integral, so doubling the scale doubles the bitmap.
        let doubled = rasterizer.render("M", 2.0).unwrap();
        assert_eq!(doubled.height, 128);
        assert_eq!(doubled.width, base.width * 2);
    }

    #[test]
    fn long_legends_widen_the_keycap_without_growing_it() {
        let mut rasterizer = renderer(SANS, HAN, 400);
        let short = rasterizer.render("M", 1.0).unwrap();
        let long = rasterizer.render("Ctrl", 1.0).unwrap();
        assert!(long.width > short.width);
        assert_eq!(long.height, short.height);
    }

    #[test]
    fn scale_outside_the_supported_range_is_rejected() {
        let mut rasterizer = renderer(SANS, HAN, 400);
        for scale in [0.0, 0.1, 4.5, f32::NAN, f32::INFINITY, -1.0] {
            assert!(rasterizer.render("M", scale).is_err(), "scale {scale}");
        }
    }

    #[test]
    fn an_overlong_label_is_rejected() {
        let mut rasterizer = renderer(SANS, HAN, 400);
        assert!(
            rasterizer
                .render(&"a".repeat(MAX_LABEL_CHARS + 1), 1.0)
                .is_err()
        );
    }

    #[test]
    fn an_empty_label_renders_the_bare_keycap() {
        // The overlay requests the empty legend for a lone modifier; it must
        // produce the rounded background rather than fail. `fit` clamps the
        // measured advance to one pixel, so the result is the minimum keycap.
        let mut rasterizer = renderer(SANS, HAN, 400);
        let cap = rasterizer.render("", 1.0).unwrap();
        assert_eq!(cap.width, 41);
        assert_eq!(cap.height, snow_core::keycap_layout::HEIGHT);
        assert!(count_opaque(&cap, [0, 0, 0]) > 0);
    }

    #[test]
    fn corners_are_rounded_and_the_discarded_area_is_transparent() {
        let mut rasterizer = renderer(SANS, HAN, 400);
        let cap = rasterizer.render("M", 1.0).unwrap();
        // The very corner lies outside the rounded outline.
        assert_eq!(alpha_at(&cap, 0, 0), 0);
        assert_eq!(alpha_at(&cap, cap.width - 1, 0), 0);
        assert_eq!(alpha_at(&cap, 0, cap.height - 1), 0);
        // The centre of an edge is inside the shape.
        assert_eq!(alpha_at(&cap, cap.width / 2, 0), 255);
        assert_eq!(alpha_at(&cap, cap.width / 2, cap.height / 2), 255);
    }

    #[test]
    fn the_border_is_visible_around_the_background() {
        let mut rasterizer = renderer(SANS, HAN, 400);
        let cap = rasterizer.render("M", 1.0).unwrap();
        assert!(count_opaque(&cap, BORDER) > 0, "border colour missing");
        // Straight across the middle, the outermost pixel is border and the
        // second one in is already background: the background is inset by one
        // scale unit.
        let mid = cap.height / 2;
        assert_eq!(rgb_at(&cap, 0, mid), BORDER);
        assert_eq!(rgb_at(&cap, 2, mid), [0, 0, 0]);
    }

    #[test]
    fn glyphs_are_painted_in_the_text_colour() {
        let mut rasterizer = renderer(SANS, HAN, 400);
        let cap = rasterizer.render("M", 1.0).unwrap();
        assert!(count_opaque(&cap, TEXT) > 0, "no glyph pixels found");
    }

    #[test]
    fn a_han_legend_falls_back_to_the_cjk_face() {
        // The Latin fixtures deliberately carry no Han outlines, so a wider
        // keycap can only come from having measured the fallback face.
        let mut rasterizer = renderer(SANS, HAN, 400);
        let han = rasterizer.render("汉", 1.0).unwrap();
        assert!(count_opaque(&han, TEXT) > 0);
        // The Han fixture gives every non-ASCII glyph a wider advance than the
        // Latin fixture gives `M`, which is what makes the two measurable.
        let latin = rasterizer.render("M", 1.0).unwrap();
        assert!(han.width > latin.width, "han {} latin {}", han.width, latin.width);
    }

    #[test]
    fn a_han_legend_without_a_fallback_face_still_produces_a_keycap() {
        // On a desktop with no CJK font the keycap still has to exist: the
        // overlay must not lose the whole frame over one unrenderable legend.
        // The character falls back to the primary face's `.notdef` box.
        let mut rasterizer = renderer(SANS, "", 400);
        let cap = rasterizer.render("汉", 1.0).unwrap();
        assert!((40..=64).contains(&cap.width), "{}", cap.width);
        assert_eq!(cap.height, snow_core::keycap_layout::HEIGHT);
        assert!(count_opaque(&cap, [0, 0, 0]) > 0);
    }

    #[test]
    fn bold_faces_draw_more_ink_than_regular_ones() {
        // The fixtures give bold a visibly thicker stroke, so this distinguishes
        // loading the bold file from silently reusing the regular one.
        let mut regular = renderer(SANS, HAN, 400);
        let mut bold = renderer(SANS, HAN, 700);
        let regular_ink = count_opaque(&regular.render("M", 1.0).unwrap(), TEXT);
        let bold_ink = count_opaque(&bold.render("M", 1.0).unwrap(), TEXT);
        assert!(bold_ink > regular_ink, "regular {regular_ink} bold {bold_ink}");
    }

    #[test]
    fn rendering_is_deterministic() {
        let mut rasterizer = renderer(SANS, HAN, 400);
        let first = rasterizer.render("Ctrl", 1.0).unwrap();
        let second = rasterizer.render("Ctrl", 1.0).unwrap();
        assert_eq!(first, second);
    }

    #[test]
    fn a_transparent_style_stays_transparent() {
        // Colour channels must be premultiplied: a fully transparent style
        // cannot leave any ink behind.
        let request = FontRequest {
            family: SANS,
            cjk_family: HAN,
            weight: 400,
        };
        let mut rasterizer = KeycapRenderer::from_faces(
            &fixture_faces(),
            &request,
            KeycapStyle {
                background_rgba: [0, 0, 0, 0],
                text_rgba: [0, 0, 0, 0],
                border_rgba: [0, 0, 0, 0],
            },
        )
        .unwrap();
        let cap = rasterizer.render("M", 1.0).unwrap();
        assert!(cap.pixels.iter().all(|value| *value == 0));
    }

    #[test]
    fn an_unknown_family_is_reported_rather_than_substituted() {
        let request = FontRequest {
            family: "Noto Sans CJK SC",
            cjk_family: "",
            weight: 400,
        };
        let error = KeycapRenderer::from_faces(
            &fixture_faces(),
            &request,
            KeycapStyle {
                background_rgba: [0, 0, 0, 255],
                text_rgba: [255, 255, 255, 255],
                border_rgba: [0, 0, 0, 255],
            },
        )
        .err()
        .expect("an uninstalled family must not resolve");
        assert!(matches!(error, TextError::FontUnavailable { .. }), "{error}");
    }

    #[test]
    fn every_channel_of_a_coloured_keycap_is_premultiplied() {
        // A colour channel may never exceed its alpha, which is the invariant
        // the overlay's compositor relies on.
        let mut rasterizer = renderer(SANS, HAN, 400);
        let cap = rasterizer.render("Ctrl", 2.0).unwrap();
        for pixel in cap.pixels.chunks_exact(4) {
            for channel in &pixel[..3] {
                assert!(*channel <= pixel[3], "{pixel:?}");
            }
        }
    }
}
