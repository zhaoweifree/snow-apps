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

/// First path segment of every stage name a capture backend records for itself.
///
/// Stage names are `family.stage`, where the first segment says who measured
/// it. One backend per family: the four below are the only ones that record
/// anything today, so a name outside this list and
/// [`AGNOSTIC_STAGE_PREFIXES`] is either a typo or a new family that has to be
/// declared before it can be recorded.
pub const BACKEND_STAGE_PREFIXES: &[&str] = &["dxgi.", "wgc.", "gdi.", "x11."];

/// Stage families recorded outside any single backend.
///
/// `readback.` is the shared Windows readback layer; `buffer.` is the frame
/// buffer allocator in [`crate::frame`].
///
/// **Neither family is additive with a backend's own spans.** GDI measures
/// `readback.frame_alloc` around a call that itself records `buffer.allocate`,
/// so summing the two counts the same microseconds twice. A consumer that wants
/// an additive breakdown has to sum the backend's primary stages, which is what
/// `tests/screenshot_stage_timing_smoke.rs` does.
///
/// Names beginning `detail.` exist only in this module's unit tests, which is
/// why the drift check below skips this directory.
pub const AGNOSTIC_STAGE_PREFIXES: &[&str] = &["readback.", "buffer."];

/// Whether `name` belongs to a declared stage family.
///
/// Used by the capture backends' smoke tests and by the drift check in this
/// module. Membership says who measured a stage, never whether it can be added
/// to another one -- see [`AGNOSTIC_STAGE_PREFIXES`].
pub fn is_known_stage(name: &str) -> bool {
    BACKEND_STAGE_PREFIXES
        .iter()
        .chain(AGNOSTIC_STAGE_PREFIXES)
        .any(|prefix| name.starts_with(prefix))
}

#[cfg(feature = "stage-timing")]
// Every backend opens a `StageScope` for its own capture calls, but part of the
// recorder surface is Windows-only: `stage_mark` and the QPC helpers have no
// caller elsewhere, and `stage_recording` is only consulted by WGC. A non-Windows
// `--all-features` build therefore has to tolerate them; deleting the code would
// break every Windows caller.
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

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::{Path, PathBuf};

    /// Names the recorder passes to the marker functions, in call order.
    ///
    /// A marker's name may be a literal (`stage_record("gdi.convert", ...)`) or
    /// arrive through a parameter, but the literal always exists at some call
    /// site, so scanning the source finds every one of them.
    fn stage_names(source: &str) -> Vec<String> {
        // Longest first: `stage_record` is a prefix of `stage_record_since`, and
        // the shorter marker would otherwise win the tie and stop at `_since`.
        const MARKERS: [&str; 3] = ["stage_record_since", "stage_record", "stage_mark"];
        let mut names = Vec::new();
        let mut rest = source;
        while !rest.is_empty() {
            let next = MARKERS
                .iter()
                .filter_map(|marker| rest.find(marker).map(|at| (at, at + marker.len())))
                .min_by_key(|(at, _)| *at);
            let Some((_, after_name)) = next else {
                break;
            };
            rest = &rest[after_name..];
            let Some(call) = rest.trim_start().strip_prefix('(') else {
                // `stage_mark` mentioned in prose or in a `use` list.
                continue;
            };
            let Some(literal) = call.trim_start().strip_prefix('"') else {
                continue;
            };
            let Some(end) = literal.find('"') else {
                continue;
            };
            names.push(literal[..end].to_string());
            rest = &literal[end..];
        }
        names
    }

    fn rust_sources(dir: &Path, out: &mut Vec<PathBuf>) {
        let Ok(entries) = std::fs::read_dir(dir) else {
            return;
        };
        let mut paths: Vec<PathBuf> = entries.flatten().map(|entry| entry.path()).collect();
        paths.sort();
        for path in paths {
            if path.is_dir() {
                // The recorder's own tests use placeholder names (`stage.one`,
                // `detail.span`) that are fixtures, not capture stages.
                if path.file_name().is_some_and(|name| name == "timing") {
                    continue;
                }
                rust_sources(&path, out);
            } else if path.extension().is_some_and(|extension| extension == "rs") {
                out.push(path);
            }
        }
    }

    #[test]
    fn declared_families_are_prefixed_and_disjoint() {
        for prefix in BACKEND_STAGE_PREFIXES.iter().chain(AGNOSTIC_STAGE_PREFIXES) {
            assert!(
                prefix.ends_with('.') && prefix.len() > 1,
                "{prefix} is not a `family.` prefix"
            );
            assert!(
                !BACKEND_STAGE_PREFIXES.contains(prefix)
                    || !AGNOSTIC_STAGE_PREFIXES.contains(prefix),
                "{prefix} is declared twice"
            );
        }
    }

    #[test]
    fn known_stages_are_recognised_by_family_only() {
        assert!(is_known_stage("gdi.sys.bitblt"));
        assert!(is_known_stage("buffer.allocate"));
        assert!(!is_known_stage("stage.one"));
        assert!(!is_known_stage(""));
        // A family is a prefix, not a substring: the separator is required.
        assert!(!is_known_stage("gdi"));
        assert!(!is_known_stage("readback"));
    }

    /// Every stage name in the crate has to sit in a declared family.
    ///
    /// This exists because it already happened once: `buffer.allocate` was
    /// added to the frame allocator and the Windows smoke test -- the only
    /// other place stage names are enumerated -- kept its own copy of the
    /// allowed prefixes, went stale, and stayed red until someone ran it with
    /// `--features stage-timing` by hand. A source scan runs on every target,
    /// with or without the feature, so the next family cannot land silently.
    #[test]
    fn every_recorded_stage_name_sits_in_a_declared_family() {
        let source_root = Path::new(env!("CARGO_MANIFEST_DIR")).join("src");
        if !source_root.is_dir() {
            // `cargo package` drops nothing from `src`, but a vendored build
            // could. Skipping is honest: this is a source check, not a
            // behavioural one.
            println!(
                "{} is missing, skipping the stage-name scan",
                source_root.display()
            );
            return;
        }
        let mut files = Vec::new();
        rust_sources(&source_root, &mut files);
        assert!(
            files.len() > 5,
            "the scan found only {} sources under {}",
            files.len(),
            source_root.display()
        );

        let mut unknown = Vec::new();
        let mut seen = 0usize;
        for file in &files {
            let Ok(source) = std::fs::read_to_string(file) else {
                continue;
            };
            for name in stage_names(&source) {
                seen += 1;
                if !is_known_stage(&name) {
                    let relative = file.strip_prefix(&source_root).unwrap_or(file);
                    unknown.push(format!("{}: {name}", relative.display()));
                }
            }
        }

        assert!(
            seen >= 20,
            "the scan found {seen} stage names, which is too few to trust"
        );
        assert!(
            unknown.is_empty(),
            "stage names outside every declared family:\n{}",
            unknown.join("\n")
        );
    }
}
