//! The transport: AT-SPI2 over the accessibility bus.
//!
//! Everything in here needs a running desktop with accessibility enabled, so on
//! the development machine it is type-checked for both Linux targets and never
//! executed. What can be decided without a bus -- the search order, the budgets,
//! the cycle guard, the offset arithmetic -- is in the parent module, and is
//! covered by tests that never come near this file.
//!
//! Three things about this layer are worth reading before changing it.
//!
//! **It is a second bus.** AT-SPI2 does not live on the session bus: a launcher
//! publishes an address on the session bus (`org.a11y.Bus.GetAddress`), and the
//! widget trees are on the connection that address names. Both connections are
//! held, because the address is only an address -- application names such as
//! `:1.42` are meaningful on the accessibility bus and nowhere else.
//!
//! **Every call is bounded, and by the connection rather than by a timer.**
//! A toolkit that stopped answering would otherwise hang the caller with no way
//! out: the capture path is synchronous. `Builder::method_timeout` makes the
//! runtime's own call future expire, which costs nothing when it is not needed.
//! Note what this rules out: wrapping each call in `portal::deadline` would
//! spawn one thread per call, and a bounded search makes hundreds of calls.
//!
//! **A failed call is "no", not "abort".** Half the widgets in a real tree do
//! not implement `Text`; asking one for its selection fails, and that failure
//! says nothing about the application. So reads return `Option` and the walk
//! treats a missing answer as an empty list. Only [`AccessibilityBus::connect`]
//! reports an error, because "there is no accessibility bus" is a fact the
//! caller has to act on (it falls back to the PRIMARY selection).

use std::time::Duration;

use zbus::Connection;
use zbus::connection::Builder;
use zbus::export::serde::de::DeserializeOwned;
use zbus::wire::{OwnedObjectPath, Type};

use super::{
    AccessibleSource, DEFAULT_WALK_LIMITS, Focus, MAX_TEXT_BYTES, ObjectRef, ROOT_PATH,
    TEXT_INTERFACE, TextSelection, choose_selection, clamp_text, extents_to_rect, find_focused,
    match_app_by_pid,
};
use crate::error::{LinuxError, LinuxResult};
use crate::geometry::Rect;

/// The launcher on the session bus, and the one method it answers.
const LAUNCHER_SERVICE: &str = "org.a11y.Bus";
const LAUNCHER_PATH: &str = "/org/a11y/bus";
const LAUNCHER_INTERFACE: &str = "org.a11y.Bus";

/// The bus daemon, for the one lookup that ties a name to a process.
const DBUS_SERVICE: &str = "org.freedesktop.DBus";
const DBUS_PATH: &str = "/org/freedesktop/DBus";
const DBUS_INTERFACE: &str = "org.freedesktop.DBus";

/// How long one D-Bus call may take.
///
/// Generous for a local socket (a toolkit answers in microseconds) and short
/// enough that a wedged application costs half a second instead of the capture.
const CALL_TIMEOUT: Duration = Duration::from_millis(500);

/// What a focused text node reports.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct FocusReading {
    /// The selected characters, clamped to [`MAX_TEXT_BYTES`].
    pub text: String,
    /// Where that selection is drawn, in desktop coordinates, when the toolkit
    /// could tell. `None` is normal -- bubble placement then falls back to the
    /// pointer.
    pub bounds: Option<Rect>,
}

/// A connection to the accessibility bus.
pub struct AccessibilityBus {
    connection: Connection,
}

