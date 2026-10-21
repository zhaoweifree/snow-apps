//! Linux selected-text backend.
//!
//! Two routes, in order:
//!
//! 1. **AT-SPI2**, through the accessibility bus: the focused widget's own text
//!    selection, with the rectangle it is drawn in. This is the only route that
//!    can report geometry, and the only one that can tell *which widget* the
//!    text came from.
//! 2. **PRIMARY**, through the X server: whatever was selected last in the
//!    session. Readable even when accessibility is switched off, and still the
//!    answer for terminals and toolkits that publish no widget tree -- but it has
//!    no geometry, and it belongs to the session rather than to a window.
//!
//! Both are passive: no synthetic `Ctrl+C`, no clipboard snapshot, no
//! restoration afterwards, so the only truthful clipboard status to report is
//! `Unchanged`.
//!
//! `Wayland` has no equivalent that a background client may read: the compositor
//! owns the clipboard and only exposes it through the data-device protocol while
//! this process has focus. Such a session reports `UnsupportedPlatform` rather
//! than an empty selection.

mod selection;
mod source;

use std::cell::Cell;
use std::time::{Duration, Instant};

use crate::model::*;
use crate::policy::{Backend, Context, Probe};
use crate::selection::{Accessibility, Answer, accessibility_answered, choose_answer};

use self::selection::{PrimaryRead, Reader};

/// How much of one accessibility stage the AT-SPI2 route may spend.
///
/// The stage budget is 600 ms (`policy::acquire`). The selection read is what
/// worked before this route existed, so it keeps the larger share: AT-SPI2 gets
/// the first 350 ms and the selection the rest. AT-SPI2 answering is not the
/// common case, so on the paths that matter this split costs nothing -- and on
/// the paths where it does matter, the fallback must not be the thing that runs
/// out of time.
const ATSPI_BUDGET: Duration = Duration::from_millis(350);

pub(crate) struct Worker {
    reader: Reader,
    /// The accessibility bus, connected on first use.
    ///
    /// The connection is attempted once and the outcome is kept: a desktop with
    /// accessibility switched off answers the same way for the life of the
    /// process, and a failed handshake is not free. `None` therefore means
    /// "tried and unavailable" as much as "not tried yet" -- `bus_attempted`
    /// tells the two apart.
    bus: Option<snow_atspi::AccessibilityBus>,
    bus_attempted: bool,
    /// The route the last passive stage reported through.
    ///
    /// `Backend::passive_method` takes `&self` and is asked *after* the stage
    /// ran, so the answer cannot be a parameter. Only the passive stage writes
    /// this, and only for the answer it is about to return; a stale value can
    /// only be read by a second call, which cannot happen -- the worker is
    /// `&mut self` for the whole stage.
    last_method: Cell<RetrievalMethod>,
}

impl Worker {
    pub(crate) fn new() -> Result<Self, SelectionError> {
        Ok(Self {
            reader: Reader::connect()?,
            bus: None,
            bus_attempted: false,
            last_method: Cell::new(RetrievalMethod::Primary),
        })
    }

    /// The accessibility route, or `None` when it is unavailable or said nothing.
    ///
    /// Never an error. This route is an enhancement, and a failure in it must
    /// not take down a selection read that already worked -- the caller cannot
    /// act on "AT-SPI2 broke" any differently from "AT-SPI2 has nothing".
    fn read_accessibility(
        &mut self,
        context: &Context,
        deadline: Instant,
    ) -> Option<Accessibility> {
        let pid = context.source.process_id;
        // Without a pid there is no application to ask. A zero here means the
        // window did not publish `_NET_WM_PID`, not that process 0 has focus.
        if pid == 0 {
            return None;
        }
        if !self.bus_attempted {
            self.bus_attempted = true;
            self.bus = snow_atspi::AccessibilityBus::connect().ok();
        }
        let reading = self
            .bus
            .as_ref()?
            .read_selection(pid, Some(deadline))
            .ok()??;
        // The rectangle stays in the accessibility crate's own units here; the
        // conversion to this crate's `SelectionRect` happens in `choose_answer`,
        // which is unit tested on a host without a desktop bus.
        Some(Accessibility {
            text: reading.text,
            bounds: reading.bounds,
        })
    }
}

/// The answer as the vocabulary of this crate: one range, no merging.
///
/// One range, not several: both routes describe a single selection of a single
/// focus. Reporting it as a list would imply the caller could see the pieces.
fn probe(answer: Answer) -> Probe {
    Probe::Selected(vec![SelectedRange {
        text: answer.text,
        bounds: answer.bounds,
    }])
}

impl Backend for Worker {
    fn passive_method(&self) -> RetrievalMethod {
        // Reported honestly: which of the two routes answered, not which stage
        // ran. A caller that branches on the method can tell what was promised,
        // and geometry is only ever promised by the accessibility route.
        self.last_method.get()
    }

    fn validate_target(&mut self, _: &Context) -> Result<(), SelectionError> {
        // Both routes belong to the session rather than to a window that can
        // change while a stage runs, so there is nothing to re-check between
        // stages.
        Ok(())
    }

    fn accessibility(
        &mut self,
        context: &Context,
        deadline: Instant,
    ) -> Result<Probe, SelectionError> {
        context.check()?;

        let atspi_deadline = context.stage_deadline(ATSPI_BUDGET).min(deadline);
        let accessibility = self.read_accessibility(context, atspi_deadline);

        // Skipping the selection read when the accessibility route answered is
        // what keeps the stage inside its budget: that read costs several round
        // trips and could only confirm the same text.
        let primary = if accessibility_answered(accessibility.as_ref()) {
            None
        } else {
            match self.reader.read(context, deadline)? {
                PrimaryRead::Text(text) => Some(text),
                // Neither "no owner" nor "no supported target" is text. The
                // first leaves the choice between routes to `choose_answer`,
                // which will report "nothing selected"; the second says the
                // owner is there but speaks only a target this crate cannot
                // decode, which is unsupported rather than empty.
                PrimaryRead::NoOwner => None,
                PrimaryRead::NoSupportedTarget => return Ok(Probe::Unsupported),
            }
        };

        match choose_answer(accessibility, primary) {
            Some(answer) => {
                self.last_method.set(answer.method);
                Ok(probe(answer))
            }
            None => Ok(Probe::Empty),
        }
    }

    fn native_control(&mut self, _: &Context, _: Instant) -> Result<Probe, SelectionError> {
        // Nothing in AT-SPI2 describes a *native control* in the sense this
        // strategy means on Windows (the message returns of an edit control):
        // every toolkit's text widget is an accessibility text node, which is
        // what the passive stage already reads.
        Ok(Probe::Unsupported)
    }

    fn copy(&mut self, _: &Context) -> CaptureResult {
        // "Copy" would mean injecting `Ctrl+C` into the focused window. The
        // passive read above already returns the selection, so an explicit
        // Clipboard strategy is answered as unsupported instead of synthesising
        // input that may land in the wrong window.
        Ok(SelectionOutcome::Unsupported)
    }
}

pub(crate) fn capture_source() -> Result<SourceApplication, SelectionError> {
    // Never fails by design: both routes are session-wide, so an unnameable
    // focus must not turn a perfectly readable selection into an error.
    Ok(source::capture())
}
