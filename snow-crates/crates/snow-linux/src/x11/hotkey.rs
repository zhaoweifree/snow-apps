//! Global shortcuts over `XGrabKey`.
//!
//! A passive key grab is a promise to the server: while this client lives and
//! holds the grab, a press of that keycode with exactly that modifier state is
//! delivered to it and to nobody else. That is the X11 equivalent of
//! `RegisterHotKey`, and it is what makes a screenshot hotkey work while another
//! application has focus.
//!
//! # Why a second connection and a worker thread
//!
//! Grabs are per-connection, and the events they generate arrive on the
//! connection that issued them. The capture session's connection is used for
//! synchronous round-trips (`GetImage` blocks the whole stream while it is in
//! flight), so a grab there would only be observed between captures. This
//! service therefore owns its own connection and polls it on a dedicated thread,
//! which also means a slow capture can never delay a hotkey.
//!
//! The connection is *not* shared with the pointer helpers in
//! [`super::pointer`]: those are one-shot round-trips, and putting a blocking
//! event loop on the same socket would make every eager reader race the loop for
//! the reply.
//!
//! # Why polling rather than blocking
//!
//! `wait_for_event` cannot be interrupted, and a hotkey service that cannot be
//! torn down would outlive the process's own shutdown sequence. Waiting on the
//! command channel with a timeout gives the same idle cost as the input
//! observer's loop (ten wake-ups a second) and a bounded worst-case latency of
//! one interval, which for a keyboard shortcut is imperceptible.

use std::collections::HashMap;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::Duration;

use crossbeam_channel::{Receiver, Sender};
use x11rb::connection::{Connection, RequestConnection};
use x11rb::protocol::Event;
use x11rb::protocol::xproto::{
    ConnectionExt as _, GrabKeyRequest, GrabMode, ModMask, UngrabKeyRequest,
};
use x11rb::rust_connection::RustConnection;

use crate::error::LinuxResult;
use crate::hotkey::{self, Hotkey, HotkeyModifiers};
use crate::x11::X11Session;

/// How long the worker sleeps when there is neither a command nor an event.
const IDLE_POLL_INTERVAL: Duration = Duration::from_millis(10);

/// How long a caller waits for the worker to answer a register/unregister.
///
/// Generous for what is one round-trip to a local server, and short enough that
/// a wedged server surfaces as "registration failed" instead of a frozen UI.
const COMMAND_TIMEOUT: Duration = Duration::from_secs(2);

/// Every modifier bit X11 defines. The high bits of `state` are buttons, which
/// must not take part in matching a key grab.
const MODIFIER_STATE_MASK: u16 = 0x00ff;

/// Shift, which is required when a binding's keysym only lives on the shifted
/// level of its keycode.
const SHIFT_MASK: u16 = 0x0001;

/// Lock (CapsLock). The protocol fixes this one; NumLock and ScrollLock do not
/// have fixed masks and are read back from the server.
const LOCK_MASK: u16 = 0x0002;

/// Control, also fixed by the protocol.
const CONTROL_MASK: u16 = 0x0004;

/// `Mod1` and `Mod4`, the conventional homes of Alt and Super. Used only when
/// the keyboard mapping does not identify them, which is the case for a layout
/// with no Alt or Super key at all.
const FALLBACK_ALT_MASK: u16 = 0x0008;
const FALLBACK_META_MASK: u16 = 0x0040;

const KEYSYM_ALT_L: u32 = 0xffe9;
const KEYSYM_ALT_R: u32 = 0xffea;
const KEYSYM_META_L: u32 = 0xffe7;
const KEYSYM_META_R: u32 = 0xffe8;
const KEYSYM_SUPER_L: u32 = 0xffeb;
const KEYSYM_SUPER_R: u32 = 0xffec;
const KEYSYM_HYPER_L: u32 = 0xffed;
const KEYSYM_HYPER_R: u32 = 0xffee;
const KEYSYM_NUM_LOCK: u32 = 0xff7f;
const KEYSYM_SCROLL_LOCK: u32 = 0xff14;

