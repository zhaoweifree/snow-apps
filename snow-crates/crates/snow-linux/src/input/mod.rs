//! Global input observation on Linux.
//!
//! Layered the same way as the rest of the crate:
//!
//! * [`keys`] and [`tracking`] are pure and compile everywhere, so their tests
//!   run on any host — they own the translation into the overlay's virtual-key
//!   space and the click/movement filtering rules.
//! * [`x11`] talks to an X server and is gated on `target_os = "linux"`; it is
//!   type-checked with `cargo check --target x86_64-unknown-linux-gnu`.
//!
//! Wayland has no equivalent of raw X11 events for unprivileged clients: the
//! only portable path there is the `GlobalShortcuts` and remote-desktop portals,
//! which will live behind the same observer interface once implemented.

pub mod keys;
pub mod tracking;

#[cfg(target_os = "linux")]
pub mod x11;

#[cfg(target_os = "linux")]
pub use x11::{InputEvent, InputEventKind, X11InputObserver};