impl AccessibilityBus {
    /// Connect to the accessibility bus through the session bus.
    ///
    /// Fails when there is no session bus, or when the launcher refuses -- which
    /// is what happens on a desktop with accessibility switched off. The caller
    /// treats both as "this route is unavailable" and falls back; neither is
    /// worth retrying in-process.
    pub fn connect() -> LinuxResult<Self> {
        let session = zbus::block_on(Connection::session())
            .map_err(|error| LinuxError::other(format!("cannot reach the session bus: {error}")))?;

        let reply = zbus::block_on(session.call_method(
            Some(LAUNCHER_SERVICE),
            LAUNCHER_PATH,
            Some(LAUNCHER_INTERFACE),
            "GetAddress",
            &(),
        ))
        .map_err(|error| {
            LinuxError::other(format!(
                "the accessibility bus launcher did not answer: {error}"
            ))
        })?;

        let address: String = reply.body().deserialize().map_err(|error| {
            LinuxError::other(format!(
                "the accessibility bus address is not a string: {error}"
            ))
        })?;
        if address.is_empty() {
            return Err(LinuxError::other(
                "the accessibility bus launcher returned an empty address",
            ));
        }

        let connection = zbus::block_on(
            Builder::address(address.as_str())
                .method_timeout(CALL_TIMEOUT)
                .build(),
        )
        .map_err(|error| {
            LinuxError::other(format!("cannot connect to the accessibility bus: {error}"))
        })?;

        Ok(Self { connection })
    }

    /// The applications on the desktop, each with the pid behind its bus name.
    ///
    /// Applications whose name cannot be resolved to a pid are left out rather
    /// than reported with a placeholder: a zero pid would match every other
    /// zero, turning "unknown" into "wrong application".
    pub fn applications(&self) -> Vec<(ObjectRef, u32)> {
        self.children(&ObjectRef::desktop())
            .into_iter()
            .filter_map(|application| {
                let pid = self.pid_of(&application.name)?;
                Some((application, pid))
            })
            .collect()
    }

    /// The focused node inside `pid`'s application, if it has one.
    pub fn focused(&self, pid: u32) -> Option<Focus> {
        let application = match_app_by_pid(&self.applications(), pid)?;
        let mut tree = AppTree { bus: self };
        find_focused(&mut tree, &[application], DEFAULT_WALK_LIMITS)
    }

    /// Read the selection of the focused text node inside `pid`'s application.
    ///
    /// `Ok(None)` covers every ordinary "nothing selected" case: no such
    /// application, nothing focused, focus on a button, a caret with no range,
    /// a range the toolkit reports as empty. They are not distinguished because
    /// no caller acts differently on them -- the next route is PRIMARY either
    /// way.
    pub fn read_selection(&self, pid: u32) -> LinuxResult<Option<FocusReading>> {
        let Some(Focus::Text(node)) = self.focused(pid) else {
            return Ok(None);
        };
        Ok(self.selection_of(&node))
    }

    /// The selection of one text node.
    fn selection_of(&self, node: &ObjectRef) -> Option<FocusReading> {
        let count: i32 = self.call(node, TEXT_INTERFACE, "GetNSelections", &())?;
        let selection = choose_selection(count, |index| {
            // `GetSelection` answers with two `i` values, not a struct.
            self.call::<_, (i32, i32)>(node, TEXT_INTERFACE, "GetSelection", &(index,))
                .map(|(start, end)| TextSelection::new(start, end))
        })?;

        let text: String = self.call(
            node,
            TEXT_INTERFACE,
            "GetText",
            &(selection.start, selection.end),
        )?;
        if text.is_empty() {
            return None;
        }

        // The rectangle is asked for with the same range as the text: a toolkit
        // that renumbered its text between the two calls returns extents that
        // no longer cover what was read, and there is no way to detect that
        // from here -- the text is what matters, the rectangle is optional.
        let bounds = self
            .call::<_, (i32, i32, i32, i32)>(
                node,
                TEXT_INTERFACE,
                "GetRangeExtents",
                // 0 is `ATSPI_COORD_TYPE_SCREEN`: desktop coordinates, which is
                // what an overlay drawn by this process needs.
                &(selection.start, selection.end, 0u32),
            )
            .and_then(extents_to_rect);

        Some(FocusReading {
            text: clamp_text(&text, MAX_TEXT_BYTES).to_string(),
            bounds,
        })
    }

    /// The pid behind a bus name on the accessibility bus.
    fn pid_of(&self, name: &str) -> Option<u32> {
        if name.is_empty() {
            return None;
        }
        self.call(
            &ObjectRef::new(DBUS_SERVICE, DBUS_PATH),
            DBUS_INTERFACE,
            "GetConnectionUnixProcessID",
            &(name,),
        )
    }

