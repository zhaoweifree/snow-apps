//! Client-window enumeration over EWMH.
//!
//! `_NET_CLIENT_LIST` is the window manager's own list of managed top-level
//! windows, already in bottom-to-top stacking order, which is exactly what a
//! hit test needs. The property is optional, so a server that does not export
//! it falls back to the root window's children -- the frame windows that a
//! reparenting manager creates -- which is coarser but still a real answer.
//!
//! Every per-window request is allowed to fail. A window can be destroyed
//! between the list read and the attribute read, and that race must drop one
//! candidate rather than fail the whole enumeration: the alternative is a
//! selector that reports an error whenever any application closes.

use x11rb::protocol::xproto::{AtomEnum, ConnectionExt as _, MapState, Window, WindowClass};

use super::X11Session;
use crate::error::{LinuxError, LinuxResult};
use crate::geometry::Rect;
use crate::window::{
    DesktopWindows, WindowAttributes, WindowKind, WindowTypeRoles, classify, desktop_windows,
};

const NET_CLIENT_LIST: &[u8] = b"_NET_CLIENT_LIST";
const NET_WM_PID: &[u8] = b"_NET_WM_PID";
const NET_WM_WINDOW_TYPE: &[u8] = b"_NET_WM_WINDOW_TYPE";
const NET_WM_WINDOW_TYPE_DESKTOP: &[u8] = b"_NET_WM_WINDOW_TYPE_DESKTOP";
const NET_WM_WINDOW_TYPE_DOCK: &[u8] = b"_NET_WM_WINDOW_TYPE_DOCK";

/// Types that describe a surface belonging to another window's interaction
/// rather than a window the user picks as a region.
const TRANSIENT_TYPES: [&[u8]; 7] = [
    b"_NET_WM_WINDOW_TYPE_TOOLTIP",
    b"_NET_WM_WINDOW_TYPE_DROPDOWN_MENU",
    b"_NET_WM_WINDOW_TYPE_POPUP_MENU",
    b"_NET_WM_WINDOW_TYPE_NOTIFICATION",
    b"_NET_WM_WINDOW_TYPE_DND",
    b"_NET_WM_WINDOW_TYPE_COMBO",
    b"_NET_WM_WINDOW_TYPE_SPLASH",
];

/// Property reads are capped rather than unbounded: a window count in the
/// thousands is not a desktop anyone selects on, and the cap keeps one absurd
/// property from turning a hover into a large allocation.
const MAX_PROPERTY_ITEMS: u32 = 1024;

/// The interned window-type atoms, paired with their meaning.
///
/// Shared with the full-screen check, which has to classify the focused window
/// for the same reason the hit test classifies its candidates: the desktop
/// backdrop covers a monitor exactly and must never be mistaken for an
/// application.
pub(crate) struct TypeAtoms {
    window_type: u32,
    desktop: u32,
    dock: u32,
    transient: Vec<u32>,
}

impl TypeAtoms {
    /// Intern every type atom in one batch.
    ///
    /// The requests are all written before any reply is awaited, so the set
    /// costs one round trip instead of one per atom -- which matters because the
    /// full-screen check runs on a timer.
    pub(crate) fn intern(session: &X11Session) -> LinuxResult<Self> {
        let conn = session.conn();
        let mut names: Vec<&[u8]> = vec![
            NET_WM_WINDOW_TYPE,
            NET_WM_WINDOW_TYPE_DESKTOP,
            NET_WM_WINDOW_TYPE_DOCK,
        ];
        names.extend(TRANSIENT_TYPES);

        let mut cookies = Vec::with_capacity(names.len());
        for name in &names {
            cookies.push(
                conn.intern_atom(false, name)
                    .map_err(|error| LinuxError::backend("intern atom", error))?,
            );
        }
        let mut atoms = Vec::with_capacity(cookies.len());
        for cookie in cookies {
            atoms.push(
                cookie
                    .reply()
                    .map_err(|error| LinuxError::reply("intern atom", error))?
                    .atom,
            );
        }

        let mut resolved = atoms.into_iter();
        let window_type = resolved.next().unwrap_or_default();
        let desktop = resolved.next().unwrap_or_default();
        let dock = resolved.next().unwrap_or_default();
        Ok(Self {
            window_type,
            desktop,
            dock,
            transient: resolved.collect(),
        })
    }

