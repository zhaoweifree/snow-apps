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

struct Atoms {
    client_list: u32,
    window_type: u32,
    desktop: u32,
    dock: u32,
    transient: Vec<u32>,
}

impl Atoms {
    fn roles(&self) -> WindowTypeRoles<'_> {
        WindowTypeRoles {
            desktop: self.desktop,
            dock: self.dock,
            transient: &self.transient,
        }
    }
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
    let intern = |name: &[u8]| -> LinuxResult<u32> {
        Ok(conn
            .intern_atom(false, name)
            .map_err(|error| LinuxError::backend("intern atom", error))?
            .reply()
            .map_err(|error| LinuxError::reply("intern atom", error))?
            .atom)
    };

    let mut transient = Vec::with_capacity(TRANSIENT_TYPES.len());
    for name in TRANSIENT_TYPES {
        transient.push(intern(name)?);
    }
    Ok(Atoms {
        client_list: intern(NET_CLIENT_LIST)?,
        window_type: intern(NET_WM_WINDOW_TYPE)?,
        desktop: intern(NET_WM_WINDOW_TYPE_DESKTOP)?,
        dock: intern(NET_WM_WINDOW_TYPE_DOCK)?,
        transient,
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
            atoms.window_type,
            AtomEnum::ATOM,
            0,
            MAX_PROPERTY_ITEMS,
        )
        .map_err(|error| LinuxError::backend("read _NET_WM_WINDOW_TYPE", error))?
        .reply()
    {
        Ok(reply) => reply
            .value32()
            .map(|values| classify(&values.collect::<Vec<_>>(), &atoms.roles()))
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
    }))
}
