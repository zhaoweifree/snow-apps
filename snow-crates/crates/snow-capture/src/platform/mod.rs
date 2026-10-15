use std::sync::Arc;

#[cfg(not(any(target_os = "windows", target_os = "macos", target_os = "linux")))]
use crate::backend::MonitorCapturer;
use crate::backend::{AutoBackendPolicy, CaptureBackend, CaptureBackendKind, CaptureMode};
#[cfg(target_os = "linux")]
use crate::error::CaptureError;
#[cfg(not(any(target_os = "windows", target_os = "macos", target_os = "linux")))]
use crate::error::{CaptureError, CaptureResult};
#[cfg(not(any(target_os = "windows", target_os = "macos", target_os = "linux")))]
use crate::monitor::MonitorId;
use crate::region::MonitorLayout;
#[cfg(not(any(target_os = "windows", target_os = "macos", target_os = "linux")))]
use crate::window::WindowId;

#[cfg(target_os = "windows")]
pub(crate) mod windows;

/// Backend names the Linux build refuses when a caller asks for them
/// explicitly, instead of silently substituting X11.
///
/// A substituted backend would produce frames from somewhere the caller did not
/// ask for, so the request fails and the message names the platform.
#[cfg(target_os = "linux")]
fn unsupported_backend_error(kind: CaptureBackendKind) -> CaptureError {
    CaptureError::BackendUnavailable(format!(
        "the '{}' backend is unavailable on Linux",
        kind.as_str()
    ))
}

#[cfg(target_os = "windows")]
pub(crate) fn build_backend(
    kind: CaptureBackendKind,
    auto_policy: AutoBackendPolicy,
    auto_policy_is_explicit: bool,
) -> crate::error::CaptureResult<Arc<dyn CaptureBackend>> {
    build_backend_for_mode(
        kind,
        auto_policy,
        auto_policy_is_explicit,
        CaptureMode::Continuous,
    )
}

#[cfg(target_os = "windows")]
pub(crate) fn build_backend_for_mode(
    kind: CaptureBackendKind,
    auto_policy: AutoBackendPolicy,
    auto_policy_is_explicit: bool,
    _mode: CaptureMode,
) -> crate::error::CaptureResult<Arc<dyn CaptureBackend>> {
    let backend =
        windows::WindowsBackend::with_kind_and_policy(kind, auto_policy, auto_policy_is_explicit)?;
    Ok(Arc::new(backend))
}

#[cfg(target_os = "windows")]
pub(crate) fn monitor_layout_from_monitors(
    monitors: Vec<crate::monitor::MonitorId>,
) -> crate::error::CaptureResult<MonitorLayout> {
    windows::monitor::snapshot_layout_from_monitors(monitors)
}

#[cfg(target_os = "linux")]
pub(crate) mod linux;

#[cfg(target_os = "linux")]
pub(crate) fn build_backend(
    kind: CaptureBackendKind,
    _auto_policy: AutoBackendPolicy,
    _auto_policy_is_explicit: bool,
) -> crate::error::CaptureResult<Arc<dyn CaptureBackend>> {
    build_backend_for_mode(kind, _auto_policy, _auto_policy_is_explicit, CaptureMode::Snapshot)
}

#[cfg(target_os = "linux")]
pub(crate) fn build_backend_for_mode(
    kind: CaptureBackendKind,
    _auto_policy: AutoBackendPolicy,
    _auto_policy_is_explicit: bool,
    _mode: CaptureMode,
) -> crate::error::CaptureResult<Arc<dyn CaptureBackend>> {
    if !matches!(kind, CaptureBackendKind::Auto | CaptureBackendKind::X11) {
        return Err(unsupported_backend_error(kind));
    }
    Ok(Arc::new(linux::LinuxBackend))
}

#[cfg(target_os = "linux")]
pub(crate) fn monitor_layout_from_monitors(
    monitors: Vec<crate::monitor::MonitorId>,
) -> crate::error::CaptureResult<MonitorLayout> {
    linux::snapshot_layout_from_monitors(monitors)
}

