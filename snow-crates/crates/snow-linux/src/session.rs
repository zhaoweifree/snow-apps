//! Environment-based session detection.
//!
//! This module only reads environment variables, so it compiles and is
//! testable on every host.

/// Which display server a session is running under.
///
/// UOS 20 defaults to X11 while Deepin 23 and later increasingly default to
/// Wayland; the two need different capture and input backends, so the choice is
/// made once here and consumed by the platform layers.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SessionKind {
    X11,
    Wayland,
    /// Neither `DISPLAY` nor `WAYLAND_DISPLAY` is set: a headless/CI host.
    Headless,
}

impl SessionKind {
    /// Classify the current process environment.
    pub fn detect() -> Self {
        classify_session(
            std::env::var("XDG_SESSION_TYPE").ok().as_deref(),
            has_env("WAYLAND_DISPLAY"),
            has_env("DISPLAY"),
        )
    }

    /// Whether an X11 connection is worth attempting.
    pub fn has_x11(self) -> bool {
        matches!(self, Self::X11)
    }

    pub const fn is_wayland(self) -> bool {
        matches!(self, Self::Wayland)
    }
}

/// The session-kind decision table, with the environment passed in explicitly.
///
/// `XDG_SESSION_TYPE` wins when it names a display server, because that is what
/// the display manager recorded. Otherwise `WAYLAND_DISPLAY` is checked before
/// `DISPLAY`: an XWayland session exports both, and the Wayland socket is the
/// one that constrains screen capture.
pub fn classify_session(
    xdg_session_type: Option<&str>,
    wayland_display: bool,
    display: bool,
) -> SessionKind {
    match xdg_session_type
        .unwrap_or_default()
        .trim()
        .to_ascii_lowercase()
        .as_str()
    {
        "x11" => return SessionKind::X11,
        "wayland" => return SessionKind::Wayland,
        _ => {}
    }

    if wayland_display {
        SessionKind::Wayland
    } else if display {
        SessionKind::X11
    } else {
        SessionKind::Headless
    }
}

fn has_env(key: &str) -> bool {
    std::env::var_os(key).is_some_and(|value| !value.is_empty())
}

#[cfg(test)]
mod tests {
    use super::{SessionKind, classify_session};

    #[test]
    fn explicit_session_type_wins_over_inherited_socket() {
        assert_eq!(classify_session(Some("x11"), true, true), SessionKind::X11);
        assert_eq!(
            classify_session(Some("wayland"), true, true),
            SessionKind::Wayland
        );
        // Display managers are inconsistent about casing and stray whitespace.
        assert_eq!(
            classify_session(Some("  WayLand "), true, true),
            SessionKind::Wayland
        );
    }

    #[test]
    fn wayland_socket_outranks_xwayland_display() {
        assert_eq!(classify_session(None, true, true), SessionKind::Wayland);
        assert_eq!(classify_session(None, false, true), SessionKind::X11);
        assert_eq!(classify_session(None, false, false), SessionKind::Headless);
    }

    #[test]
    fn unknown_session_type_falls_back_to_sockets() {
        // "tty" is what a bare console reports; it must not mask a live socket.
        assert_eq!(classify_session(Some("tty"), false, true), SessionKind::X11);
        assert_eq!(
            classify_session(Some("tty"), true, false),
            SessionKind::Wayland
        );
        assert_eq!(
            classify_session(Some(""), false, false),
            SessionKind::Headless
        );
    }

    #[test]
    fn headless_is_not_mistaken_for_x11() {
        assert!(!SessionKind::Headless.has_x11());
        assert!(SessionKind::X11.has_x11());
        assert!(SessionKind::Wayland.is_wayland());
    }
}