    /// `Accessible.GetChildren`, as owned references.
    fn children(&self, node: &ObjectRef) -> Vec<ObjectRef> {
        let children: Vec<(String, OwnedObjectPath)> =
            match self.call(node, super::ACCESSIBLE_INTERFACE, "GetChildren", &()) {
                Some(children) => children,
                None => return Vec::new(),
            };
        children
            .into_iter()
            .map(|(name, path)| node.child(ObjectRef::new(name, path.as_str())))
            .collect()
    }

    /// One bounded call, with the reply deserialised by its declared shape.
    ///
    /// The interface strings are passed in rather than derived from constants
    /// here so that the one place that names a method also names its interface:
    /// a call to the right method on the wrong interface fails at runtime with
    /// `UnknownInterface`, which a type check cannot catch.
    fn call<B, R>(&self, node: &ObjectRef, interface: &str, member: &str, body: &B) -> Option<R>
    where
        B: zbus::export::serde::Serialize + Type,
        R: DeserializeOwned + Type,
    {
        let reply = zbus::block_on(self.connection.call_method(
            Some(node.name.as_str()),
            node.path.as_str(),
            Some(interface),
            member,
            body,
        ))
        .ok()?;
        reply.body().deserialize::<R>().ok()
    }
}

/// [`AccessibleSource`] over a live connection.
struct AppTree<'a> {
    bus: &'a AccessibilityBus,
}

impl AccessibleSource for AppTree<'_> {
    fn children(&mut self, node: &ObjectRef) -> Vec<ObjectRef> {
        self.bus.children(node)
    }

    fn states(&mut self, node: &ObjectRef) -> Vec<u32> {
        self.bus
            .call(node, super::ACCESSIBLE_INTERFACE, "GetState", &())
            .unwrap_or_default()
    }

    fn interfaces(&mut self, node: &ObjectRef) -> Vec<String> {
        self.bus
            .call(node, super::ACCESSIBLE_INTERFACE, "GetInterfaces", &())
            .unwrap_or_default()
    }
}

/// The desktop root, for a caller that wants to enumerate applications itself.
pub fn desktop() -> ObjectRef {
    ObjectRef::new(super::REGISTRY_SERVICE, ROOT_PATH)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The reply signatures this layer relies on, spelled the way the interface
    /// XML spells them. A typo in a Rust type here is a signature mismatch at
    /// runtime, and this is the only place it can be noticed off-target.
    #[test]
    fn the_reply_shapes_match_the_interface_xml() {
        assert_eq!(<i32 as Type>::SIGNATURE.to_string(), "i");
        assert_eq!(<u32 as Type>::SIGNATURE.to_string(), "u");
        assert_eq!(<String as Type>::SIGNATURE.to_string(), "s");
        assert_eq!(<bool as Type>::SIGNATURE.to_string(), "b");
        assert_eq!(<Vec<u32> as Type>::SIGNATURE.to_string(), "au");
        assert_eq!(<Vec<String> as Type>::SIGNATURE.to_string(), "as");
        assert_eq!(
            <Vec<(String, OwnedObjectPath)> as Type>::SIGNATURE.to_string(),
            "a(so)"
        );
        // `GetSelection` returns two separate `i` values; `GetRangeExtents`
        // returns one `(iiii)` struct. Both deserialise into a Rust tuple, and
        // both bodies read as a structure of integers.
        assert_eq!(<(i32, i32) as Type>::SIGNATURE.to_string(), "(ii)");
        assert_eq!(
            <(i32, i32, i32, i32) as Type>::SIGNATURE.to_string(),
            "(iiii)"
        );
    }

    #[test]
    fn the_desktop_reference_is_the_registry_root() {
        let root = desktop();
        assert_eq!(root.name, "org.a11y.atspi.Registry");
        assert_eq!(root.path, "/org/a11y/atspi/accessible/root");
        assert!(!root.is_null());
    }

    #[test]
    fn a_reading_without_bounds_is_still_a_reading() {
        // Bubble placement falls back to the pointer, so missing geometry must
        // not turn a readable selection into nothing.
        let reading = FocusReading {
            text: "选中".to_string(),
            bounds: None,
        };
        assert_eq!(reading.text, "选中");
        assert_eq!(FocusReading::default().text, "");
    }
}
