pub mod bench_timing;
pub mod config;
pub mod deferred;
pub mod direct;
pub mod error;
#[cfg(feature = "legacy-capture")]
pub mod recording;

pub(crate) mod adapter;
mod capture_policy;
pub(crate) mod keyboard_hook;
pub(crate) mod keyboard_overlay;
pub(crate) mod keyboard_rasterizer;
pub(crate) mod mouse_hook;
mod output_schedule;
mod worker_startup;
pub use keyboard_overlay::{KeyboardOverlayConfig, KeyboardOverlayFont};
#[cfg(feature = "legacy-capture")]
pub(crate) mod processor;
#[cfg(feature = "legacy-capture")]
pub(crate) mod temp;

pub use config::{
    AudioChannels, CaptureBackendKind, MonitorSelector, RecordingAudioConfig,
    RecordingAudioTrackConfig, RecordingAudioTrackSource, RecordingConfig, RecordingRegion,
    RecordingTarget, WindowId,
};
pub use deferred::{
    DeferredRecordingOptions, DeferredRecordingSession, DeferredRecordingSource,
    DeferredRenderProgress, DeferredRenderState, DeferredRenderTask,
};
pub use direct::{
    DirectRecordingConfig, DirectRecordingReport, DirectRecordingSession, RecordingAudioMode,
};
pub use error::{MediaPermission, ScreenRecorderError};
#[cfg(feature = "legacy-capture")]
pub use recording::RecordingSession;
mod state;
pub use state::RecordingState;

#[cfg(target_os = "macos")]
pub mod macos;

#[cfg(target_os = "macos")]
mod macos_effects;
