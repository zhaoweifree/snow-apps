//! Global mouse drags over passive button grabs.
//!
//! # Why a grab and not an observer
//!
//! The feature swallows the press it starts from: the application under the
//! pointer must not begin a drag of its own when the user holds a chord that
//! belongs to this program. Observation cannot do that -- by the time an
//! observer sees a press, the application has it -- and XInput2's raw events are
//! deliberately delivery-neutral, so they carry no ability to consume. A passive
//! `XGrabButton`, by contrast, is a promise the server keeps: that button with
//! exactly that modifier state goes to this client and to nobody else.
//!
//! The server therefore does the matching, and this module only has to hold the
//! right set of grabs. That is also why a press that reaches the event loop is
//! always an activation: there is nothing to re-decide, and nothing to replay.
//!
//! # Shape of the event stream
//!
//! While a passive grab is active, `owner_events: false` and an event mask of
//! press/release/motion means every button event and every pointer move in the
//! session is delivered here, with `grab_window` (the root) as the event window.
//! Pointer motion stays asynchronous (`pointer_mode: ASYNC`), so the cursor still
//! moves while the drag is in progress -- the drag simply belongs to this process
//! instead of the application underneath.
//!
//! # Limits
//!
//! The keyboard's modifier tables are read once, when the service starts: a
//! layout change that moves Alt to another X11 modifier leaves the grabs holding
//! the old bit until the service is recreated. Re-reading them inside the event
//! loop would mean a round-trip per `MappingNotify`, and the hotkey service makes
//! the same trade.
//!
//! # Why its own connection and thread
//!
//! Same reasoning as [`super::hotkey`]: grabs are per-connection, their events
//! arrive on that connection, and the capture session's connection spends most of
//! its time blocked inside image round-trips.

use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::Duration;

use crossbeam_channel::{Receiver, Sender};
use x11rb::connection::{Connection, RequestConnection};
use x11rb::protocol::Event;
use x11rb::protocol::xproto::{
    ButtonIndex, EventMask, GrabButtonRequest, GrabMode, ModMask, UngrabButtonRequest, Window,
};
use x11rb::rust_connection::RustConnection;

use crate::error::LinuxResult;
use crate::globalmouse::{self, Binding};
use crate::x11::X11Session;
use crate::x11::keymap::{Keymap, MODIFIER_STATE_MASK};

/// How long the worker sleeps when there is neither a command nor an event.
///
/// One interval is also the worst-case latency between a pointer move arriving
/// and the overlay hearing about it, which is why it is one frame and not the
/// ten-times-a-second idle cost of a coarser loop.
const IDLE_POLL_INTERVAL: Duration = Duration::from_millis(10);

/// How long a caller waits for the worker to answer a configuration change.
const COMMAND_TIMEOUT: Duration = Duration::from_secs(2);

/// The phase of a drag, as reported to the caller.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DragPhase {
    /// A matching press started the drag. `id` is new and identifies it.
    Begin,
    /// The pointer moved while the drag is in progress.
    Update,
    /// The button was released; this is the last event for `id`.
    Finish,
    /// The caller gave up on `id`; no further events follow for it.
    Cancel,
}

impl DragPhase {
    /// The value used on the C boundary.
    pub const fn as_i32(self) -> i32 {
        match self {
            Self::Begin => 0,
            Self::Update => 1,
            Self::Finish => 2,
            Self::Cancel => 3,
        }
    }
}

/// One drag event, in root-window pixels.
#[derive(Clone, Copy, Debug)]
pub struct DragEvent {
    /// Identifies the drag. Monotonic within one service instance.
    pub id: u64,
    /// The action id of the binding that started it.
    pub action: i32,
    pub x: i32,
    pub y: i32,
}

/// Called on the worker thread for every drag event.
///
/// The handler must not call back into [`GlobalMouseService`]: the configuration
/// calls wait for this same thread to answer. The C++ side posts to the Qt
/// thread instead.
pub type DragHandler = Box<dyn Fn(DragPhase, DragEvent) + Send + 'static>;

enum Command {
    SetBindings {
        bindings: Vec<Binding>,
        reply: Sender<usize>,
    },
    SetEnabled {
        enabled: bool,
        reply: Sender<usize>,
    },
    Cancel {
        id: u64,
    },
    Stop,
}

/// The drag currently owning the pointer, if any.
struct Active {
    id: u64,
    button: u8,
    action: i32,
    /// Set when the caller cancelled: the drag stops reporting, but the button
    /// is still held, so the pointer stays grabbed and further presses must not
    /// start a second drag.
    cancelled: bool,
}

