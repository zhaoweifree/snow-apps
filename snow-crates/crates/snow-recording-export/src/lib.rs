#![allow(clippy::too_many_arguments)]

pub(crate) mod codec;
pub mod config;
pub mod deferred;
pub mod editing;
pub mod error;
pub mod export;
#[cfg(windows)]
pub mod gpu;
pub mod resize;
pub mod streaming;

#[cfg(feature = "bench-timing")]
pub mod bench_timing;

mod animation;
#[doc(hidden)]
pub mod ffmpeg_util;
#[cfg(any(test, feature = "bench-experiments"))]
mod frame_converter;
mod gif_encoder;
mod gif_palette;
mod output_scaler;
pub(crate) use snow_recording_model::video_quality;

pub use config::{
    ExportAudioOutputConfig, ExportAudioTrackRequest, ExportExecutionMode, ExportFormat,
    ExportPerformanceConfig, ExportRequest, MouseEditConfig, SoftwareH264Priority, VideoCodec,
};
pub use deferred::{
    DeferredRenderProgress, DeferredRenderState, DeferredRenderTask, read_deferred_output_settings,
    write_deferred_output_settings,
};
pub use editing::EditingSession;
pub use error::RecordingExportError;
pub use export::{
    ExportPathKind, ExportProgress, ExportResult, ExportRuntimeReport, ExportStage,
    ExportStageDurationsMs, ExportTask,
};
pub use hdr::preserves_hdr_output;
pub use snow_recording_model::PlaybackSpeed;
pub use streaming::{
    StreamingAudioConfig, StreamingEncoder, StreamingEncoderBuilder, StreamingEncoderConfig,
    StreamingEncoderReport, VideoFrameReuse, cleanup_stale_staging_files, scaled_output_dimensions,
};

#[cfg(target_os = "macos")]
mod videotoolbox;

mod hdr;

pub mod clip;
pub(crate) mod decoder;
pub mod playback;
pub(crate) mod tempo;

#[cfg(test)]
pub(crate) fn has_avi_compatibility() -> bool {
    // The standalone compatibility FFmpeg profile adds AVI; the app profile omits it.
    unsafe {
        !ffmpeg_next::ffi::av_guess_format(c"avi".as_ptr(), std::ptr::null(), std::ptr::null())
            .is_null()
    }
}
