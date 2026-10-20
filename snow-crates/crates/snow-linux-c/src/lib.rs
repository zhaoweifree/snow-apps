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

use std::ffi::{c_int, c_void};
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

/// C-side handler for activations, plus the opaque pointer it wants back.
///
/// The pointer outlives the service by contract: the caller keeps it valid until
/// `snow_linux_hotkeys_destroy`, which joins the worker before returning. That is
/// what makes the `unsafe impl Send` below sound -- the closure only ever copies
/// the pair and calls through the function pointer, and the worker thread cannot
/// outlive the handle.
#[derive(Clone, Copy)]
struct ActivationSink {
    handler: extern "C" fn(i32, *mut c_void),
    user_data: *mut c_void,
}

// SAFETY: see the type's documentation. The pair is `Copy`, never dereferenced
// here, and its lifetime is guaranteed by the C contract on the handle.
unsafe impl Send for ActivationSink {}

impl ActivationSink {
    fn invoke(self, registration_id: i32) {
        (self.handler)(registration_id, self.user_data);
    }
}

/// A running global-shortcut service.
pub struct SnowLinuxHotkeys {
    service: snow_linux::x11::hotkey::HotkeyService,
}

/// Result codes; they mirror `SNOW_LINUX_HOTKEY_*` in the header.
mod hotkey_status {
    pub const OK: i32 = 0;
    pub const KEY_NOT_ON_KEYBOARD: i32 = 1;
    pub const ALREADY_IN_USE: i32 = 2;
    pub const SERVER_ERROR: i32 = 3;
    pub const INVALID_BINDING: i32 = 4;
}

/// Opens a hotkey service.
///
/// Returns null when no X server is reachable or the keyboard tables cannot be
/// read; a null handler is refused as well, since a grab with nowhere to report
/// would take the key away from the session for no reason.
///
/// # Safety
///
/// `handler` must be a valid function pointer, and `user_data` must stay valid
/// until the returned handle is destroyed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_linux_hotkeys_create(
    handler: Option<extern "C" fn(i32, *mut c_void)>,
    user_data: *mut c_void,
) -> *mut SnowLinuxHotkeys {
    let Some(handler) = handler else {
        return ptr::null_mut();
    };
    let sink = ActivationSink { handler, user_data };
    let Ok(service) = snow_linux::x11::hotkey::HotkeyService::start(move |id| sink.invoke(id))
    else {
        return ptr::null_mut();
    };
    Box::into_raw(Box::new(SnowLinuxHotkeys { service }))
}

/// Releases every grab and joins the worker.
///
/// # Safety
///
/// `hotkeys` must be null or a pointer from [`snow_linux_hotkeys_create`] that
/// has not already been destroyed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_linux_hotkeys_destroy(hotkeys: *mut SnowLinuxHotkeys) {
    if hotkeys.is_null() {
        return;
    }
    // SAFETY: the caller's contract above says this came from `Box::into_raw` in
    // the create function and is released exactly once. Dropping it stops the
    // worker, which joins before this returns -- so `user_data` is not touched
    // afterwards, as the type's documentation promises.
    drop(unsafe { Box::from_raw(hotkeys) });
}

/// Whether a Qt key and modifier bits name a bindable combination.
///
/// Answered without contacting the server: this runs while the user is editing a
/// shortcut, and a round-trip per keystroke would be paid on every key press for
/// an answer that only depends on the static rules. Registration can still fail
/// afterwards when the key is not on this keyboard, which no static check can
/// know.
///
/// Returns [`hotkey_status::OK`] or [`hotkey_status::INVALID_BINDING`].
#[unsafe(no_mangle)]
pub extern "C" fn snow_linux_hotkeys_validate_binding(qt_key: i32, modifier_bits: u32) -> i32 {
    let Ok(qt_key) = u32::try_from(qt_key) else {
        return hotkey_status::INVALID_BINDING;
    };
    match snow_linux::hotkey::hotkey(qt_key, modifier_bits) {
        Ok(_) => hotkey_status::OK,
        Err(_) => hotkey_status::INVALID_BINDING,
    }
}

/// Grabs a Qt key with the given modifier bits. Re-registering an id replaces it.
///
/// # Safety
///
/// `hotkeys` must be null or a live handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_linux_hotkeys_register(
    hotkeys: *mut SnowLinuxHotkeys,
    registration_id: i32,
    qt_key: i32,
    modifier_bits: u32,
) -> i32 {
    let Some(handle) = (unsafe { hotkeys.as_ref() }) else {
        return hotkey_status::SERVER_ERROR;
    };
    let Ok(qt_key) = u32::try_from(qt_key) else {
        return hotkey_status::INVALID_BINDING;
    };
    let Ok(binding) = snow_linux::hotkey::hotkey(qt_key, modifier_bits) else {
        return hotkey_status::INVALID_BINDING;
    };
    match handle.service.register(registration_id, binding) {
        Ok(()) => hotkey_status::OK,
        Err(snow_linux::x11::hotkey::HotkeyRegistrationError::KeyNotOnKeyboard) => {
            hotkey_status::KEY_NOT_ON_KEYBOARD
        }
        Err(snow_linux::x11::hotkey::HotkeyRegistrationError::AlreadyInUse) => {
            hotkey_status::ALREADY_IN_USE
        }
        Err(snow_linux::x11::hotkey::HotkeyRegistrationError::Server(_)) => {
            hotkey_status::SERVER_ERROR
        }
    }
}

/// Releases the grabs held for `registration_id`.
///
/// # Safety
///
/// `hotkeys` must be null or a live handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_linux_hotkeys_unregister(
    hotkeys: *mut SnowLinuxHotkeys,
    registration_id: i32,
) {
    if let Some(handle) = unsafe { hotkeys.as_ref() } {
        handle.service.unregister(registration_id);
    }
}

/// Whether a key producing `qt_key` is held: 1 down, 0 up, -1 unknown.
///
/// # Safety
///
/// `hotkeys` must be null or a live handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_linux_hotkeys_key_down(
    hotkeys: *const SnowLinuxHotkeys,
    qt_key: i32,
) -> i32 {
    let Some(handle) = (unsafe { hotkeys.as_ref() }) else {
        return -1;
    };
    let Ok(qt_key) = u32::try_from(qt_key) else {
        return -1;
    };
    let Some(keysym) = snow_linux::hotkey::keysym_for_qt_key(qt_key) else {
        return -1;
    };
    match handle.service.key_down(keysym) {
        Some(true) => 1,
        Some(false) => 0,
        None => -1,
    }
}
