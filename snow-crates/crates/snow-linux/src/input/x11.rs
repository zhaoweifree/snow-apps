//! Passive, session-scoped input observation over XInput2 raw events.
//!
//! Like the macOS tap, this is an *observer*: it selects events and never grabs,
//! rewrites or synthesises them. Raw events are delivered to every client that
//! selected them, regardless of grabs, which is what makes them the X11
//! equivalent of a Windows low-level hook.
//!
//! # Why raw events and not the core event stream
//!
//! Core `KeyPress`/`ButtonPress` events are redirected to whichever client holds
//! the grab, so a global observer built on them silently loses everything typed
//! into an application that grabbed the keyboard. XInput2 raw events are not
//! redirected and are the only correct source for a recorder.
//!
//! # Coordinates
//!
//! Raw motion events carry their position in device valuators, whose encoding
//! depends on the device and on which of `axisvalues`/`axisvalues_raw` holds the
//! accelerated value. Neither can be validated without a server to test
//! against, and a wrong choice produces a cursor trail that is *plausibly*
//! wrong — the worst kind of failure. This backend therefore takes positions
//! from `QueryPointer`, which is unambiguous on every server and device version.
//! That costs one round-trip per sampled movement, so motion is sampled at
//! [`MOTION_SAMPLE_INTERVAL`] and coalesced in between; the consumer keeps only
//! the latest movement anyway.
//!
//! # State ownership
//!
//! The observer owns the pressed-key table and the keyboard mapping, and stamps
//! every event with them. Key events therefore cost no round-trips at all, and
//! the adapter above only has to rename fields.

use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::time::{Duration, Instant};

use crossbeam_channel::{Receiver, Sender};
use x11rb::connection::Connection;
use x11rb::protocol::Event;
use x11rb::protocol::xinput::{self, EventMask, XIEventMask};
use x11rb::protocol::xproto::{self, ConnectionExt as _};
use x11rb::rust_connection::RustConnection;

use crate::error::{LinuxError, LinuxResult};
use crate::input::keys::{self, KeyMap};
use crate::input::tracking::{self, Modifiers};
use crate::x11::X11Session;

/// Upper bound on how often a movement is reported.
///
/// 8 ms is 125 Hz, comfortably above the frame rate the trail is composited at,
/// and it bounds the `QueryPointer` traffic the sampler generates. The Windows
/// hook reports every movement; the consumer coalesces them to the latest one
/// per output frame, so sampling this side loses nothing it would have kept.
pub const MOTION_SAMPLE_INTERVAL: Duration = Duration::from_millis(8);

/// How long the worker sleeps when the server has nothing to report.
///
/// The connection is polled rather than blocked on because the socket lives
/// inside the worker: `Drop` has no way to interrupt a blocking read, and a
/// recording must always be able to tear its observer down. Polling costs 100
/// wake-ups a second while idle — an order of magnitude less than the capture
/// path's own frame timer — and adds no latency, because a pending event wakes
/// the loop on the next pass instead of waiting the interval out.
const IDLE_POLL_INTERVAL: Duration = Duration::from_millis(10);

/// XInput version this backend needs for raw events.
const REQUIRED_MAJOR: u16 = 2;

/// `XIAllDevices`.
///
/// x11rb models a device id as a plain `u16` and does not name the two pseudo
/// ids the protocol reserves, so the value is spelled out here: `0` selects every
/// master *and* slave device, which is what lets a device attached after the
/// observer starts be observed without re-selecting.
const XI_ALL_DEVICES: u16 = 0;

/// What an observed event is.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum InputEventKind {
    KeyPress,
    KeyRelease,
    ButtonPress,
    ButtonRelease,
    Motion,
}

