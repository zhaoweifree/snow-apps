//! X11 cursor backend.
//!
//! X11 has no "cursor visibility" flag reachable from core protocol, so
//! `visible` is derived from whether the pointer is still on this screen.
//! The shape comes from XFixes and is cached across transient failures for the
//! same reason as the Windows backend: the pointer moves far more often than it
//! changes shape, and dropping a valid bitmap because one `GetCursorImage`
//! round-trip lost a race would make the composited cursor flicker.

use snow_linux::cursor::X11CursorSampler;
use snow_linux::error::LinuxError;

use crate::CursorCaptureError;
use crate::CursorShapeCapture;
use crate::model::{CursorCompositionMode, CursorShape};
use crate::sampler::CursorProbe;

pub(crate) struct LinuxCursorSampler {
    inner: X11CursorSampler,
    last_shape: Option<CursorShape>,
}

impl LinuxCursorSampler {
    pub(crate) fn new() -> Result<Self, CursorCaptureError> {
        let inner = X11CursorSampler::new().map_err(map_error)?;
        Ok(Self {
            inner,
            last_shape: None,
        })
    }

    pub(crate) fn sample_cursor(&mut self) -> Result<CursorProbe, CursorCaptureError> {
        let sample = self.inner.sample().map_err(map_error)?;

        let shape = match sample.image {
            Some(image) => {
                let shape = CursorShape::from_rgba(
                    image.hotspot_x,
                    image.hotspot_y,
                    image.width,
                    image.height,
                    CursorCompositionMode::AlphaBlend,
                    image.rgba,
                );
                self.last_shape = Some(shape.clone());
                CursorShapeCapture::Captured(shape)
            }
            None => self
                .last_shape
                .clone()
                .map(CursorShapeCapture::Captured)
                .unwrap_or(CursorShapeCapture::Unavailable),
        };

        Ok(CursorProbe {
            x: sample.x,
            y: sample.y,
            visible: sample.on_screen,
            shape,
        })
    }
}

/// A host with no reachable X session is "feature unavailable", not "broken":
/// retrying in-process will never make `$DISPLAY` appear.
fn map_error(error: LinuxError) -> CursorCaptureError {
    match error {
        LinuxError::SessionMissing => CursorCaptureError::UnsupportedPlatform,
        other => CursorCaptureError::platform(other.to_string()),
    }
}