/// Why a binding could not be grabbed.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum HotkeyRegistrationError {
    /// The keysym is not on this keyboard: nothing to grab.
    KeyNotOnKeyboard,
    /// This service already holds that combination for another id, or another
    /// client holds it. X11 reports the latter as `BadAccess`.
    AlreadyInUse,
    /// The server refused for another reason. The payload is the X protocol
    /// error code, or 0 when the service did not answer in time.
    Server(u8),
}

/// Called on the worker thread for every activated registration.
///
/// The handler runs on the worker thread, and must not call back into
/// [`HotkeyService`]: registration waits for that same thread to answer, so a
/// call from inside the handler would wait for itself. The C++ side posts to the
/// Qt thread instead, which is asynchronous by construction.
pub type ActivationHandler = Box<dyn Fn(i32) + Send + 'static>;

enum Command {
    Register {
        id: i32,
        binding: Hotkey,
        reply: Sender<Result<(), HotkeyRegistrationError>>,
    },
    Unregister {
        id: i32,
        reply: Sender<()>,
    },
    KeyDown {
        keysym: u32,
        reply: Sender<Option<bool>>,
    },
    Stop,
}

/// The parts of the keyboard description that turn a binding into grabs.
struct Tables {
    min_keycode: u8,
    per_keycode: usize,
    keysyms: Vec<u32>,
    alt_mask: u16,
    meta_mask: u16,
    /// The lock modifiers this keyboard actually has, so a grab survives
    /// CapsLock / NumLock / ScrollLock being on.
    lock_masks: Vec<u16>,
}

impl Tables {
    fn read(conn: &RustConnection) -> LinuxResult<Self> {
        let setup = conn.setup();
        let min_keycode = setup.min_keycode;
        // `count` is one byte wide in the protocol: the whole keycode space is
        // 8 bits, so the inclusive span always fits.
        let count = setup.max_keycode - setup.min_keycode + 1;
        let keyboard = conn
            .get_keyboard_mapping(min_keycode, count)
            .map_err(|err| crate::error::LinuxError::backend("reading the keyboard mapping", err))?
            .reply()
            .map_err(|err| crate::error::LinuxError::reply("reading the keyboard mapping", err))?;
        let modifier = conn
            .get_modifier_mapping()
            .map_err(|err| crate::error::LinuxError::backend("reading the modifier mapping", err))?
            .reply()
            .map_err(|err| crate::error::LinuxError::reply("reading the modifier mapping", err))?;

        let per_keycode = usize::from(keyboard.keysyms_per_keycode);
        // The modifier map is one flat array of keycodes, `keycodes_per_modifier`
        // per modifier, in mask order. A zero width would mean the server has no
        // modifier keys at all, which is legal but useless here.
        let mut modifier_map: Vec<Vec<u8>> = if modifier.keycodes_per_modifier() == 0 {
            Vec::new()
        } else {
            modifier
                .keycodes
                .chunks(usize::from(modifier.keycodes_per_modifier()))
                .map(<[u8]>::to_vec)
                .collect()
        };
        modifier_map.resize(8, Vec::new());

        let mask_for = |wanted: &[u32]| {
            hotkey::modifier_mask_for(
                &modifier_map,
                min_keycode,
                per_keycode,
                &keyboard.keysyms,
                wanted,
            )
        };
        // Alt is Mod1 on every mainstream layout, but "on every mainstream
        // layout" is an assumption, and the cost of checking is one reply that
        // is needed anyway for the keyboard mapping.
        let alt_mask = mask_for(&[KEYSYM_ALT_L, KEYSYM_ALT_R])
            .or_else(|| mask_for(&[KEYSYM_META_L, KEYSYM_META_R]))
            .unwrap_or(FALLBACK_ALT_MASK);
        let meta_mask = mask_for(&[KEYSYM_SUPER_L, KEYSYM_SUPER_R])
            .or_else(|| mask_for(&[KEYSYM_HYPER_L, KEYSYM_HYPER_R]))
            .unwrap_or(FALLBACK_META_MASK);

        let mut lock_masks = vec![LOCK_MASK];
        for keysym in [KEYSYM_NUM_LOCK, KEYSYM_SCROLL_LOCK] {
            if let Some(mask) = mask_for(&[keysym]) {
                // A "lock" that is really the Alt or Super modifier would turn
                // every Alt binding into a grab of Alt plus itself; skip it and
                // accept that hotkeys will miss while that key is held.
                if mask != alt_mask && mask != meta_mask && !lock_masks.contains(&mask) {
                    lock_masks.push(mask);
                }
            }
        }

        Ok(Self {
            min_keycode,
            per_keycode,
            keysyms: keyboard.keysyms,
            alt_mask,
            meta_mask,
            lock_masks,
        })
    }

