//! Thin, checked wrapper around a single X11 connection.
//!
//! All X11 traffic in the crate funnels through [`X11Session`] so that
//! connection setup, extension probing and error translation stay in one place.

use x11rb::connection::Connection;
use x11rb::protocol::randr;
use x11rb::protocol::xfixes;
use x11rb::protocol::xproto::{ConnectionExt as _, Screen, Window};
use x11rb::rust_connection::RustConnection;

use crate::error::{LinuxError, LinuxResult};

/// Cached presence of the X extensions this backend depends on.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ExtensionSupport {
    pub randr: bool,
    pub xfixes: bool,
}

/// An owned X11 connection plus the screen selected at connect time.
pub struct X11Session {
    conn: RustConnection,
    screen_num: usize,
    extension: ExtensionSupport,
}

impl std::fmt::Debug for X11Session {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("X11Session")
            .field("screen_num", &self.screen_num)
            .field("extension", &self.extension)
            .finish_non_exhaustive()
    }
}

impl X11Session {
    /// Connect using `$DISPLAY` (x11rb's `None` means "consult the environment").
    ///
    /// A host without `DISPLAY` is reported as [`LinuxError::SessionMissing`] so
    /// callers can degrade instead of treating a headless machine as a fault.
    /// Every other failure keeps the server's own message: a socket that is not
    /// accepting connections yet is transient (a compositor may still be coming
    /// up), so it must not be flattened into "unsupported".
    pub fn connect() -> LinuxResult<Self> {
        if std::env::var_os("DISPLAY").is_none_or(|value| value.is_empty()) {
            return Err(LinuxError::SessionMissing);
        }

        let (conn, screen_num) = x11rb::connect(None)
            .map_err(|err| LinuxError::other(format!("connecting to the X display: {err}")))?;

        // A well-formed display string can still name a screen the server does
        // not expose; that is as unusable as having no display at all.
        if screen_num >= conn.setup().roots.len() {
            return Err(LinuxError::SessionMissing);
        }

        let extension = probe_extensions(&conn);

        Ok(Self {
            conn,
            screen_num,
            extension,
        })
    }

    pub fn conn(&self) -> &RustConnection {
        &self.conn
    }

    pub fn screen(&self) -> &Screen {
        &self.conn.setup().roots[self.screen_num]
    }

    pub fn root(&self) -> Window {
        self.screen().root
    }

    pub fn extension_support(&self) -> ExtensionSupport {
        self.extension
    }

    /// RandR 1.5 monitor objects: the single source of truth for logical
    /// monitor geometry, including negative-origin multi-head layouts.
    pub fn randr_monitors(&self) -> LinuxResult<Vec<randr::MonitorInfo>> {
        if !self.extension.randr {
            return Err(LinuxError::other("the RandR extension is unavailable"));
        }
        let reply = randr::get_monitors(&self.conn, self.root(), false)
            .map_err(|err| LinuxError::backend("querying RandR monitors", err))?
            .reply()
            .map_err(|err| LinuxError::reply("querying RandR monitors", err))?;
        Ok(reply.monitors)
    }

    /// Resolve an atom to its name; unnamed atoms yield an empty string.
    pub fn atom_name(&self, atom: x11rb::protocol::xproto::Atom) -> LinuxResult<String> {
        let reply = self
            .conn
            .get_atom_name(atom)
            .map_err(|err| LinuxError::backend("resolving an atom name", err))?
            .reply()
            .map_err(|err| LinuxError::reply("resolving an atom name", err))?;
        Ok(String::from_utf8_lossy(&reply.name).into_owned())
    }

    pub fn xfixes_available(&self) -> bool {
        self.extension.xfixes
    }
}

fn probe_extensions(conn: &RustConnection) -> ExtensionSupport {
    // `GetMonitors` is a RandR 1.5 request: a 1.2-only server answers the
    // version handshake happily and then rejects the request, so the answer to
    // "is randr usable" has to come from the version, not from presence.
    let randr = randr::query_version(conn, 1, 5)
        .ok()
        .and_then(|cookie| cookie.reply().ok())
        .is_some_and(|reply| reply.major_version > 1 || reply.minor_version >= 5);

    let xfixes = xfixes::query_version(conn, 5, 0)
        .ok()
        .and_then(|cookie| cookie.reply().ok())
        .is_some();

    ExtensionSupport { randr, xfixes }
}