struct State {
    bindings: Vec<Binding>,
    /// Whether a capture can start right now. False while another capture owns
    /// the screen: holding grabs then would swallow clicks for a stale reason.
    enabled: bool,
    /// The grabs this service currently holds.
    installed: Vec<globalmouse::Grab>,
    active: Option<Active>,
    next_id: u64,
}

/// A running grab set for global mouse drags.
pub struct GlobalMouseService {
    commands: Sender<Command>,
    stopped: Arc<AtomicBool>,
    worker: Option<std::thread::JoinHandle<()>>,
}

impl std::fmt::Debug for GlobalMouseService {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("GlobalMouseService")
            .field("stopped", &self.stopped.load(Ordering::Acquire))
            .finish_non_exhaustive()
    }
}

impl GlobalMouseService {
    /// Connects and starts the worker.
    ///
    /// Fails only when no X server is reachable or its keyboard tables cannot be
    /// read.
    pub fn start(handler: impl Fn(DragPhase, DragEvent) + Send + 'static) -> LinuxResult<Self> {
        let session = X11Session::connect()?;
        let keymap = Keymap::read(session.conn())?;
        let root = session.root();
        let conn = session.into_connection();

        let (commands, receiver) = crossbeam_channel::unbounded();
        let stopped = Arc::new(AtomicBool::new(false));
        let thread_state = Arc::clone(&stopped);
        let worker = std::thread::Builder::new()
            .name("snow-linux-global-mouse".into())
            .spawn(move || {
                snow_core::qos::apply_current_thread();
                let handler: DragHandler = Box::new(handler);
                run(conn, root, keymap, handler, receiver);
                thread_state.store(true, Ordering::Release);
            })
            .map_err(|err| {
                crate::error::LinuxError::other(format!("starting the mouse service: {err}"))
            })?;

        Ok(Self {
            commands,
            stopped,
            worker: Some(worker),
        })
    }

    /// Replaces the binding set. Returns how many grabs the server accepted.
    ///
    /// A grab can be refused because another client holds it -- a window manager
    /// grabbing `Super`-plus-drag is the common case -- and one refused grab must
    /// not cost the others: the answer is the count that worked, and the caller
    /// can compare it to the number of configured chords if it wants to warn.
    pub fn set_bindings(&self, bindings: Vec<Binding>) -> Option<usize> {
        let (reply, answers) = crossbeam_channel::bounded(1);
        self.commands
            .send(Command::SetBindings { bindings, reply })
            .ok()?;
        answers.recv_timeout(COMMAND_TIMEOUT).ok()
    }

    /// Enables or disables the grabs without changing the bindings.
    ///
    /// Disabled means "hold nothing": an ungrabbed configuration is the only way
    /// to be sure a press is not swallowed while a capture cannot start.
    pub fn set_enabled(&self, enabled: bool) -> Option<usize> {
        let (reply, answers) = crossbeam_channel::bounded(1);
        self.commands
            .send(Command::SetEnabled { enabled, reply })
            .ok()?;
        answers.recv_timeout(COMMAND_TIMEOUT).ok()
    }

    /// Stops reporting the drag `id`.
    ///
    /// The button is still held, so the pointer remains grabbed until it is
    /// released; the drag simply goes quiet in the meantime. This mirrors what
    /// the shared gesture logic does on the platforms where the process sees
    /// every event: cancel clears the active id and the release that follows is
    /// still consumed.
    pub fn cancel(&self, id: u64) {
        let _ = self.commands.send(Command::Cancel { id });
    }

    pub fn stopped(&self) -> bool {
        self.stopped.load(Ordering::Acquire)
    }
}

impl Drop for GlobalMouseService {
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
    root: Window,
    keymap: Keymap,
    handler: DragHandler,
    commands: Receiver<Command>,
) {
    let mut state = State {
        bindings: Vec::new(),
        enabled: false,
        installed: Vec::new(),
        active: None,
        next_id: 0,
    };

    loop {
        match commands.recv_timeout(IDLE_POLL_INTERVAL) {
            Ok(Command::SetBindings { bindings, reply }) => {
                state.bindings = bindings;
                let installed = reinstall(&conn, root, &keymap, &mut state);
                let _ = reply.send(installed);
            }
            Ok(Command::SetEnabled { enabled, reply }) => {
                state.enabled = enabled;
                let installed = reinstall(&conn, root, &keymap, &mut state);
                let _ = reply.send(installed);
            }
            Ok(Command::Cancel { id }) => {
                if let Some(active) = state.active.as_mut()
                    && active.id == id
                {
                    active.cancelled = true;
                }
            }
            Ok(Command::Stop) | Err(crossbeam_channel::RecvTimeoutError::Disconnected) => break,
            Err(crossbeam_channel::RecvTimeoutError::Timeout) => {}
        }

        loop {
            match conn.poll_for_event() {
                Ok(Some(event)) => handle_event(&mut state, &handler, event),
                Ok(None) => break,
                // A dead connection cannot be revived in place; the service keeps
                // running but stops producing drags.
                Err(_) => return,
            }
        }
    }

    state.enabled = false;
    let _ = reinstall(&conn, root, &keymap, &mut state);
    let _ = conn.flush();
}

