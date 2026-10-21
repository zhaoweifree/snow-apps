//! Linux platform backend for Snow Apps.
//!
//! The crate is deliberately layered so that most of it stays verifiable on a
//! developer machine that is not running Linux:
//!
//! * [`capture`], [`damage`], [`fullscreen`], [`geometry`], [`globalmouse`],
//!   [`hotkey`], [`pixels`], [`pointer`], [`text`], [`window`],
//!   [`windowcapture`], [`portal`] and [`input`]'s `keys`/`tracking` modules
//!   are pure and compile everywhere, so their unit tests run on any host.
//! * [`session`] only inspects environment variables and therefore compiles
//!   everywhere too.
//! * [`x11`], [`display`], [`cursor`], [`input::x11`] and [`portal::dbus`] talk
//!   to a server -- an X server, or the session bus -- and are gated on
//!   `target_os = "linux"`; they are type-checked with
//!   `cargo check --target x86_64-unknown-linux-gnu`.
//!
//! Nothing here links a C library. The X11 work is built on `x11rb` instead of
//! `libxcb`, the D-Bus work on `zbus` instead of `libdbus`, and the text stack
//! on `ab_glyph`/`ttf-parser` instead of fontconfig and FreeType, so the
//! cross-target type check above needs no C toolchain and no target headers.
//!
//! AT-SPI2 lives in the separate `snow-atspi` crate, not here: its two
//! consumers are independent C archives and one of them deliberately stays
//! clear of this crate's dependency surface.

pub mod capture;
pub mod damage;
pub mod error;
pub mod fullscreen;
pub mod geometry;
pub mod globalmouse;
pub mod hotkey;
pub mod input;
pub mod pixels;
pub mod pointer;
pub mod portal;
pub mod session;
pub mod text;
pub mod window;
pub mod windowcapture;

#[cfg(target_os = "linux")]
pub mod cursor;
#[cfg(target_os = "linux")]
pub mod display;
#[cfg(target_os = "linux")]
pub mod x11;

pub use error::{LinuxError, LinuxResult};
