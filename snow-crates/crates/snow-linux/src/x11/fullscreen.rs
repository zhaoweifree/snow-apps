//! Reading the focused window for the full-screen check.
//!
//! Three facts have to be assembled before the pure rule in
//! [`crate::fullscreen`] can answer: which window has focus, how big its
//! top-level frame is, and what kind of window it is.
//!
//! *Focus* comes from `_NET_ACTIVE_WINDOW` on the root window, which is what
//! every EWMH window manager maintains. Servers that do not export it fall back
//! to the keyboard focus, walked up to its top-level ancestor.
//!
//! *Size* is read from the **top-level** window -- the ancestor whose parent is
//! the root. Under a reparenting manager that is the frame the manager drew
//! around the client, so its rectangle already includes the decorations; under a
//! manager that does not reparent it is the client itself. Reading the client's
//! own geometry instead would miss the decorations and, for a maximised window,
//! make the frame look one title bar short of the monitor.
//!
//! *Kind* has to come from the **client**, not the frame: the type property is
//! set by the application and inherited by nobody, so the search walks from the
//! focused window up to the top-level and takes the first property it finds.
//! Getting this wrong is not a crash -- the desktop backdrop would simply be
//! classified as a normal window, and an empty desktop would pause every global
//! shortcut.

use x11rb::protocol::xproto::{AtomEnum, ConnectionExt as _, InputFocus, MapState, Window};

use super::X11Session;
use super::window::TypeAtoms;
use crate::display::enumerate_monitors;
use crate::error::{LinuxError, LinuxResult};
use crate::fullscreen::{FocusedWindow, focused_is_fullscreen};
use crate::geometry::Rect;
use crate::window::{WindowKind, classify};

const NET_ACTIVE_WINDOW: &[u8] = b"_NET_ACTIVE_WINDOW";

/// How far up the tree to look for a root child before giving up.
///
/// Real window managers nest a frame and a client, so a handful of levels is
/// generous. The cap exists because a cycle in a broken server would otherwise
/// hang the timer that calls this.
const MAX_ANCESTRY_DEPTH: usize = 16;

/// A type list longer than this is not a window any manager produced.
const MAX_TYPE_ITEMS: u32 = 8;

/// Whether a full-screen window has focus, on any monitor of the session.
pub fn focused_fullscreen_exists(session: &X11Session) -> LinuxResult<bool> {
    let displays: Vec<Rect> = enumerate_monitors(session)?
        .into_iter()
        .map(|monitor| monitor.rect)
        .collect();
    focused_fullscreen_within(session, &displays)
}

/// Whether a full-screen window has focus and covers `display`.
///
/// `display` is a rectangle in root pixels. Qt knows the head it is asking
/// about, and passing it in keeps this function independent of whether RandR
/// describes the same layout -- a session where Qt's idea of the head and the
/// server's differ still gets an answer about the head the caller meant.
pub fn focused_fullscreen_on_display(session: &X11Session, display: Rect) -> LinuxResult<bool> {
    focused_fullscreen_within(session, &[display])
}

fn focused_fullscreen_within(session: &X11Session, displays: &[Rect]) -> LinuxResult<bool> {
    let atoms = Atoms::intern(session)?;
    let Some((_, window)) = focused_window(session, &atoms)? else {
        return Ok(false);
    };
    Ok(focused_is_fullscreen(Some(&window), displays))
}

/// The focused window together with the X11 id it was read from.
///
/// The id is what a capture request names its target by, so the window-capture
/// path needs it and the full-screen check does not. Both go through the same
/// read: which window is focused is a single fact, and answering it twice would
/// let the two paths disagree about a window that changed focus in between.
pub(crate) fn focused_window_with_id(
    session: &X11Session,
) -> LinuxResult<Option<(Window, FocusedWindow)>> {
    let atoms = Atoms::intern(session)?;
    focused_window(session, &atoms)
}

/// The atoms one check needs, interned once per call.
///
/// Interning is a round trip each and the check runs on a timer, so the two
/// lists are resolved before any other request is made instead of lazily from
/// the helpers below.
struct Atoms {
    active_window: u32,
    types: TypeAtoms,
}

impl Atoms {
    fn intern(session: &X11Session) -> LinuxResult<Self> {
        let active_window = session
            .conn()
            .intern_atom(false, NET_ACTIVE_WINDOW)
            .map_err(|error| LinuxError::backend("intern atom", error))?
            .reply()
            .map_err(|error| LinuxError::reply("intern atom", error))?
            .atom;
        Ok(Self {
            active_window,
            types: TypeAtoms::intern(session)?,
        })
    }
}