fn handle_event(state: &mut State, handler: &DragHandler, event: Event) {
    match event {
        Event::ButtonPress(event) => {
            // While a drag is active the pointer is grabbed, so this press is
            // ours by definition: the application underneath must not see it,
            // and it must not start a second drag either.
            if state.active.is_some() {
                return;
            }
            let mask = u16::from(event.state) & MODIFIER_STATE_MASK;
            let Some(grab) = state
                .installed
                .iter()
                .find(|grab| grab.button == event.detail && grab.mask == mask)
            else {
                // Reachable when the grab set changed under a press that the
                // server had already delivered; dropping it is the only option,
                // since replaying would need the grab to still be active.
                return;
            };
            state.next_id = state.next_id.wrapping_add(1);
            let id = state.next_id;
            state.active = Some(Active {
                id,
                button: event.detail,
                action: grab.action,
                cancelled: false,
            });
            handler(
                DragPhase::Begin,
                DragEvent {
                    id,
                    action: grab.action,
                    x: i32::from(event.root_x),
                    y: i32::from(event.root_y),
                },
            );
        }
        Event::MotionNotify(event) => {
            let Some(active) = state.active.as_ref() else {
                return;
            };
            if active.cancelled {
                return;
            }
            handler(
                DragPhase::Update,
                DragEvent {
                    id: active.id,
                    action: active.action,
                    x: i32::from(event.root_x),
                    y: i32::from(event.root_y),
                },
            );
        }
        Event::ButtonRelease(event) => {
            let Some(active) = state.active.as_ref() else {
                return;
            };
            if event.detail != active.button {
                return;
            }
            let drag = DragEvent {
                id: active.id,
                action: active.action,
                x: i32::from(event.root_x),
                y: i32::from(event.root_y),
            };
            let cancelled = active.cancelled;
            state.active = None;
            if !cancelled {
                handler(DragPhase::Finish, drag);
            }
        }
        _ => {}
    }
}

/// Drops the current grabs and installs the ones the configuration asks for.
///
/// Returns the number of grabs the server accepted.
fn reinstall(conn: &RustConnection, root: Window, keymap: &Keymap, state: &mut State) -> usize {
    for grab in state.installed.drain(..) {
        let _ = conn.send_trait_request_without_reply(UngrabButtonRequest {
            button: ButtonIndex::from(grab.button),
            grab_window: root,
            modifiers: ModMask::from(grab.mask),
        });
    }
    if !state.enabled {
        let _ = conn.flush();
        return 0;
    }

    let wanted = globalmouse::grabs(
        &state.bindings,
        |bits| keymap.mask_bits(bits),
        keymap.lock_masks(),
    );
    for grab in wanted {
        if grab_button(conn, root, &grab) {
            state.installed.push(grab);
        }
    }
    let _ = conn.flush();
    state.installed.len()
}

fn grab_button(conn: &RustConnection, root: Window, grab: &globalmouse::Grab) -> bool {
    let request = GrabButtonRequest {
        // The events must reach this connection, not the window under the
        // pointer: acting only while this process has focus is the opposite of
        // what a global gesture is for.
        owner_events: false,
        grab_window: root,
        // Motion has to be part of the grab: once activated, the pointer belongs
        // to this client, so without it the overlay would see a press and a
        // release with nothing in between.
        event_mask: EventMask::BUTTON_PRESS | EventMask::BUTTON_RELEASE | EventMask::POINTER_MOTION,
        // Neither the pointer nor the keyboard is frozen while the grab is held:
        // the cursor must keep moving, and the overlay still receives its keys.
        pointer_mode: GrabMode::ASYNC,
        keyboard_mode: GrabMode::ASYNC,
        confine_to: x11rb::NONE,
        cursor: x11rb::NONE,
        button: ButtonIndex::from(grab.button),
        modifiers: ModMask::from(grab.mask),
    };
    // A refused grab is a normal answer, not a failure: `BadAccess` means another
    // client already owns that chord, which is how a window manager's own
    // Super+drag keeps working.
    conn.send_trait_request_without_reply(request)
        .map(|cookie| cookie.check().is_ok())
        .unwrap_or(false)
}
