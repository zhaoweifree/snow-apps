use snow_core::error::{Classify, ErrorClass};

/// Failures raised by the Linux platform backend.
///
/// `SessionMissing` is deliberately distinct from the backend variants: it
/// describes a host that is reachable but unattended (no `DISPLAY`, refused
/// socket), which callers should treat as "feature unavailable" rather than
/// "broken".
#[derive(Debug, thiserror::Error)]
pub enum LinuxError {
    #[error("no display server is reachable (DISPLAY unset or refused)")]
    SessionMissing,

    #[cfg(target_os = "linux")]
    #[error("{context}: {source}")]
    Backend {
        context: &'static str,
        #[source]
        source: x11rb::errors::ConnectionError,
    },

    #[cfg(target_os = "linux")]
    #[error("{context}: {source}")]
    Reply {
        context: &'static str,
        #[source]
        source: x11rb::errors::ReplyError,
    },

    #[error("{0}")]
    Other(String),

    /// The server's packed pixel layout cannot be decoded exactly.
    ///
    /// Distinct from [`LinuxError::Other`] because no retry will change it: a
    /// depth-8 `PseudoColor` root or a discontiguous colour mask is a property
    /// of the X server's configuration, so callers should report a capture that
    /// this backend cannot serve rather than a transient failure.
    #[error("the X server's pixel layout is not supported: {reason}")]
    UnsupportedPixelLayout { reason: &'static str },

    /// The session cannot observe global input at all.
    ///
    /// Distinct from [`LinuxError::Other`] for the same reason: a server without
    /// XInput2, or a Wayland session with no remote-desktop portal, will not grow
    /// the capability by being retried, and reporting it as transient would make
    /// the recorder promise keyboard and mouse effects it can never deliver.
    #[error("global input observation is not available: {reason}")]
    UnsupportedInputBackend { reason: &'static str },
}

#[cfg(target_os = "linux")]
impl LinuxError {
    pub(crate) fn backend(context: &'static str, source: x11rb::errors::ConnectionError) -> Self {
        Self::Backend { context, source }
    }

    pub(crate) fn reply(context: &'static str, source: x11rb::errors::ReplyError) -> Self {
        Self::Reply { context, source }
    }
}

impl LinuxError {
    pub fn other(message: impl Into<String>) -> Self {
        Self::Other(message.into())
    }
}

impl Classify for LinuxError {
    fn class(&self) -> ErrorClass {
        match self {
            // An unreachable session is an environment problem, not a fault:
            // retrying in-process will never fix it.
            Self::SessionMissing
            | Self::UnsupportedPixelLayout { .. }
            | Self::UnsupportedInputBackend { .. } => ErrorClass::InvalidConfig,
            Self::Other(_) => ErrorClass::Transient,
            #[cfg(target_os = "linux")]
            Self::Backend { .. } | Self::Reply { .. } => ErrorClass::Transient,
        }
    }
}

pub type LinuxResult<T> = Result<T, LinuxError>;

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn missing_session_is_an_environment_problem() {
        assert_eq!(
            LinuxError::SessionMissing.class(),
            ErrorClass::InvalidConfig
        );
    }

    #[test]
    fn transient_backend_failures_are_retryable() {
        assert_eq!(LinuxError::other("boom").class(), ErrorClass::Transient);
    }

    #[test]
    fn a_session_that_cannot_observe_input_is_a_configuration_problem() {
        // Reporting this as transient would let the recorder promise keyboard
        // and mouse effects it can never deliver.
        let error = LinuxError::UnsupportedInputBackend {
            reason: "no XInput2",
        };
        assert_eq!(error.class(), ErrorClass::InvalidConfig);
    }
}
