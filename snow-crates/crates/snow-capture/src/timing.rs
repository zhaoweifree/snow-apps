//! Per-stage timing instrumentation for capture calls.
//!
//! This module is gated by the **`stage-timing` cargo feature** so that
//! production and release builds are completely unaffected: with the
//! feature disabled, every marker compiles to an inline no-op and no
//! timing API, fields, or clock reads exist anywhere in the crate.
//!
//! When the feature is enabled, a backend opens a [`StageScope`] at the
//! entry of a capture operation (if the session was configured with
//! `CaptureOptions::record_stage_timings`), and capture helpers record
//! stage boundaries through the marker functions in this module:
//!
//! - `stage_mark` closes the span since the previous mark (or scope entry)
//!   and is used for the additive, sequential stage breakdown.
//! - `stage_record` appends an externally measured span such as the time
//!   inside a single OS call, or a derived metric like OS frame age. These
//!   entries may overlap mark-based entries and are diagnostic detail
//!   rather than additive buckets.
//!
//! All markers are cheap no-ops while no scope is installed, so even
//! instrumented builds only pay for recording on sessions that opted in.

#[cfg(feature = "stage-timing")]
// Recording a scope is driven by the Windows capture backends today: the X11
// backend records checkpoints and spans but never opens a `StageScope`, so on
// Linux the recorder fields and methods have no caller. `--all-features` on
// Linux therefore has to tolerate them; the alternative -- deleting the code --
// would break every Windows caller.
#[cfg_attr(not(windows), allow(dead_code))]
mod active;
#[cfg(not(feature = "stage-timing"))]
mod inert;

#[cfg(feature = "stage-timing")]
pub use active::StageTiming;

#[cfg(feature = "stage-timing")]
pub(crate) use active::{stage_checkpoint, stage_record_since};

// QPC is a Windows clock, so these helpers only exist there; the markers that
// use them live in the DXGI/GDI backends. Re-exporting them unconditionally
// made `--all-features` fail to compile on Linux.
#[cfg(all(windows, feature = "stage-timing"))]
pub(crate) use active::{qpc_duration_between, qpc_ticks_to_duration};

// Backend-side instrumentation surface. Each platform backend opens scopes and
// records spans for the stages it can actually measure, so on any one target
// part of this set has no caller yet. `-D warnings` would otherwise reject a
// build for symbols that are deliberately part of the API.
#[cfg(feature = "stage-timing")]
#[allow(unused_imports)]
pub(crate) use active::{StageScope, attach_stage_timings, stage_record, stage_recording};

#[cfg(not(feature = "stage-timing"))]
pub(crate) use inert::{stage_checkpoint, stage_record_since};

#[cfg(all(windows, feature = "stage-timing"))]
pub(crate) use active::stage_mark;
#[cfg(all(windows, not(feature = "stage-timing")))]
pub(crate) use inert::stage_mark;