    pub(crate) fn window_type(&self) -> u32 {
        self.window_type
    }

    pub(crate) fn roles(&self) -> WindowTypeRoles<'_> {
        WindowTypeRoles {
            desktop: self.desktop,
            dock: self.dock,
            transient: &self.transient,
        }
    }
}

struct Atoms {
    client_list: u32,
    pid: u32,
    types: TypeAtoms,
}

/// The windows the pointer may select, in bottom-to-top stacking order.
pub fn desktop_windows_for(session: &X11Session, excluded: &[u64]) -> LinuxResult<DesktopWindows> {
    let atoms = intern_atoms(session)?;
    let root = session.root();
    let candidates = match client_list(session, root, &atoms)? {
        Some(list) => list,
        None => root_children(session, root)?,
    };

    let mut collected = Vec::with_capacity(candidates.len());
    for window in candidates {
        if let Some(attributes) = read_window(session, root, &atoms, window)? {
            collected.push(attributes);
        }
    }
    Ok(desktop_windows(&collected, excluded, session.root_rect()))
}

fn intern_atoms(session: &X11Session) -> LinuxResult<Atoms> {
    let conn = session.conn();
    // Both atoms are interned before either reply is awaited, so the pair costs
    // one round trip. `TypeAtoms::intern` does the same for its own batch.
    let client_list = conn
        .intern_atom(false, NET_CLIENT_LIST)
        .map_err(|error| LinuxError::backend("intern atom", error))?;
    let pid = conn
        .intern_atom(false, NET_WM_PID)
        .map_err(|error| LinuxError::backend("intern atom", error))?;
    let client_list = client_list
        .reply()
        .map_err(|error| LinuxError::reply("intern atom", error))?
        .atom;
    let pid = pid
        .reply()
        .map_err(|error| LinuxError::reply("intern atom", error))?
        .atom;
    Ok(Atoms {
        client_list,
        pid,
        types: TypeAtoms::intern(session)?,
    })
}

/// The window manager's client list, or `None` when it does not export one.
///
/// Distinguishing "no property" from "empty list" matters: only the former
/// should fall back to walking the tree, because an empty list is a manager
/// saying there are no managed windows.
fn client_list(
    session: &X11Session,
    root: Window,
    atoms: &Atoms,
) -> LinuxResult<Option<Vec<Window>>> {
    let conn = session.conn();
    let reply = conn
        .get_property(
            false,
            root,
            atoms.client_list,
            AtomEnum::WINDOW,
            0,
            MAX_PROPERTY_ITEMS,
        )
        .map_err(|error| LinuxError::backend("read _NET_CLIENT_LIST", error))?
        .reply()
        .map_err(|error| LinuxError::reply("read _NET_CLIENT_LIST", error))?;

    if reply.type_ == u32::from(AtomEnum::NONE) {
        return Ok(None);
    }
    Ok(Some(
        reply
            .value32()
            .map(|values| values.collect())
            .unwrap_or_default(),
    ))
}

/// Root window children, in the server's stacking order.
fn root_children(session: &X11Session, root: Window) -> LinuxResult<Vec<Window>> {
    let reply = session
        .conn()
        .query_tree(root)
        .map_err(|error| LinuxError::backend("query window tree", error))?
        .reply()
        .map_err(|error| LinuxError::reply("query window tree", error))?;
    Ok(reply.children)
}

