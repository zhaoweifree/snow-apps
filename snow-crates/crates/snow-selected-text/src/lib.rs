//! Acquire the foreground application's selected text on Windows, macOS or Linux
//! without blocking its UI.
//!
//! Call [`SelectedTextService::start_capture`] before activating your own window.
//! Poll the returned [`CaptureRequest`] from your event loop. Copy fallback is
//! enabled by default and may change the clipboard; inspect [`ClipboardStatus`].
//! macOS capture never prompts for Accessibility permission. On Linux the text
//! comes from the X11 PRIMARY selection, which is read without touching the
//! clipboard at all. See the crate README for platform compatibility, clipboard
//! side effects, and timeout guarantees.

#[cfg(any(windows, test))]
mod clipboard;
mod model;
#[cfg(any(windows, target_os = "macos", target_os = "linux", test))]
mod policy;
mod runtime;
#[cfg(any(target_os = "linux", test))]
mod selection;

#[cfg(any(windows, target_os = "macos", target_os = "linux"))]
mod platform;

pub use model::*;
pub use runtime::{CaptureRequest, SelectedTextService};

#[cfg(test)]
mod tests;
