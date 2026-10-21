//! X11 transport for the PRIMARY selection.
//!
//! The protocol decisions (target preference, `INCR` classification, decoding,
//! limits) live in `crate::selection` and are unit tested on every platform.
//! What is left here is round trips and the deadline, which only a real
//! connection can exercise.

use std::time::{Duration, Instant};

use x11rb::connection::Connection;
use x11rb::protocol::Event;
use x11rb::protocol::xproto::{
    Atom, AtomEnum, ConnectionExt as _, CreateWindowAux, EventMask, Property, SelectionNotifyEvent,
    Window, WindowClass,
};
use x11rb::rust_connection::RustConnection;

use crate::model::{ErrorKind, SelectionError};
use crate::policy::Context;
use crate::selection::{
    Fetch, Incr, TARGETS, append_chunk, decode, fetch_step, next_offset, target_plan,
};

/// `GetProperty` length unit is four bytes, so this asks for 256 KiB per reply.
const CHUNK_LONGS: u32 = 64 * 1024;

/// Polling interval while waiting for an event the server will not block on.
///
/// `poll_for_event` never blocks (`RustConnection` puts the socket in
/// non-blocking mode at connect), so the deadline is enforced here rather than
/// by the library. One millisecond keeps the latency invisible against a 600 ms
/// stage budget without spinning a core.
const RETRY: Duration = Duration::from_millis(1);

/// Outcome of one PRIMARY read.
pub(super) enum PrimaryRead {
    Text(String),
    /// Nobody owns PRIMARY: reporting "no selection" is correct, not a failure.
    NoOwner,
    /// Somebody owns it but offers nothing this crate can decode.
    NoSupportedTarget,
}

/// A dedicated connection plus the unmapped window that receives the transfer.
///
/// The window is never mapped and never destroyed: it exists only so the owner
/// has a property to write into, and it dies with the connection when the worker
/// thread ends.
pub(super) struct Reader {
    conn: RustConnection,
    requestor: Window,
    atoms: Atoms,
}

struct Atoms {
    primary: Atom,
    /// The property the owner writes into on our window.
    property: Atom,
    targets: Atom,
    incr: Atom,
    /// `text[i]` is the atom for `TARGETS[i]`, so a `TARGETS` reply can be matched
    /// without asking the server to spell the names back.
    text: [Atom; TARGETS.len()],
}

impl Reader {
    pub(super) fn connect() -> Result<Self, SelectionError> {
        if std::env::var_os("DISPLAY").is_none_or(|value| value.is_empty()) {
            // A Wayland-only session has no X server, hence no PRIMARY selection
            // to read: the clipboard there belongs to the compositor.
            return Err(SelectionError::new(
                ErrorKind::UnsupportedPlatform,
                "PRIMARY selection",
            ));
        }
        let (conn, screen_num) =
            x11rb::connect(None).map_err(|error| backend("connecting to the display", error))?;
        let root = conn
            .setup()
            .roots
            .get(screen_num)
            .map(|screen| screen.root)
            .ok_or_else(|| SelectionError::new(ErrorKind::UnsupportedPlatform, "display screen"))?;

        let mut requests = Vec::with_capacity(4 + TARGETS.len());
        for name in ["PRIMARY", "SNOW_SHOT_SELECTION", "TARGETS", "INCR"] {
            requests.push(
                conn.intern_atom(false, name.as_bytes())
                    .map_err(|error| backend("interning a selection atom", error))?,
            );
        }
        for target in TARGETS {
            requests.push(
                conn.intern_atom(false, target.atom_name().as_bytes())
                    .map_err(|error| backend("interning a text atom", error))?,
            );
        }
        let mut atoms = Vec::with_capacity(requests.len());
        for request in requests {
            atoms.push(
                request
                    .reply()
                    .map_err(|error| reply("interning a selection atom", error))?
                    .atom,
            );
        }
        let text: [Atom; TARGETS.len()] = atoms[4..]
            .try_into()
            .map_err(|_| SelectionError::new(ErrorKind::NativeApi, "interned atom count"))?;

        let requestor = conn
            .generate_id()
            .map_err(|error| backend("allocating a requestor window", error))?;
        conn.create_window(
            x11rb::COPY_DEPTH_FROM_PARENT,
            requestor,
            root,
            0,
            0,
            1,
            1,
            0,
            WindowClass::INPUT_OUTPUT,
            x11rb::COPY_FROM_PARENT,
            &CreateWindowAux::new().event_mask(EventMask::PROPERTY_CHANGE),
        )
        .map_err(|error| backend("creating a requestor window", error))?;
        conn.flush()
            .map_err(|error| backend("creating a requestor window", error))?;

        Ok(Self {
            conn,
            requestor,
            atoms: Atoms {
                primary: atoms[0],
                property: atoms[1],
                targets: atoms[2],
                incr: atoms[3],
                text,
            },
        })
    }