/// Read one candidate, or `None` when it disappeared mid-enumeration.
fn read_window(
    session: &X11Session,
    root: Window,
    atoms: &Atoms,
    window: Window,
) -> LinuxResult<Option<WindowAttributes>> {
    let conn = session.conn();
    let attributes = match conn
        .get_window_attributes(window)
        .map_err(|error| LinuxError::backend("read window attributes", error))?
        .reply()
    {
        Ok(reply) => reply,
        Err(_) => return Ok(None),
    };

    let kind = match conn
        .get_property(
            false,
            window,
            atoms.types.window_type,
            AtomEnum::ATOM,
            0,
            MAX_PROPERTY_ITEMS,
        )
        .map_err(|error| LinuxError::backend("read _NET_WM_WINDOW_TYPE", error))?
        .reply()
    {
        Ok(reply) => reply
            .value32()
            .map(|values| classify(&values.collect::<Vec<_>>(), &atoms.types.roles()))
            .unwrap_or(WindowKind::Normal),
        // A missing type property is normal for most applications, and a
        // window that vanishes here is simply dropped from selection rather
        // than being reported as a normal window.
        Err(_) => return Ok(None),
    };

    let geometry = match conn
        .get_geometry(window)
        .map_err(|error| LinuxError::backend("read window geometry", error))?
        .reply()
    {
        Ok(reply) => reply,
        Err(_) => return Ok(None),
    };
    let translated = match conn
        .translate_coordinates(window, root, 0, 0)
        .map_err(|error| LinuxError::backend("translate window origin", error))?
        .reply()
    {
        Ok(reply) if reply.same_screen => reply,
        // A window on another screen has no position in this screen's root
        // coordinates, and a destroyed one has none at all.
        _ => return Ok(None),
    };

    // The border is part of what the user sees, so the frame rectangle
    // includes it; `translate_coordinates` reports the origin of the interior.
    let border = i32::from(geometry.border_width);
    let bounds = Rect::new(
        i32::from(translated.dst_x) - border,
        i32::from(translated.dst_y) - border,
        u32::from(geometry.width) + 2 * u32::from(geometry.border_width),
        u32::from(geometry.height) + 2 * u32::from(geometry.border_width),
    );

    Ok(Some(WindowAttributes {
        id: u64::from(window),
        kind,
        viewable: attributes.map_state == MapState::VIEWABLE,
        override_redirect: attributes.override_redirect,
        has_pixels: attributes.class == WindowClass::INPUT_OUTPUT,
        bounds,
        pid: window_pid(session, root, window, atoms.pid),
    }))
}

/// How many ancestors are searched for `_NET_WM_PID`.
///
/// The property belongs on the client window, which is exactly what
/// `_NET_CLIENT_LIST` names, so the first read is normally the answer. The walk
/// exists for window managers that copy the property onto their frame, and the
/// cap keeps a reparenting chain -- frame, group leader, root -- from turning
/// into an unbounded traversal on every enumeration.
const MAX_PID_ANCESTORS: u32 = 8;

/// The process that created `window`, or `None` when it does not say.
///
/// A published zero is reported as absent. `_NET_WM_PID` has no meaning at zero,
/// and a caller that matched on it would pair the window with whichever
/// application also failed to publish one.
fn window_pid(session: &X11Session, root: Window, window: Window, atom: u32) -> Option<u32> {
    let conn = session.conn();
    let mut current = window;
    for _ in 0..MAX_PID_ANCESTORS {
        let reply = conn
            .get_property(false, current, atom, AtomEnum::CARDINAL, 0, 1)
            .ok()
            .and_then(|cookie| cookie.reply().ok())?;
        if let Some(pid) = reply.value32().and_then(|mut values| values.next())
            && pid != 0
        {
            return Some(pid);
        }
        let parent = conn
            .query_tree(current)
            .ok()
            .and_then(|cookie| cookie.reply().ok())
            .map(|tree| tree.parent)
            .unwrap_or_default();
        // No parent to walk to: a window whose parent is itself would loop, and
        // the root has nothing above it.
        if parent == 0 || parent == current || parent == root {
            return None;
        }
        current = parent;
    }
    None
}
