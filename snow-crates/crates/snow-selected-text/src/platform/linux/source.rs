//! Best-effort identification of the window whose selection is being read.
//!
//! Every lookup here can fail without failing the capture. On X11 the PRIMARY
//! selection belongs to the session rather than to a window, so the text is
//! readable even when the focus cannot be named -- a bare X server with no window
//! manager has no `_NET_ACTIVE_WINDOW` at all. The fields exist for reporting and
//! for the caller's exclusion lists, and each one degrades on its own.

use std::sync::{Mutex, OnceLock};

use x11rb::connection::Connection;
use x11rb::protocol::xproto::{Atom, AtomEnum, ConnectionExt as _, Window};
use x11rb::rust_connection::RustConnection;

use crate::model::SourceApplication;
use crate::selection::executable_basename;

/// How far to walk towards the root when the active window is a frame.
const PARENT_WALK: usize = 3;

struct Session {
    conn: RustConnection,
    root: Window,
    active: Atom,
    pid: Atom,
}

/// One connection shared by every capture in this process.
///
/// A fresh handshake costs more than the three round trips it saves, and the
/// call runs before the host activates its own window, so it must stay short.
/// A failure to connect is cached too: the answer cannot change without a new
/// display, and retrying per keystroke would cost a failed connect each time.
fn session() -> Option<&'static Mutex<Session>> {
    static SESSION: OnceLock<Option<Mutex<Session>>> = OnceLock::new();
    SESSION
        .get_or_init(|| {
            let (conn, screen_num) = x11rb::connect(None).ok()?;
            let root = conn.setup().roots.get(screen_num)?.root;
            let active = conn
                .intern_atom(false, b"_NET_ACTIVE_WINDOW")
                .ok()?
                .reply()
                .ok()?
                .atom;
            let pid = conn
                .intern_atom(false, b"_NET_WM_PID")
                .ok()?
                .reply()
                .ok()?
                .atom;
            Some(Mutex::new(Session {
                conn,
                root,
                active,
                pid,
            }))
        })
        .as_ref()
}

/// Identify the active window. Never returns an error; unknown fields stay empty.
pub(super) fn capture() -> SourceApplication {
    let mut source = SourceApplication {
        native_window: None,
        process_id: 0,
        native_focus: None,
        executable: String::new(),
    };
    let Some(session) = session() else {
        return source;
    };
    let session = session.lock().unwrap_or_else(|error| error.into_inner());
    let Ok(Some(window)) = property_u32(&session, session.root, session.active) else {
        return source;
    };
    if window == x11rb::NONE {
        return source;
    }
    source.native_window = Some(window as usize);

    let Ok(Some(pid)) = window_pid(&session, window) else {
        return source;
    };
    source.process_id = pid;
    if let Ok(target) = std::fs::read_link(format!("/proc/{pid}/exe")) {
        source.executable = executable_basename(&target.to_string_lossy());
    }
    source
}

/// `_NET_WM_PID` of the active window or of the nearest ancestor that carries it.
///
/// `_NET_ACTIVE_WINDOW` is the client window by specification, but a window
/// manager that reports its own frame instead is common enough that one short
/// walk saves the exclusion lists from silently missing.
fn window_pid(session: &Session, window: Window) -> Result<Option<u32>, x11rb::errors::ReplyError> {
    let mut current = window;
    for _ in 0..PARENT_WALK {
        if let Some(pid) = property_u32(session, current, session.pid)? {
            return Ok(Some(pid));
        }
        if current == session.root {
            break;
        }
        let parent = session.conn.query_tree(current)?.reply()?.parent;
        if parent == x11rb::NONE || parent == current {
            break;
        }
        current = parent;
    }
    Ok(None)
}

/// First 32-bit value of a property, or `None` when it is absent or not a number.
fn property_u32(
    session: &Session,
    window: Window,
    property: Atom,
) -> Result<Option<u32>, x11rb::errors::ReplyError> {
    let reply = session
        .conn
        .get_property(false, window, property, AtomEnum::ANY, 0, 1)?
        .reply()?;
    Ok(reply.value32().and_then(|mut values| values.next()))
}
