use super::NativePlayerBounds;
use libmpv2::Mpv;
use std::sync::{Mutex, OnceLock};
use tauri::{AppHandle, PhysicalPosition, PhysicalSize, WebviewWindow, Window};

#[cfg(any(
    target_os = "linux",
    target_os = "dragonfly",
    target_os = "freebsd",
    target_os = "netbsd",
    target_os = "openbsd"
))]
use raw_window_handle::{HasWindowHandle, RawWindowHandle};

static SURFACE_WINDOW_RUN: OnceLock<Mutex<u64>> = OnceLock::new();
const NATIVE_SURFACE_LABEL: &str = "native-player-surface";
const SURFACE_BACKEND_ENV: &str = "MELEARNER_SURFACE_BACKEND";

fn surface_window_slot() -> &'static Mutex<u64> {
    SURFACE_WINDOW_RUN.get_or_init(|| Mutex::new(0))
}

pub(super) fn next_surface_window_label() -> Result<String, String> {
    let mut guard = surface_window_slot()
        .lock()
        .map_err(|_| "native player surface label lock is poisoned".to_string())?;
    *guard = guard.wrapping_add(1);
    Ok(format!("{NATIVE_SURFACE_LABEL}-{}", *guard))
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) struct NativeSurfaceRect {
    pub(super) x: i32,
    pub(super) y: i32,
    pub(super) width: u32,
    pub(super) height: u32,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum NativeSurfaceBackend {
    WindowHandle(&'static str),
}

impl NativeSurfaceBackend {
    pub(super) fn label(self) -> String {
        match self {
            Self::WindowHandle(name) => format!("window-handle:{name}"),
        }
    }

    pub(super) fn uses_render_api(self) -> bool {
        match self {
            Self::WindowHandle(_) => false,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum NativeSurfaceBackendPreference {
    WindowHandle,
    RenderApi,
}

impl NativeSurfaceBackendPreference {
    pub(super) fn current() -> Result<Self, String> {
        match std::env::var(SURFACE_BACKEND_ENV) {
            Ok(value) => Self::from_env_value(Some(value.as_str())),
            Err(std::env::VarError::NotPresent) => Ok(Self::WindowHandle),
            Err(std::env::VarError::NotUnicode(_)) => {
                Err(format!("{SURFACE_BACKEND_ENV} must be valid unicode"))
            }
        }
    }

    pub(super) fn from_env_value(value: Option<&str>) -> Result<Self, String> {
        match value.map(str::trim).filter(|value| !value.is_empty()) {
            None | Some("window-handle") => Ok(Self::WindowHandle),
            Some("render-api") => Ok(Self::RenderApi),
            Some(value) => Err(format!(
                "invalid {SURFACE_BACKEND_ENV} value {value:?}; expected window-handle or render-api"
            )),
        }
    }
}

pub(super) fn render_api_surface_unavailable_error() -> &'static str {
    "native render-api surface backend is not implemented yet; current available backend is window-handle"
}

pub struct NativeVideoSurface {
    window: Window,
    window_id: i64,
    backend: NativeSurfaceBackend,
}

impl NativeVideoSurface {
    pub(super) fn attach(
        app: &AppHandle,
        parent: &WebviewWindow,
        bounds: NativePlayerBounds,
    ) -> Result<Self, String> {
        match NativeSurfaceBackendPreference::current()? {
            NativeSurfaceBackendPreference::WindowHandle => {
                Self::attach_window_handle(app, parent, bounds)
            }
            NativeSurfaceBackendPreference::RenderApi => {
                Err(render_api_surface_unavailable_error().to_string())
            }
        }
    }

    fn attach_window_handle(
        app: &AppHandle,
        parent: &WebviewWindow,
        bounds: NativePlayerBounds,
    ) -> Result<Self, String> {
        let origin = parent
            .inner_position()
            .map_err(|err| format!("native player could not read host window position: {err}"))?;
        let rect = surface_rect_for_bounds(origin, bounds)?;
        let window = build_surface_window(app, parent, rect)?;

        apply_surface_rect(&window, rect)?;
        window
            .show()
            .map_err(|err| format!("native player could not show video surface: {err}"))?;

        let handle = mpv_window_handle(&window)?;
        Ok(Self {
            window,
            window_id: handle.id,
            backend: NativeSurfaceBackend::WindowHandle(handle.backend),
        })
    }

    pub(super) fn move_to(
        &self,
        parent: &WebviewWindow,
        bounds: NativePlayerBounds,
    ) -> Result<(), String> {
        let origin = parent
            .inner_position()
            .map_err(|err| format!("native player could not read host window position: {err}"))?;
        apply_surface_rect(&self.window, surface_rect_for_bounds(origin, bounds)?)
    }

    pub(super) fn attach_to_mpv(&self, mpv: &Mpv) -> Result<(), String> {
        mpv.set_property("wid", self.window_id)
            .map_err(|err| format!("libmpv could not attach to native video surface: {err}"))?;
        mpv.set_property("vo", "gpu")
            .map_err(|err| format!("libmpv could not enable native video output: {err}"))
    }

    pub(super) fn set_visible(&self, visible: bool) -> Result<(), String> {
        if visible {
            self.window
                .show()
                .map_err(|err| format!("native player could not show video surface: {err}"))
        } else {
            self.window
                .hide()
                .map_err(|err| format!("native player could not hide video surface: {err}"))
        }
    }

    pub(super) fn backend_label(&self) -> String {
        self.backend.label()
    }

    pub(super) fn uses_render_api(&self) -> bool {
        self.backend.uses_render_api()
    }
}

impl Drop for NativeVideoSurface {
    fn drop(&mut self) {
        let _ = self.window.close();
    }
}

fn apply_surface_rect(window: &Window, rect: NativeSurfaceRect) -> Result<(), String> {
    window
        .set_size(PhysicalSize::new(rect.width, rect.height))
        .map_err(|err| format!("native player could not resize video surface: {err}"))?;
    window
        .set_position(PhysicalPosition::new(rect.x, rect.y))
        .map_err(|err| format!("native player could not move video surface: {err}"))
}

fn build_surface_window(
    app: &AppHandle,
    parent: &WebviewWindow,
    rect: NativeSurfaceRect,
) -> Result<Window, String> {
    let label = next_surface_window_label()?;
    let builder = tauri::WindowBuilder::new(app, label)
        .title("melearner video")
        .decorations(false)
        .resizable(false)
        .focused(false)
        .focusable(false)
        .visible(false)
        .skip_taskbar(true)
        .inner_size(rect.width as f64, rect.height as f64)
        .position(rect.x as f64, rect.y as f64);

    #[cfg(windows)]
    let builder = builder.parent_raw(
        parent
            .hwnd()
            .map_err(|err| format!("native player could not read host window handle: {err}"))?,
    );

    #[cfg(target_os = "macos")]
    let builder = builder.parent_raw(
        parent
            .ns_window()
            .map_err(|err| format!("native player could not read host window handle: {err}"))?,
    );

    #[cfg(any(
        target_os = "linux",
        target_os = "dragonfly",
        target_os = "freebsd",
        target_os = "netbsd",
        target_os = "openbsd"
    ))]
    let builder = {
        let gtk_window = parent
            .gtk_window()
            .map_err(|err| format!("native player could not read host window handle: {err}"))?;
        builder.transient_for_raw(&gtk_window)
    };

    builder
        .build()
        .map_err(|err| format!("native player could not create video surface: {err}"))
}

pub(super) fn surface_rect_for_bounds(
    origin: PhysicalPosition<i32>,
    bounds: NativePlayerBounds,
) -> Result<NativeSurfaceRect, String> {
    if bounds.width <= 0 || bounds.height <= 0 {
        return Err("native player bounds are invalid".to_string());
    }
    if !bounds.scale_factor.is_finite() || bounds.scale_factor <= 0.0 {
        return Err("native player scale factor is invalid".to_string());
    }

    Ok(NativeSurfaceRect {
        x: checked_surface_i32(origin.x as f64 + bounds.x as f64 * bounds.scale_factor, "x")?,
        y: checked_surface_i32(origin.y as f64 + bounds.y as f64 * bounds.scale_factor, "y")?,
        width: checked_surface_u32(bounds.width as f64 * bounds.scale_factor, "width")?,
        height: checked_surface_u32(bounds.height as f64 * bounds.scale_factor, "height")?,
    })
}

fn checked_surface_i32(value: f64, label: &str) -> Result<i32, String> {
    let rounded = value.round();
    if !rounded.is_finite() || rounded < i32::MIN as f64 || rounded > i32::MAX as f64 {
        return Err(format!("native player surface {label} is out of range"));
    }
    Ok(rounded as i32)
}

fn checked_surface_u32(value: f64, label: &str) -> Result<u32, String> {
    let rounded = value.round();
    if !rounded.is_finite() || rounded <= 0.0 || rounded > u32::MAX as f64 {
        return Err(format!("native player surface {label} is out of range"));
    }
    Ok(rounded as u32)
}

#[cfg(windows)]
fn mpv_window_handle(window: &Window) -> Result<NativeMpvWindowHandle, String> {
    Ok(NativeMpvWindowHandle {
        id: window
            .hwnd()
            .map_err(|err| {
                format!("native player could not read Win32 video surface handle: {err}")
            })?
            .0 as isize as i64,
        backend: "win32",
    })
}

#[cfg(target_os = "macos")]
fn mpv_window_handle(window: &Window) -> Result<NativeMpvWindowHandle, String> {
    Ok(NativeMpvWindowHandle {
        id: window.ns_view().map_err(|err| {
            format!("native player could not read Cocoa video surface handle: {err}")
        })? as isize as i64,
        backend: "cocoa",
    })
}

#[cfg(any(
    target_os = "linux",
    target_os = "dragonfly",
    target_os = "freebsd",
    target_os = "netbsd",
    target_os = "openbsd"
))]
fn mpv_window_handle(window: &Window) -> Result<NativeMpvWindowHandle, String> {
    let handle = window
        .window_handle()
        .map_err(|err| format!("native player could not read video surface handle: {err}"))?;
    match handle.as_raw() {
        RawWindowHandle::Xlib(handle) => Ok(NativeMpvWindowHandle {
            id: handle.window as i64,
            backend: "xlib",
        }),
        RawWindowHandle::Xcb(handle) => Ok(NativeMpvWindowHandle {
            id: handle.window.get() as i64,
            backend: "xcb",
        }),
        _ => Err("native player video surface requires an X11 window handle on Linux".to_string()),
    }
}

struct NativeMpvWindowHandle {
    id: i64,
    backend: &'static str,
}
