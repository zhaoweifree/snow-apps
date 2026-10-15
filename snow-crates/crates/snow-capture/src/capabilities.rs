//! Feature discovery is separate from permission requests and target acquisition.
use crate::backend::CaptureBackendKind;
use snow_media::{PixelFormat, geometry::DesktopSpace};
#[derive(Clone, Debug)]
pub struct CaptureCapabilities {
    pub backends: Vec<CaptureBackendKind>,
    pub desktop_space: DesktopSpace,
    pub cpu_formats: Vec<PixelFormat>,
    pub native_frames: bool,
    pub hdr_capture: bool,
    pub window_enumeration: bool,
}
impl CaptureCapabilities {
    pub fn current() -> Self {
        #[cfg(target_os = "macos")]
        {
            let native = snow_macos::capabilities::CaptureSupport::current();
            let supported = native.screen_capture_kit && native.metal_composition;
            let hdr = supported && native.hdr_capture;
            let mut formats = vec![PixelFormat::Rgba8, PixelFormat::Bgra8];
            if hdr {
                formats.push(PixelFormat::Rgba16Float);
            }
            Self {
                backends: if supported {
                    vec![CaptureBackendKind::ScreenCaptureKit]
                } else {
                    vec![]
                },
                desktop_space: DesktopSpace::Points,
                cpu_formats: if supported { formats } else { vec![] },
                native_frames: supported,
                hdr_capture: hdr,
                window_enumeration: supported,
            }
        }
        #[cfg(target_os = "linux")]
        {
            // X11 root-window capture produces CPU pixels only. Whether a server
            // is actually reachable is a session question rather than a
            // capability one: a headless or pure-Wayland host advertises no
            // backend, and everything else fails later with `SessionMissing`,
            // which callers already treat as "feature unavailable".
            let kind = snow_linux::session::SessionKind::detect();
            let supported = kind.has_x11();
            Self {
                backends: if supported {
                    vec![CaptureBackendKind::X11]
                } else {
                    vec![]
                },
                // X11 root coordinates are pixels; a HiDPI compositor scales at
                // the toolkit layer, never in the root window.
                desktop_space: DesktopSpace::PhysicalPixels,
                cpu_formats: if supported {
                    vec![PixelFormat::Rgba8, PixelFormat::Bgra8]
                } else {
                    vec![]
                },
                native_frames: false,
                hdr_capture: false,
                window_enumeration: false,
            }
        }
        #[cfg(not(any(target_os = "macos", target_os = "linux")))]
        {
            let supported = cfg!(windows);
            Self {
                backends: if supported {
                    vec![
                        CaptureBackendKind::DxgiDuplication,
                        CaptureBackendKind::WindowsGraphicsCapture,
                        CaptureBackendKind::Gdi,
                    ]
                } else {
                    vec![]
                },
                desktop_space: DesktopSpace::PhysicalPixels,
                cpu_formats: if supported {
                    vec![PixelFormat::Rgba8, PixelFormat::Bgra8]
                } else {
                    vec![]
                },
                native_frames: supported,
                hdr_capture: false,
                window_enumeration: false,
            }
        }
    }
}