#[cfg(not(any(target_os = "windows", target_os = "macos", target_os = "linux")))]
fn unsupported_error() -> CaptureError {
    CaptureError::platform(anyhow::anyhow!(
        "screen capture is only supported on Windows, macOS and Linux"
    ))
}

#[cfg(not(any(target_os = "windows", target_os = "macos", target_os = "linux")))]
struct UnsupportedBackend;

#[cfg(not(any(target_os = "windows", target_os = "macos", target_os = "linux")))]
impl CaptureBackend for UnsupportedBackend {
    fn enumerate_monitors(&self) -> CaptureResult<Vec<MonitorId>> {
        Err(unsupported_error())
    }

    fn primary_monitor(&self) -> CaptureResult<MonitorId> {
        Err(unsupported_error())
    }

    fn monitor_layout(&self) -> CaptureResult<MonitorLayout> {
        Err(unsupported_error())
    }

    fn inspect_window(
        &self,
        _window: &WindowId,
    ) -> CaptureResult<crate::capture_session::CaptureTargetInfo> {
        Err(unsupported_error())
    }

    fn create_monitor_capturer(
        &self,
        _monitor: &MonitorId,
    ) -> CaptureResult<Box<dyn MonitorCapturer>> {
        Err(unsupported_error())
    }

    fn create_window_capturer(
        &self,
        _window: &WindowId,
    ) -> CaptureResult<Box<dyn MonitorCapturer>> {
        Err(unsupported_error())
    }
}

#[cfg(not(any(target_os = "windows", target_os = "macos", target_os = "linux")))]
pub(crate) fn build_backend(
    kind: CaptureBackendKind,
    auto_policy: AutoBackendPolicy,
    auto_policy_is_explicit: bool,
) -> crate::error::CaptureResult<Arc<dyn CaptureBackend>> {
    build_backend_for_mode(
        kind,
        auto_policy,
        auto_policy_is_explicit,
        CaptureMode::Continuous,
    )
}

#[cfg(not(any(target_os = "windows", target_os = "macos", target_os = "linux")))]
pub(crate) fn build_backend_for_mode(
    _kind: CaptureBackendKind,
    _auto_policy: AutoBackendPolicy,
    _auto_policy_is_explicit: bool,
    _mode: CaptureMode,
) -> crate::error::CaptureResult<Arc<dyn CaptureBackend>> {
    Ok(Arc::new(UnsupportedBackend))
}

#[cfg(not(any(target_os = "windows", target_os = "macos", target_os = "linux")))]
pub(crate) fn monitor_layout_from_monitors(
    _monitors: Vec<crate::monitor::MonitorId>,
) -> crate::error::CaptureResult<MonitorLayout> {
    Err(unsupported_error())
}

#[cfg(target_os = "macos")]
pub(crate) mod macos;
#[cfg(target_os = "macos")]
pub(crate) fn build_backend(
    kind: CaptureBackendKind,
    policy: AutoBackendPolicy,
    explicit: bool,
) -> crate::error::CaptureResult<Arc<dyn CaptureBackend>> {
    build_backend_for_mode(kind, policy, explicit, CaptureMode::Snapshot)
}
#[cfg(target_os = "macos")]
pub(crate) fn build_backend_for_mode(
    kind: CaptureBackendKind,
    _policy: AutoBackendPolicy,
    _explicit: bool,
    _mode: CaptureMode,
) -> crate::error::CaptureResult<Arc<dyn CaptureBackend>> {
    if !matches!(
        kind,
        CaptureBackendKind::Auto | CaptureBackendKind::ScreenCaptureKit
    ) {
        return Err(crate::error::CaptureError::BackendUnavailable(
            "requested backend is unavailable on macOS".into(),
        ));
    }
    Ok(Arc::new(macos::MacBackend))
}
#[cfg(target_os = "macos")]
pub(crate) fn monitor_layout_from_monitors(
    monitors: Vec<crate::monitor::MonitorId>,
) -> crate::error::CaptureResult<MonitorLayout> {
    macos::layout(Some(&monitors))
}
