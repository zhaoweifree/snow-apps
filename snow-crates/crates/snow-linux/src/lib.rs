//! Linux platform backend for Snow Apps.
//!
//! The crate is deliberately layered so that most of it stays verifiable on a
//! developer machine that is not running Linux:
//!
//! * [`capture`], [`geometry`], [`pixels`], [`text`] and [`input`]'s
//!   `keys`/`tracking` modules are pure and compile everywhere, so their unit
//!   tests run on any host.
//! * [`session`] only inspects environment variables and therefore compiles
//!   everywhere too.
//! * [`x11`], [`display`], [`cursor`] and [`input::x11`] talk to an X server and
//!   are gated on `target_os = "linux"`; they are type-checked with
//!   `cargo check --target x86_64-unknown-linux-gnu`.
//!
//! Nothing here links a C library. The X11 work is built on `x11rb` instead of
//! `libxcb`, and the text stack is built on `ab_glyph`/`ttf-parser` instead of
//! fontconfig and FreeType, so the cross-target type check above needs no C
//! toolchain and no target headers.

pub mod capture;
pub mod error;
pub mod geometry;
pub mod input;
pub mod pixels;
pub mod session;
pub mod text;

#[cfg(target_os = "linux")]
pub mod cursor;
#[cfg(target_os = "linux")]
pub mod display;
#[cfg(target_os = "linux")]
pub mod x11;

pub use error::{LinuxError, LinuxResult};