    /// Read the whole PRIMARY selection, or a reason it is not readable.
    ///
    /// Returns `Ok(PrimaryRead::Text)` with the text the owner published, without
    /// ever writing to any selection: unlike the Windows and macOS paths there is
    /// no injection involved, so nothing outside this process changes.
    pub(super) fn read(
        &self,
        context: &Context,
        deadline: Instant,
    ) -> Result<PrimaryRead, SelectionError> {
        let owner = self
            .conn
            .get_selection_owner(self.atoms.primary)
            .map_err(|error| backend("reading the PRIMARY owner", error))?
            .reply()
            .map_err(|error| reply("reading the PRIMARY owner", error))?
            .owner;
        if owner == x11rb::NONE {
            return Ok(PrimaryRead::NoOwner);
        }

        let offered = self.offered_targets(context, deadline)?;
        for target in target_plan(offered.as_deref(), &self.atoms.text) {
            context.check()?;
            if let Some(bytes) = self.convert(self.atoms.text[target.index()], context, deadline)? {
                return Ok(PrimaryRead::Text(decode(target, &bytes)?));
            }
        }
        Ok(PrimaryRead::NoSupportedTarget)
    }

    /// Atoms the owner lists for `TARGETS`, or `None` when it refuses to answer.
    ///
    /// `None` means "ask for the supported targets one by one" rather than "no
    /// text": a refusal costs one round trip and several owners (`xterm`) only
    /// answer conversions, never `TARGETS`.
    fn offered_targets(
        &self,
        context: &Context,
        deadline: Instant,
    ) -> Result<Option<Vec<u32>>, SelectionError> {
        let Some(bytes) = self.convert(self.atoms.targets, context, deadline)? else {
            return Ok(None);
        };
        // `TARGETS` is an atom list: four bytes per entry. Truncating the tail to
        // whole atoms is the only sensible reading of anything else, and an owner
        // whose bytes then fail to match falls through to probing one by one.
        Ok(Some(
            bytes
                .chunks_exact(4)
                .map(|chunk| u32::from_ne_bytes([chunk[0], chunk[1], chunk[2], chunk[3]]))
                .collect(),
        ))
    }

    /// Ask the owner for one target. `None` means the owner refused it.
    fn convert(
        &self,
        target: Atom,
        context: &Context,
        deadline: Instant,
    ) -> Result<Option<Vec<u8>>, SelectionError> {
        context.check()?;
        // Clear whatever the previous target left behind. An owner that answers
        // `SelectionNotify` without writing the property would otherwise hand
        // back the previous target's text as if it were this one.
        self.conn
            .delete_property(self.requestor, self.atoms.property)
            .map_err(|error| backend("clearing the requestor property", error))?;
        self.conn
            .convert_selection(
                self.requestor,
                self.atoms.primary,
                target,
                self.atoms.property,
                // `CurrentTime` is what requestors send: there is no triggering
                // input event on this connection to borrow a timestamp from.
                x11rb::CURRENT_TIME,
            )
            .map_err(|error| backend("requesting the PRIMARY conversion", error))?;
        self.conn
            .flush()
            .map_err(|error| backend("requesting the PRIMARY conversion", error))?;

        let event = self.wait_selection(deadline)?;
        if event.property == x11rb::NONE {
            return Ok(None);
        }
        self.read_property(event.property, context, deadline)
            .map(Some)
    }