    fn mask(&self, modifiers: HotkeyModifiers) -> u16 {
        let mut mask = 0;
        if modifiers.contains(hotkey::MODIFIER_CONTROL) {
            mask |= CONTROL_MASK;
        }
        if modifiers.contains(hotkey::MODIFIER_SHIFT) {
            mask |= SHIFT_MASK;
        }
        if modifiers.contains(hotkey::MODIFIER_ALT) {
            mask |= self.alt_mask;
        }
        if modifiers.contains(hotkey::MODIFIER_META) {
            mask |= self.meta_mask;
        }
        mask
    }

    fn keycode_matches(&self, keysym: u32) -> Vec<hotkey::KeycodeMatch> {
        hotkey::keycode_matches(keysym, self.min_keycode, self.per_keycode, &self.keysyms)
    }

    /// Whether any keycode currently producing `keysym` is held down.
    fn is_down(&self, conn: &RustConnection, keysym: u32) -> Option<bool> {
        let reply = conn.query_keymap().ok()?.reply().ok()?;
        Self::pressed_in(&reply.keys, &self.keycode_matches(keysym))
    }

    fn pressed_in(bitmap: &[u8], matches: &[hotkey::KeycodeMatch]) -> Option<bool> {
        if bitmap.is_empty() {
            return None;
        }
        for item in matches {
            let index = usize::from(item.keycode);
            let byte = bitmap.get(index / 8)?;
            if (byte >> (index % 8)) & 1 != 0 {
                return Some(true);
            }
        }
        Some(false)
    }
}

/// A running grab set, plus polling helpers for key state.
pub struct HotkeyService {
    commands: Sender<Command>,
    stopped: Arc<AtomicBool>,
    worker: Option<std::thread::JoinHandle<()>>,
}

impl std::fmt::Debug for HotkeyService {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("HotkeyService")
            .field("stopped", &self.stopped.load(Ordering::Acquire))
            .finish_non_exhaustive()
    }
}

impl HotkeyService {
    /// Connects and starts the worker.
    ///
    /// Fails only when no X server is reachable or its keyboard tables cannot be
    /// read; a bad individual binding is reported by [`Self::register`] so one
    /// shortcut being taken by another client does not disable the rest.
    pub fn start(handler: impl Fn(i32) + Send + 'static) -> LinuxResult<Self> {
        let session = X11Session::connect()?;
        let tables = Tables::read(session.conn())?;
        let root = session.root();
        let conn = session.into_connection();

        let (commands, receiver) = crossbeam_channel::unbounded();
        let stopped = Arc::new(AtomicBool::new(false));
        let thread_state = Arc::clone(&stopped);
        let worker = std::thread::Builder::new()
            .name("snow-linux-hotkey".into())
            .spawn(move || {
                snow_core::qos::apply_current_thread();
                let handler: ActivationHandler = Box::new(handler);
                run(conn, root, tables, handler, receiver);
                thread_state.store(true, Ordering::Release);
            })
            .map_err(|err| {
                crate::error::LinuxError::other(format!("starting the hotkey service: {err}"))
            })?;

        Ok(Self {
            commands,
            stopped,
            worker: Some(worker),
        })
    }