/// One observation of the global input stream.
///
/// Positions are root-window coordinates: the adapter applies the recorded
/// region, because the region belongs to the recording, not to the platform.
#[derive(Clone, Debug)]
pub struct InputEvent {
    pub kind: InputEventKind,
    /// Raw X11 keycode; the identity of last resort for a key event.
    pub keycode: u8,
    /// Symbol the keycode produced, already resolved for the shift state.
    /// `None` for pointer events.
    pub keysym: Option<u32>,
    /// X11 button number for button events, `0` otherwise.
    pub button: u8,
    pub x: i32,
    pub y: i32,
    /// Modifier state at the moment of the event.
    pub modifiers: Modifiers,
    /// Pressed keys, indexed by the overlay's virtual-key space.
    ///
    /// A full snapshot per event rather than a delta: the consumer builds an
    /// observation value out of it, and a snapshot cannot drift.
    pub pressed: [u8; 256],
    /// Server millisecond clock. Not comparable across servers, so callers order
    /// by their own receipt time instead.
    pub server_time: u32,
    /// Changes after a dropped batch. Observations carrying a generation the
    /// consumer has already passed must be discarded, because a key release may
    /// have been lost with the batch.
    pub generation: u64,
}

struct State {
    stop: AtomicBool,
    generation: AtomicU64,
    dropped: AtomicU64,
}

/// An active XInput2 observer.
pub struct X11InputObserver {
    pub events: Receiver<InputEvent>,
    state: Arc<State>,
    worker: Option<std::thread::JoinHandle<()>>,
}

impl std::fmt::Debug for X11InputObserver {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("X11InputObserver")
            .field("generation", &self.generation())
            .field("dropped", &self.dropped())
            .finish_non_exhaustive()
    }
}

impl X11InputObserver {
    /// Start observing.
    ///
    /// `keyboard` and `mouse` select which raw event classes are subscribed to;
    /// asking for neither is a configuration error rather than an empty stream.
    pub fn start(keyboard: bool, mouse: bool) -> LinuxResult<Self> {
        if !keyboard && !mouse {
            return Err(LinuxError::other("no input event types selected"));
        }

        // The observer owns its connection: it needs a private event queue, and
        // sharing the capture session's would let a long `GetImage` reply starve
        // the input stream.
        let session = X11Session::connect()?;

        let reply = xinput::xi_query_version(session.conn(), REQUIRED_MAJOR, 0)
            .map_err(|err| LinuxError::backend("querying the XInput extension", err))?
            .reply()
            .map_err(|err| LinuxError::reply("querying the XInput extension", err))?;
        if reply.major_version < REQUIRED_MAJOR {
            return Err(LinuxError::UnsupportedInputBackend {
                reason: "the X server provides no XInput2, so raw input events cannot be observed",
            });
        }

        let mut mask = Vec::with_capacity(5);
        if keyboard {
            mask.push(XIEventMask::RAW_KEY_PRESS);
            mask.push(XIEventMask::RAW_KEY_RELEASE);
        }
        if mouse {
            mask.push(XIEventMask::RAW_BUTTON_PRESS);
            mask.push(XIEventMask::RAW_BUTTON_RELEASE);
            mask.push(XIEventMask::RAW_MOTION);
        }
        // `DeviceId::ALL` (XIAllDevices) covers slaves and masters alike, so a
        // device attached after the observer starts is picked up without
        // re-selecting — which is what a recorder needs across input hot-plug.
        xinput::xi_select_events(
            session.conn(),
            session.root(),
            &[EventMask {
                deviceid: XI_ALL_DEVICES,
                mask,
            }],
        )
        .map_err(|err| LinuxError::backend("selecting raw input events", err))?
        .check()
        .map_err(|err| LinuxError::reply("selecting raw input events", err))?;
        session
            .conn()
            .flush()
            .map_err(|err| LinuxError::backend("flushing the input subscription", err))?;

        let (sender, events) = crossbeam_channel::bounded(256);
        let state = Arc::new(State {
            stop: AtomicBool::new(false),
            generation: AtomicU64::new(0),
            dropped: AtomicU64::new(0),
        });
        let thread_state = Arc::clone(&state);
        let (ready_tx, ready_rx) = crossbeam_channel::bounded(1);
        let worker = std::thread::Builder::new()
            .name("snow-linux-input".into())
            .spawn(move || {
                snow_core::qos::apply_current_thread();
                let result = run(session, keyboard, mouse, sender, &thread_state, &ready_tx);
                if let Err(error) = result {
                    // A failure after start-up has no channel left to report on,
                    // so it is surfaced the same way a dead server is: the
                    // consumer sees the stream disconnect and stops observing.
                    let _ = ready_tx.try_send(Err(error));
                }
                thread_state.stop.store(true, Ordering::Release);
            })
            .map_err(|err| LinuxError::other(format!("starting the input observer: {err}")))?;

        let observer = Self {
            events,
            state,
            worker: Some(worker),
        };
        ready_rx
            .recv_timeout(Duration::from_secs(3))
            .map_err(|_| LinuxError::other("the input observer did not become ready"))??;
        Ok(observer)
    }

