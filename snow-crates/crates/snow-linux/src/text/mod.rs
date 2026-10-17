//! Text rasterization for the X11 keycap overlay.
//!
//! The overlay draws keycaps from a bitmap it is handed, so a platform adapter
//! only has to turn a legend into pixels. Windows and macOS delegate that to
//! their compositor; on Linux it is done here in pure Rust.
//!
//! The two halves are deliberately separate:
//!
//! * [`font`] finds the file behind a family name and reads its metrics. It
//!   enumerates installed fonts instead of calling fontconfig, so it stays
//!   free of C dependencies and is unit-tested against `test-support/fonts`.
//! * [`raster`] turns a legend into a premultiplied RGBA keycap, using
//!   `ab_glyph` for outlines and evaluating background coverage directly.

pub mod font;
pub mod raster;

pub use font::{
    DEFAULT_CJK_FAMILIES, DEFAULT_FAMILIES, FaceInfo, FaceSource, FontRequest, installed_faces,
    resolve, scan_faces, select_face,
};
pub use raster::{KeycapBitmap, KeycapRenderer, KeycapStyle};

/// Why a keycap could not be produced.
#[derive(Clone, Debug, PartialEq, Eq, thiserror::Error)]
pub enum TextError {
    /// The application named a family and no installed face matches it.
    ///
    /// Reported rather than substituted: drawing a different typeface would
    /// make the recording's legends disagree with the ones the application
    /// renders everywhere else.
    #[error("font family is not installed: {family}")]
    FontUnavailable { family: String },

    /// The selected file could not be read, which usually means it was removed
    /// between the enumeration and the load.
    #[error("font file could not be read: {path}")]
    FontUnreadable { path: String },

    /// The file is not a font this rasterizer can parse.
    #[error("font file is malformed: {path}")]
    MalformedFont { path: String },

    /// The request itself is outside the supported range.
    #[error("{0}")]
    InvalidConfig(&'static str),
}