    /// Grabs the combination for `id`, replacing any previous binding for it.
    pub fn register(&self, id: i32, binding: Hotkey) -> Result<(), HotkeyRegistrationError> {
        let (reply, answers) = crossbeam_channel::bounded(1);
        if self
            .commands
            .send(Command::Register { id, binding, reply })
            .is_err()
        {
            return Err(HotkeyRegistrationError::Server(0));
        }
        answers
            .recv_timeout(COMMAND_TIMEOUT)
            .unwrap_or(Err(HotkeyRegistrationError::Server(0)))
    }

    /// Releases the grabs held for `id`. Unknown ids are ignored.
    pub fn unregister(&self, id: i32) {
        let (reply, answers) = crossbeam_channel::bounded(1);
        if self
            .commands
            .send(Command::Unregister { id, reply })
            .is_ok()
        {
            let _ = answers.recv_timeout(COMMAND_TIMEOUT);
        }
    }

    /// Whether a key producing `keysym` is currently held.
    ///
    /// Returns `None` when the worker cannot answer in time, which callers report
    /// as "not observable" rather than as "not pressed": a shortcut picker that
    /// treats unavailable state as "nothing is down" would accept a half-finished
    /// combination.
    pub fn key_down(&self, keysym: u32) -> Option<bool> {
        let (reply, answers) = crossbeam_channel::bounded(1);
        self.commands
            .send(Command::KeyDown { keysym, reply })
            .ok()?;
        // A timed-out query yields `None` -- neither "down" nor "up", because the
        // caller has to be able to tell an unanswerable query from a released
        // key.
        answers
            .recv_timeout(Duration::from_millis(200))
            .ok()
            .flatten()
    }

    pub fn stopped(&self) -> bool {
        self.stopped.load(Ordering::Acquire)
    }
}

impl Drop for HotkeyService {
    fn drop(&mut self) {
        let _ = self.commands.send(Command::Stop);
        if let Some(worker) = self.worker.take() {
            // Bounded: the worker never blocks longer than one poll interval.
            let _ = worker.join();
        }
    }
}

fn run(
    conn: RustConnection,
    root: u32,
    tables: Tables,
    handler: ActivationHandler,
    commands: Receiver<Command>,
) {
    // id -> the exact (keycode, modifier state) pairs it grabbed. Matching on the
    // state the server reports is the only reliable way to tell which binding a
    // KeyPress belongs to once CapsLock or NumLock changes the state bits.
    let mut owned: HashMap<i32, Vec<(u8, u16)>> = HashMap::new();

    loop {
        match commands.recv_timeout(IDLE_POLL_INTERVAL) {
            Ok(Command::Register { id, binding, reply }) => {
                let outcome = register(&conn, root, &tables, &mut owned, id, binding);
                let _ = reply.send(outcome);
            }
            Ok(Command::Unregister { id, reply }) => {
                release(&conn, root, &mut owned, id);
                let _ = reply.send(());
            }
            Ok(Command::KeyDown { keysym, reply }) => {
                // `is_down` answers `None` when the server did not reply; that
                // has to reach the caller as "unknown", not as "released".
                let _ = reply.send(tables.is_down(&conn, keysym));
            }
            Ok(Command::Stop) | Err(crossbeam_channel::RecvTimeoutError::Disconnected) => break,
            Err(crossbeam_channel::RecvTimeoutError::Timeout) => {}
        }

        // Drain everything the server has queued: a grab delivers one event per
        // press, and holding a key down makes the server repeat them, which is
        // the same behaviour `RegisterHotKey` has on Windows.
        loop {
            match conn.poll_for_event() {
                Ok(Some(Event::KeyPress(event))) => {
                    let state = u16::from(event.state) & MODIFIER_STATE_MASK;
                    let keycode = event.detail;
                    if let Some(id) = owned
                        .iter()
                        .find(|(_, grabs)| {
                            grabs
                                .iter()
                                .any(|&(code, mask)| code == keycode && mask == state)
                        })
                        .map(|(id, _)| *id)
                    {
                        handler(id);
                    }
                }
                Ok(Some(_)) => {}
                Ok(None) => break,
                // A dead connection cannot be revived in place; the service keeps
                // running but stops producing activations, which is what the
                // caller would see anyway if the session went away.
                Err(_) => return,
            }
        }
    }

    for (_, grabs) in owned {
        for (keycode, mask) in grabs {
            release_one(&conn, root, keycode, mask);
        }
    }
    let _ = conn.flush();
}