    pub fn generation(&self) -> u64 {
        self.state.generation.load(Ordering::Acquire)
    }

    /// Events dropped because the consumer fell behind.
    pub fn dropped(&self) -> u64 {
        self.state.dropped.load(Ordering::Relaxed)
    }
}

impl Drop for X11InputObserver {
    fn drop(&mut self) {
        self.state.stop.store(true, Ordering::Release);
        if let Some(worker) = self.worker.take() {
            // Bounded by `IDLE_POLL_INTERVAL`, because the worker never blocks
            // indefinitely: see the constant's rationale.
            let _ = worker.join();
        }
    }
}

/// Live state of the observed session.
///
/// The connection is borrowed from the [`X11Session`] the worker owns, so the
/// session outlives every request made through it.
struct Session<'a> {
    conn: &'a RustConnection,
    root: xproto::Window,
    keymap: KeyMap,
    pressed: [u8; 256],
    last_motion: Option<Instant>,
}

impl Session<'_> {
    fn shift(&self) -> bool {
        // Both Shift keys, in the overlay's virtual-key space.
        self.pressed[0xa0] != 0 || self.pressed[0xa1] != 0
    }

    fn alt_gr(&self) -> bool {
        self.pressed[0xa5] != 0
    }

    /// Resolve a keycode against the modifier state in force *now*.
    fn keysym(&self, keycode: u8) -> Option<u32> {
        self.keymap.keysym(keycode, self.shift(), self.alt_gr())
    }

    /// One round-trip that returns both the pointer position and the modifier
    /// mask. `MOD1` and `MOD4` are Alt and Super by the X convention that every
    /// layout follows, so clicks need no separate keyboard query.
    fn pointer(&self) -> LinuxResult<(i32, i32, Modifiers)> {
        let reply = self
            .conn
            .query_pointer(self.root)
            .map_err(|err| LinuxError::backend("querying the pointer", err))?
            .reply()
            .map_err(|err| LinuxError::reply("querying the pointer", err))?;
        let mask = reply.mask;
        Ok((
            i32::from(reply.root_x),
            i32::from(reply.root_y),
            Modifiers {
                control: mask.contains(xproto::KeyButMask::CONTROL),
                alt: mask.contains(xproto::KeyButMask::MOD1),
                shift: mask.contains(xproto::KeyButMask::SHIFT),
                super_key: mask.contains(xproto::KeyButMask::MOD4),
            },
        ))
    }

    /// Rebuild the pressed table from the server's own bitmap.
    ///
    /// Called after a dropped batch: the events carrying those transitions are
    /// gone, so only the server can say which keys are actually held. Without
    /// this, a lost key release would leave a modifier painted on every
    /// subsequent keycap.
    fn resynchronise(&mut self) -> LinuxResult<()> {
        let reply = self
            .conn
            .query_keymap()
            .map_err(|err| LinuxError::backend("resynchronising modifier state", err))?
            .reply()
            .map_err(|err| LinuxError::reply("resynchronising modifier state", err))?;
        keys::pressed_from_keymap(&reply.keys, &self.keymap, &mut self.pressed);
        Ok(())
    }

    /// Re-read the keyboard mapping after a layout change.
    fn reload_keymap(&mut self) {
        let min_keycode = self.conn.setup().min_keycode;
        let max_keycode = self.conn.setup().max_keycode;
        let Some(count) = max_keycode
            .checked_sub(min_keycode)
            .and_then(|span| span.checked_add(1))
        else {
            return;
        };
        let loaded = self
            .conn
            .get_keyboard_mapping(min_keycode, count)
            .ok()
            .and_then(|cookie| cookie.reply().ok())
            .and_then(|reply| KeyMap::new(min_keycode, reply.keysyms_per_keycode, &reply.keysyms));
        // A malformed refresh keeps the previous mapping: a stale label beats a
        // table that cannot be indexed.
        if let Some(refreshed) = loaded {
            self.keymap = refreshed;
        }
    }

    /// Send one observation, widening the generation after a drop.
    fn publish(&mut self, state: &State, sender: &Sender<InputEvent>, event: InputEvent) {
        if sender.try_send(event).is_ok() {
            return;
        }
        // The consumer missed a batch that may have carried a key release. The
        // generation change makes it discard everything older, and the
        // resynchronisation below repairs the held-key table it will see from
        // the next event on.
        state.generation.fetch_add(1, Ordering::AcqRel);
        state.dropped.fetch_add(1, Ordering::Relaxed);
        if self.resynchronise().is_err() {
            // Without the server's view the table may be stuck. Clearing is the
            // conservative repair: a modifier that is wrongly cleared is visible
            // on screen, one that is wrongly held is not.
            self.pressed = [0u8; 256];
        }
    }
}