/// The focused window, or `None` when nothing usable has focus.
fn focused_window(
    session: &X11Session,
    atoms: &Atoms,
) -> LinuxResult<Option<(Window, FocusedWindow)>> {
    let Some(active) = active_window(session, atoms.active_window)? else {
        return Ok(None);
    };
    let Some((top, chain)) = ascend_to_top_level(session, active)? else {
        return Ok(None);
    };

    let conn = session.conn();
    let attributes = match conn
        .get_window_attributes(top)
        .map_err(|error| LinuxError::backend("read the focused window's attributes", error))?
        .reply()
    {
        Ok(reply) => reply,
        // The window closed between the property read and this request. That is
        // a normal race, not a failure of the check.
        Err(_) => return Ok(None),
    };
    let geometry = match conn
        .get_geometry(top)
        .map_err(|error| LinuxError::backend("read the focused window's geometry", error))?
        .reply()
    {
        Ok(reply) => reply,
        Err(_) => return Ok(None),
    };
    let translated = match conn
        .translate_coordinates(top, session.root(), 0, 0)
        .map_err(|error| LinuxError::backend("translate the focused window's origin", error))?
        .reply()
    {
        Ok(reply) if reply.same_screen => reply,
        // A window on another root has no position in this one's coordinates.
        _ => return Ok(None),
    };

    // The border is part of the frame the user sees, exactly as in the hit
    // test's geometry read.
    let border = i32::from(geometry.border_width);
    let frame = Rect::new(
        i32::from(translated.dst_x) - border,
        i32::from(translated.dst_y) - border,
        u32::from(geometry.width) + 2 * u32::from(geometry.border_width),
        u32::from(geometry.height) + 2 * u32::from(geometry.border_width),
    );

    Ok(Some((
        top,
        FocusedWindow {
            kind: window_kind(session, &atoms.types, &chain)?,
            viewable: attributes.map_state == MapState::VIEWABLE,
            frame,
        },
    )))
}

/// `_NET_ACTIVE_WINDOW`, falling back to the keyboard focus.
///
/// The property is preferred because it is what the user sees as active: a
/// manager may focus a hidden input-only window while a different one is drawn
/// on top. The fallback matters on a bare server with no window manager, where
/// the keyboard focus is the only thing that moves.
fn active_window(session: &X11Session, atom: u32) -> LinuxResult<Option<Window>> {
    let conn = session.conn();
    let reply = conn
        .get_property(false, session.root(), atom, AtomEnum::WINDOW, 0, 1)
        .map_err(|error| LinuxError::backend("read _NET_ACTIVE_WINDOW", error))?
        .reply()
        .map_err(|error| LinuxError::reply("read _NET_ACTIVE_WINDOW", error))?;

    if reply.type_ != u32::from(AtomEnum::NONE)
        && let Some(active) = reply.value32().and_then(|mut values| values.next())
        && active != 0
        && active != session.root()
    {
        return Ok(Some(active));
    }

    let reply = conn
        .get_input_focus()
        .map_err(|error| LinuxError::backend("read the input focus", error))?
        .reply()
        .map_err(|error| LinuxError::reply("read the input focus", error))?;
    // 0 is "none" and 1 is "pointer root": both are sentinels rather than window
    // ids, and neither describes a window the user is looking at.
    if reply.focus == 0 || reply.focus == u32::from(InputFocus::POINTER_ROOT) {
        return Ok(None);
    }
    Ok(Some(reply.focus))
}

/// Walk from `window` up to the ancestor whose parent is the root.
///
/// Returns the top-level window together with the chain from `window` up to it,
/// inclusive, so the type search does not have to walk the tree twice.
fn ascend_to_top_level(
    session: &X11Session,
    window: Window,
) -> LinuxResult<Option<(Window, Vec<Window>)>> {
    let root = session.root();
    if window == root {
        return Ok(None);
    }

    let conn = session.conn();
    let mut chain = vec![window];
    let mut current = window;
    for _ in 0..MAX_ANCESTRY_DEPTH {
        let tree = match conn
            .query_tree(current)
            .map_err(|error| LinuxError::backend("query the focused window's tree", error))?
            .reply()
        {
            Ok(reply) => reply,
            // A destroyed window ends the search rather than failing the check.
            Err(_) => return Ok(None),
        };
        if tree.parent == 0 || tree.parent == root {
            return Ok(Some((current, chain)));
        }
        current = tree.parent;
        chain.push(current);
    }

    // Deeper than any real window manager produces: report "no window" rather
    // than guessing which level was meant.
    Ok(None)
}

/// Classify the focused window from the nearest `_NET_WM_WINDOW_TYPE` in its
/// ancestry.
///
/// A missing property means a normal window: the property is optional, and
/// treating its absence as "not an application" would make the check blind on
/// every application that does not set it.
fn window_kind(
    session: &X11Session,
    atoms: &TypeAtoms,
    chain: &[Window],
) -> LinuxResult<WindowKind> {
    let conn = session.conn();
    for window in chain {
        let reply = match conn
            .get_property(
                false,
                *window,
                atoms.window_type(),
                AtomEnum::ATOM,
                0,
                MAX_TYPE_ITEMS,
            )
            .map_err(|error| LinuxError::backend("read _NET_WM_WINDOW_TYPE", error))?
            .reply()
        {
            Ok(reply) => reply,
            Err(_) => continue,
        };
        if reply.type_ == u32::from(AtomEnum::NONE) {
            continue;
        }
        let types: Vec<u32> = reply
            .value32()
            .map(|values| values.collect())
            .unwrap_or_default();
        return Ok(classify(&types, &atoms.roles()));
    }
    Ok(WindowKind::Normal)
}
