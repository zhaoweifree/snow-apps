//! Linux platform backend for Snow Apps.
//!
//! The crate is deliberately layered so that most of it stays verifiable on a
//! developer machine that is not running Linux:
//!
//! * [`geometry`] and [`pixels`] are pure and compile everywhere, so their unit
//!   tests run on any host.
//! * [`session`] only inspects environment variables and therefore compiles
//!   everywhere too.
//! * [`x11`], [`display`] and [`cursor`] talk to an X server and are gated on
//!   `target_os = "linux"`; they are type-checked with
//!   `cargo check --target x86_64-unknown-linux-gnu`.
//!
//! The X11 work is deliberately built on `x11rb` (pure Rust, no `libxcb`), so
//! that cross-target type checking above needs no C toolchain.

pub mod error;
pub mod geometry;
pub mod pixels;
pub mod session;

#[cfg(target_os = "linux")]
pub mod cursor;
#[cfg(target_os = "linux")]
pub mod display;
#[cfg(target_os = "linux")]
pub mod x11;

pub use error::{LinuxError, LinuxResult};