    /// Read a property to the end, following the `INCR` protocol when it is used.
    fn read_property(
        &self,
        property: Atom,
        context: &Context,
        deadline: Instant,
    ) -> Result<Vec<u8>, SelectionError> {
        let limit = context.options.max_text_bytes;
        let mut data = Vec::new();
        let mut offset = 0;
        let mut incr = Incr::Idle;
        loop {
            context.check()?;
            if Instant::now() >= deadline {
                return Err(timed_out("reading the PRIMARY property"));
            }
            let reply = self
                .conn
                .get_property(
                    // Never delete here: an `INCR` chunk needs deleting only after
                    // it has been read whole, which is one of the branches below.
                    false,
                    self.requestor,
                    property,
                    AtomEnum::ANY,
                    offset,
                    CHUNK_LONGS,
                )
                .map_err(|error| backend("reading the PRIMARY property", error))?
                .reply()
                .map_err(|error| reply("reading the PRIMARY property", error))?;
            match fetch_step(
                reply.format,
                reply.type_ == self.atoms.incr,
                reply.bytes_after,
                reply.value.len(),
                incr,
            )? {
                Fetch::StartIncr => {
                    incr = Incr::Awaiting;
                    // Deleting is the acknowledgement that makes the owner send
                    // the first chunk; without it the transfer never starts.
                    self.release(property)?;
                    self.wait_new_value(property, deadline)?;
                    offset = 0;
                }
                Fetch::End => {
                    self.release(property)?;
                    return Ok(data);
                }
                Fetch::Payload { more } => {
                    append_chunk(&mut data, &reply.value, limit)?;
                    if more {
                        offset = next_offset(offset, reply.value.len())?;
                    } else if incr == Incr::Awaiting {
                        self.release(property)?;
                        self.wait_new_value(property, deadline)?;
                        offset = 0;
                    } else {
                        self.release(property)?;
                        return Ok(data);
                    }
                }
            }
        }
    }

    fn release(&self, property: Atom) -> Result<(), SelectionError> {
        self.conn
            .delete_property(self.requestor, property)
            .map_err(|error| backend("releasing the requestor property", error))?;
        self.conn
            .flush()
            .map_err(|error| backend("releasing the requestor property", error))
    }

    fn wait_selection(&self, deadline: Instant) -> Result<SelectionNotifyEvent, SelectionError> {
        loop {
            match self
                .conn
                .poll_for_event()
                .map_err(|error| backend("waiting for SelectionNotify", error))?
            {
                Some(Event::SelectionNotify(event))
                    if event.requestor == self.requestor
                        && event.selection == self.atoms.primary =>
                {
                    return Ok(event);
                }
                Some(_) => {}
                None => pause(deadline, "waiting for SelectionNotify")?,
            }
        }
    }

    fn wait_new_value(&self, property: Atom, deadline: Instant) -> Result<(), SelectionError> {
        loop {
            match self
                .conn
                .poll_for_event()
                .map_err(|error| backend("waiting for a property chunk", error))?
            {
                Some(Event::PropertyNotify(event))
                    if event.window == self.requestor
                        && event.atom == property
                        && event.state == Property::NEW_VALUE =>
                {
                    return Ok(());
                }
                Some(_) => {}
                None => pause(deadline, "waiting for a property chunk")?,
            }
        }
    }
}

fn pause(deadline: Instant, operation: &'static str) -> Result<(), SelectionError> {
    if Instant::now() >= deadline {
        return Err(timed_out(operation));
    }
    std::thread::sleep(RETRY);
    Ok(())
}

fn timed_out(operation: &'static str) -> SelectionError {
    SelectionError::new(ErrorKind::TimedOut, operation)
}

/// A request the client failed to build. `SelectionError` reports operation names
/// only, so the transport detail stays here instead of reaching a caller that
/// cannot act on it.
fn backend(operation: &'static str, _error: impl std::fmt::Debug) -> SelectionError {
    SelectionError::new(ErrorKind::NativeApi, operation)
}

fn reply(operation: &'static str, _error: impl std::fmt::Debug) -> SelectionError {
    SelectionError::new(ErrorKind::NativeApi, operation)
}