/// Worker body: own the connection and pump events until told to stop.
fn run(
    session: X11Session,
    keyboard: bool,
    mouse: bool,
    sender: Sender<InputEvent>,
    state: &State,
    ready: &Sender<LinuxResult<()>>,
) -> LinuxResult<()> {
    let min_keycode = session.conn().setup().min_keycode;
    let max_keycode = session.conn().setup().max_keycode;
    let count = max_keycode
        .checked_sub(min_keycode)
        .and_then(|span| span.checked_add(1))
        .ok_or_else(|| LinuxError::other("the server reported an empty keycode range"))?;
    let reply = session
        .conn()
        .get_keyboard_mapping(min_keycode, count)
        .map_err(|err| LinuxError::backend("reading the keyboard mapping", err))?
        .reply()
        .map_err(|err| LinuxError::reply("reading the keyboard mapping", err))?;
    let keymap = KeyMap::new(min_keycode, reply.keysyms_per_keycode, &reply.keysyms)
        .ok_or_else(|| LinuxError::other("the server's keyboard mapping is malformed"))?;

    let mut live = Session {
        conn: session.conn(),
        root: session.root(),
        keymap,
        pressed: [0u8; 256],
        last_motion: None,
    };
    live.resynchronise()?;

    let _ = ready.try_send(Ok(()));

    if mouse {
        // Publish the starting pointer state so the consumer learns whether the
        // pointer begins inside the recorded region, instead of assuming it was
        // outside until the first movement. The Windows hook does the same.
        let (x, y, modifiers) = live.pointer()?;
        live.last_motion = Some(Instant::now());
        let pressed = live.pressed;
        live.publish(
            state,
            &sender,
            InputEvent {
                kind: InputEventKind::Motion,
                keycode: 0,
                keysym: None,
                button: 0,
                x,
                y,
                modifiers,
                pressed,
                server_time: 0,
                generation: state.generation.load(Ordering::Acquire),
            },
        );
    }

    while !state.stop.load(Ordering::Acquire) {
        let mut handled = false;
        loop {
            let event = match live.conn.poll_for_event() {
                Ok(Some(event)) => event,
                Ok(None) => break,
                // A broken connection cannot be recovered in place, and the
                // consumer must not keep an observer that will never report
                // again.
                Err(error) => {
                    return Err(LinuxError::other(format!(
                        "the input event stream ended: {error}"
                    )));
                }
            };
            handled = true;
            observe(&mut live, state, &sender, event, keyboard, mouse)?;
        }
        if !handled {
            std::thread::sleep(IDLE_POLL_INTERVAL);
        }
    }

    Ok(())
}

