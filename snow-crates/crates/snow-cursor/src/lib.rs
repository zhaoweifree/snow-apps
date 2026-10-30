//! Cursor domain types, synchronous sampling, and streaming.
//!
//! This crate exposes three layers:
//! - [`CursorSampler`] for raw, absolute cursor snapshots.
//! - [`CursorProjector`] for converting snapshots into target-relative samples.
//! - [`CursorStreamHandle`] for consuming raw snapshots from a worker thread.
//!
//! Windows (Win32) and Linux (X11) are supported. Other platforms return
//! [`CursorCaptureError::UnsupportedPlatform`].

mod crosshair;
mod error;
mod model;
mod platform;
mod projector;
mod sampler;
mod stream;

pub use crosshair::{WindowsCrosshairNormalizer, crosshair_cursor_shape};
pub use error::CursorCaptureError;
pub use model::{
    AttachedCursorSample, CursorCompositionMode, CursorShape, CursorShapeCapture, CursorShapeId,
    CursorShapeState, CursorSnapshot, CursorTargetInfo,
};
pub use projector::CursorProjector;
pub use sampler::CursorSampler;
pub use stream::{
    CursorStreamConfig, CursorStreamEvent, CursorStreamHandle, CursorStreamStats,
    CursorStreamStatsSnapshot,
};