fn register(
    conn: &RustConnection,
    root: u32,
    tables: &Tables,
    owned: &mut HashMap<i32, Vec<(u8, u16)>>,
    id: i32,
    binding: Hotkey,
) -> Result<(), HotkeyRegistrationError> {
    // Re-registering the same id is a replace, not an error: the manager applies
    // a whole shortcut list at once and would otherwise have to unregister first
    // to be safe against its own retries.
    release(conn, root, owned, id);

    let matches = tables.keycode_matches(binding.keysym);
    if matches.is_empty() {
        return Err(HotkeyRegistrationError::KeyNotOnKeyboard);
    }

    let base = tables.mask(binding.modifiers);
    let mut grabs = Vec::new();
    for item in matches {
        let mask = if item.implied_shift {
            base | SHIFT_MASK
        } else {
            base
        };
        for variant in hotkey::grab_masks(mask, &tables.lock_masks) {
            if !grabs.contains(&(item.keycode, variant)) {
                grabs.push((item.keycode, variant));
            }
        }
    }

    // Two registrations for one combination would make one keypress fire two
    // actions. Windows answers the second `RegisterHotKey` with "already in use"
    // and the manager shows that in the UI, so X11 has to answer the same way
    // rather than quietly permitting it.
    let taken = owned.values().flatten().any(|grab| grabs.contains(grab));
    if taken {
        return Err(HotkeyRegistrationError::AlreadyInUse);
    }

    let mut issued = Vec::with_capacity(grabs.len());
    for &(keycode, mask) in &grabs {
        match grab(conn, root, keycode, mask) {
            Ok(()) => issued.push((keycode, mask)),
            Err(error) => {
                // All-or-nothing: a half-grabbed binding would fire on some lock
                // states and not others, which reads as an intermittent bug.
                for (keycode, mask) in issued {
                    release_one(conn, root, keycode, mask);
                }
                let _ = conn.flush();
                return Err(error);
            }
        }
    }
    let _ = conn.flush();
    owned.insert(id, grabs);
    Ok(())
}

fn grab(
    conn: &RustConnection,
    root: u32,
    keycode: u8,
    mask: u16,
) -> Result<(), HotkeyRegistrationError> {
    let request = GrabKeyRequest {
        // The events must reach this connection, not the focused window: the
        // whole point is to act while another application has focus.
        owner_events: false,
        grab_window: root,
        modifiers: ModMask::from(mask),
        key: keycode,
        // Passive grab: neither the pointer nor the keyboard is frozen while it
        // is held, which a synchronous mode would do for the entire session.
        pointer_mode: GrabMode::ASYNC,
        keyboard_mode: GrabMode::ASYNC,
    };
    let result = conn
        .send_trait_request_without_reply(request)
        .map_err(|_| HotkeyRegistrationError::Server(0))?
        .check();
    match result {
        Ok(()) => Ok(()),
        Err(x11rb::errors::ReplyError::X11Error(error)) => {
            Err(if error.error_kind == x11rb::protocol::ErrorKind::Access {
                HotkeyRegistrationError::AlreadyInUse
            } else {
                HotkeyRegistrationError::Server(error.error_code)
            })
        }
        Err(_) => Err(HotkeyRegistrationError::Server(0)),
    }
}

fn release(conn: &RustConnection, root: u32, owned: &mut HashMap<i32, Vec<(u8, u16)>>, id: i32) {
    if let Some(grabs) = owned.remove(&id) {
        for (keycode, mask) in grabs {
            release_one(conn, root, keycode, mask);
        }
        let _ = conn.flush();
    }
}

fn release_one(conn: &RustConnection, root: u32, keycode: u8, mask: u16) {
    let _ = conn.send_trait_request_without_reply(UngrabKeyRequest {
        key: keycode,
        grab_window: root,
        modifiers: ModMask::from(mask),
    });
}
