//! Linux selected-text backend.
//!
//! On X11 the selection of the focused window *is* the PRIMARY selection: the
//! owner publishes it and any client may read it. Capture is therefore passive --
//! no synthetic `Ctrl+C`, no clipboard snapshot, no restoration afterwards -- so
//! the only truthful clipboard status to report is `Unchanged`.
//!
//! `Wayland` has no equivalent that a background client may read: the compositor
//! owns the clipboard and only exposes it through the data-device protocol while
//! this process has focus. Such a session reports `UnsupportedPlatform` rather
//! than an empty selection.

mod selection;
mod source;

use std::time::Instant;

use crate::model::*;
use crate::policy::{Backend, Context, Probe};

use self::selection::{PrimaryRead, Reader};

pub(crate) struct Worker {
    reader: Reader,
}

impl Worker {
    pub(crate) fn new() -> Result<Self, SelectionError> {
        Ok(Self {
            reader: Reader::connect()?,
        })
    }
}

impl Backend for Worker {
    fn passive_method(&self) -> RetrievalMethod {
        // Reported honestly: the text came from PRIMARY, not from an accessibility
        // API, so a caller that branches on the method can tell what was promised.
        RetrievalMethod::Primary
    }

    fn validate_target(&mut self, _: &Context) -> Result<(), SelectionError> {
        // PRIMARY belongs to the session, not to a window that can change while a
        // stage runs, so there is nothing to re-check between stages.
        Ok(())
    }

    fn accessibility(
        &mut self,
        context: &Context,
        deadline: Instant,
    ) -> Result<Probe, SelectionError> {
        context.check()?;
        Ok(match self.reader.read(context, deadline)? {
            PrimaryRead::Text(text) => Probe::Selected(vec![SelectedRange {
                text,
                // Bounds come from the accessibility tree on the other platforms.
                // X11 core has no such geometry, and the contract says geometry is
                // optional and never required for a text result.
                bounds: Vec::new(),
            }]),
            PrimaryRead::NoOwner => Probe::Empty,
            PrimaryRead::NoSupportedTarget => Probe::Unsupported,
        })
    }

    fn native_control(&mut self, _: &Context, _: Instant) -> Result<Probe, SelectionError> {
        // Nothing in the X11 core protocol describes control text. AT-SPI2 over
        // D-Bus would be the route and is not implemented yet; see
        // `docs-uos-linux-port.md` 9.4.22.
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
    // Never fails by design: PRIMARY is session-wide, so an unnameable focus must
    // not turn a perfectly readable selection into an error.
    Ok(source::capture())
}