/// Translate one server event into an observation and publish it.
fn observe(
    live: &mut Session<'_>,
    state: &State,
    sender: &Sender<InputEvent>,
    event: Event,
    keyboard: bool,
    mouse: bool,
) -> LinuxResult<()> {
    let generation = state.generation.load(Ordering::Acquire);
    match event {
        // A layout switch rewrites the mapping for every keycode; labels would
        // otherwise keep describing the previous layout.
        Event::MappingNotify(_) => live.reload_keymap(),
        Event::XinputRawKeyPress(raw) if keyboard => {
            let keycode = u8::try_from(raw.detail).unwrap_or(u8::MAX);
            let Some(keysym) = live.keysym(keycode) else {
                // No symbol in the mapping means there is no keycap to draw.
                return Ok(());
            };
            // Held state first: the symbol is resolved against the modifiers in
            // force *including* this key, which is what the overlay reads.
            keys::apply(&mut live.pressed, keysym, true);
            let pressed = live.pressed;
            live.publish(
                state,
                sender,
                InputEvent {
                    kind: InputEventKind::KeyPress,
                    keycode,
                    keysym: Some(keysym),
                    button: 0,
                    x: 0,
                    y: 0,
                    modifiers: tracking::modifiers_from_pressed(&pressed),
                    pressed,
                    server_time: raw.time,
                    generation,
                },
            );
        }
        Event::XinputRawKeyRelease(raw) if keyboard => {
            let keycode = u8::try_from(raw.detail).unwrap_or(u8::MAX);
            let keysym = live.keysym(keycode);
            let Some(keysym) = keysym else {
                return Ok(());
            };
            keys::apply(&mut live.pressed, keysym, false);
            let pressed = live.pressed;
            live.publish(
                state,
                sender,
                InputEvent {
                    kind: InputEventKind::KeyRelease,
                    keycode,
                    keysym: Some(keysym),
                    button: 0,
                    x: 0,
                    y: 0,
                    modifiers: tracking::modifiers_from_pressed(&pressed),
                    pressed,
                    server_time: raw.time,
                    generation,
                },
            );
        }
        Event::XinputRawButtonPress(raw) if mouse => {
            let (x, y, modifiers) = live.pointer()?;
            let pressed = live.pressed;
            live.publish(
                state,
                sender,
                InputEvent {
                    kind: InputEventKind::ButtonPress,
                    keycode: 0,
                    keysym: None,
                    button: u8::try_from(raw.detail).unwrap_or(u8::MAX),
                    x,
                    y,
                    modifiers,
                    pressed,
                    server_time: raw.time,
                    generation,
                },
            );
        }
        Event::XinputRawButtonRelease(raw) if mouse => {
            let (x, y, modifiers) = live.pointer()?;
            let pressed = live.pressed;
            live.publish(
                state,
                sender,
                InputEvent {
                    kind: InputEventKind::ButtonRelease,
                    keycode: 0,
                    keysym: None,
                    button: u8::try_from(raw.detail).unwrap_or(u8::MAX),
                    x,
                    y,
                    modifiers,
                    pressed,
                    server_time: raw.time,
                    generation,
                },
            );
        }
        Event::XinputRawMotion(raw) if mouse => {
            let now = Instant::now();
            if live
                .last_motion
                .is_some_and(|last| now.duration_since(last) < MOTION_SAMPLE_INTERVAL)
            {
                // Coalesced: the consumer keeps only the newest movement, so
                // sampling faster would add round-trips without adding
                // information.
                return Ok(());
            }
            live.last_motion = Some(now);
            let (x, y, modifiers) = live.pointer()?;
            let pressed = live.pressed;
            live.publish(
                state,
                sender,
                InputEvent {
                    kind: InputEventKind::Motion,
                    keycode: 0,
                    keysym: None,
                    button: 0,
                    x,
                    y,
                    modifiers,
                    pressed,
                    server_time: raw.time,
                    generation,
                },
            );
        }
        _ => {}
    }
    Ok(())
}
