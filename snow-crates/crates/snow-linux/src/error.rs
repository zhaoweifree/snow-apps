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
            Self::SessionMissing => ErrorClass::InvalidConfig,
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
        assert_eq!(LinuxError::SessionMissing.class(), ErrorClass::InvalidConfig);
    }

    #[test]
    fn transient_backend_failures_are_retryable() {
        assert_eq!(LinuxError::other("boom").class(), ErrorClass::Transient);
    }
}
