//! C ABI over the Linux platform backend, for the Qt application.
//!
//! This archive is the surface `snow_shot`'s platform layer calls on Linux. It
//! is deliberately separate from `snow-shot-rust-ffi`: that archive pulls in the
//! whole capture/draw/recording graph, which reaches C sources (`zstd-sys` via
//! the stitching code), so `cargo check --target <linux-triple>` cannot get past
//! its dependencies from a Windows host. This one depends only on `snow-linux`,
//! which is pure Rust, so the cross-target check does cover it -- and this is the
//! part of the port that is Linux-only by definition, i.e. the part that most
//! needs a check the developer machine can run.
//!
//! On hosts that are not Linux the archive exports nothing at all. The platform
//! it wraps does not exist there, so there is nothing to stand in for.
//!
//! # Connection lifetime
//!
//! One connection per process, opened on first use. Qt constructs
//! `PhysicalCursor` as a temporary on hot paths -- the global canvas and the
//! colour picker both do -- so a connection per handle would mean an X11
//! handshake on every mouse move. The handle therefore owns no connection; it is
//! a capability token that also gives later calls somewhere to hang per-caller
//! state without changing the C side.
#![cfg(target_os = "linux")]

use std::ffi::c_int;
use std::ptr;
use std::sync::{Mutex, OnceLock};

use snow_linux::pointer::RootPosition;

/// The process-wide connection, opened the first time somebody asks for it.
///
/// The first attempt decides for the life of the process. A session that refuses
/// the connection is either permanently absent (a Wayland-only login with no
/// XWayland) or transiently busy (a compositor still coming up); retrying per
/// call would spend a handshake to relearn the same answer, and the Qt
/// application cannot run without a reachable display anyway.
static SESSION: OnceLock<Option<Mutex<snow_linux::x11::X11Session>>> = OnceLock::new();

fn session() -> Option<&'static Mutex<snow_linux::x11::X11Session>> {
    SESSION
        .get_or_init(|| snow_linux::x11::X11Session::connect().ok().map(Mutex::new))
        .as_ref()
}

/// Opaque handle. The C++ side owns one behind a `shared_ptr` deleter.
pub struct SnowLinuxPlatform {
    _private: (),
}

/// Opens the platform.
///
/// Returns null when no X server is reachable, which is also the condition that
/// makes every other entry point here fail. Callers use null as "the cursor
/// backend is unsupported on this session" rather than as an error to report.
#[unsafe(no_mangle)]
pub extern "C" fn snow_linux_platform_create() -> *mut SnowLinuxPlatform {
    if session().is_none() {
        return ptr::null_mut();
    }
    Box::into_raw(Box::new(SnowLinuxPlatform { _private: () }))
}

/// Releases a handle from [`snow_linux_platform_create`]. Does not close the
/// connection: other handles and other callers may still be using it.
///
/// # Safety
///
/// `platform` must be null or a pointer returned by
/// [`snow_linux_platform_create`] that has not already been released.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_linux_platform_destroy(platform: *mut SnowLinuxPlatform) {
    if platform.is_null() {
        return;
    }
    // SAFETY: the caller's contract above says this pointer came from
    // `Box::into_raw` in the create function and is released exactly once.
    drop(unsafe { Box::from_raw(platform) });
}

/// Writes the pointer position in root-window pixels.
///
/// These are device pixels. Qt's own logical coordinates are a fraction of them
/// when the session scales, and the conversion belongs on the Qt side, which
/// knows the screen the point falls on.
///
/// Returns 1 on success and 0 when no position is available: no platform, a null
/// output pointer, or a pointer sitting on a root this connection does not own.
///
/// # Safety
///
/// `platform` must be null or a live handle, and both outputs must be null or
/// point at writable `int32_t`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_linux_platform_pointer_position(
    platform: *const SnowLinuxPlatform,
    x: *mut i32,
    y: *mut i32,
) -> c_int {
    if platform.is_null() || x.is_null() || y.is_null() {
        return 0;
    }
    let Some(lock) = session() else {
        return 0;
    };
    // A poisoned lock means a panic unwound while a request was in flight, and
    // the connection may be half-way through a message. Reporting the position
    // as unavailable is the only honest answer; pretending the stream is still
    // aligned would resynchronise on the wrong byte.
    let Ok(connection) = lock.lock() else {
        return 0;
    };
    let Ok(sample) = connection.pointer_sample() else {
        return 0;
    };
    let Some(position) = sample.position() else {
        return 0;
    };
    // SAFETY: both pointers were checked for null above; the C contract is that
    // they point at writable `int32_t`.
    unsafe {
        *x = i32::from(position.x);
        *y = i32::from(position.y);
    }
    1
}

/// Moves the pointer to an absolute root position.
///
/// Returns 1 when the request was accepted and flushed, 0 when no platform, the
/// coordinates are outside the range X11 can express, or the server refused.
///
/// # Safety
///
/// `platform` must be null or a live handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_linux_platform_set_pointer_position(
    platform: *mut SnowLinuxPlatform,
    x: i32,
    y: i32,
) -> c_int {
    if platform.is_null() {
        return 0;
    }
    // Refused rather than clamped: a request from outside the root coordinate
    // space is a caller bug, and silently moving the cursor to the nearest edge
    // hides it behind a pointer that is merely in the wrong place.
    let Some(target) = RootPosition::new(x, y) else {
        return 0;
    };
    let Some(lock) = session() else {
        return 0;
    };
    let Ok(connection) = lock.lock() else {
        return 0;
    };
    if connection.warp_pointer_to(target).is_ok() {
        1
    } else {
        0
    }
}
