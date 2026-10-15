#[cfg(target_os = "windows")]
mod windows;

#[cfg(target_os = "linux")]
mod linux;

#[cfg(target_os = "windows")]
pub(crate) use windows::WindowsCursorSampler as CursorSamplerImpl;

#[cfg(target_os = "linux")]
pub(crate) use linux::LinuxCursorSampler as CursorSamplerImpl;

#[cfg(not(any(target_os = "windows", target_os = "linux")))]
pub(crate) struct CursorSamplerImpl;

#[cfg(not(any(target_os = "windows", target_os = "linux")))]
impl CursorSamplerImpl {
    pub(crate) fn new() -> Result<Self, crate::CursorCaptureError> {
        Err(crate::CursorCaptureError::UnsupportedPlatform)
    }

    pub(crate) fn sample_cursor(
        &mut self,
    ) -> Result<crate::sampler::CursorProbe, crate::CursorCaptureError> {
        Err(crate::CursorCaptureError::UnsupportedPlatform)
    }
}
