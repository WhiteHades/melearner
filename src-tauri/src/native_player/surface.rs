use super::NativePlayerBounds;
#[cfg(target_os = "linux")]
mod linux_gtk;
#[cfg(target_os = "macos")]
mod macos_appkit;

use libmpv2::Mpv;
use std::{
    path::{Path, PathBuf},
    sync::{
        Arc, Mutex,
        atomic::{AtomicBool, AtomicU64, Ordering},
    },
};
use tauri::WebviewWindow;

const SURFACE_BACKEND_ENV: &str = "MELEARNER_SURFACE_BACKEND";
const NATIVE_SURFACE_LOG_ENV: &str = "MELEARNER_NATIVE_SURFACE_LOG";

fn native_surface_runtime_log_path() -> PathBuf {
    let configured = std::env::var(NATIVE_SURFACE_LOG_ENV).ok();
    let home = std::env::var("HOME").ok();
    native_surface_runtime_log_path_from_values(configured.as_deref(), home.as_deref())
}

fn native_surface_runtime_log_path_from_values(
    configured: Option<&str>,
    home: Option<&str>,
) -> PathBuf {
    if let Some(configured) = configured.map(str::trim).filter(|value| !value.is_empty()) {
        return PathBuf::from(configured);
    }

    home.map(|home| {
        PathBuf::from(home)
            .join(".melearner")
            .join("native-surface.log")
    })
    .unwrap_or_else(|| std::env::temp_dir().join("melearner-native-surface.log"))
}

fn record_native_surface_runtime_log(message: &str) {
    let _ = record_native_surface_runtime_log_at(&native_surface_runtime_log_path(), message);
}

fn record_native_surface_runtime_log_at(path: &Path, message: &str) -> std::io::Result<()> {
    use std::io::Write;

    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent)?;
    }

    let ts = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|duration| duration.as_secs_f64())
        .unwrap_or(0.0);
    let mut file = std::fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(path)?;
    writeln!(file, "[{ts}] {message}")
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
    RenderApi(&'static str),
}

impl NativeSurfaceBackend {
    pub(super) fn label(self) -> String {
        match self {
            Self::RenderApi(name) => format!("render-api:{name}"),
        }
    }

    pub(super) fn uses_render_api(self) -> bool {
        true
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum NativeSurfaceBackendPreference {
    RenderApi,
}

#[derive(Clone, Debug, Default)]
pub(super) struct NativeSurfaceDiagnostics {
    pub(super) render_thread_alive: bool,
    pub(super) rendered_frames: u64,
    pub(super) last_render_width: Option<u32>,
    pub(super) last_render_height: Option<u32>,
    pub(super) last_render_update_flags: u64,
    pub(super) last_error: Option<String>,
}

impl NativeSurfaceBackendPreference {
    pub(super) fn current() -> Result<Self, String> {
        Self::from_env_result(std::env::var(SURFACE_BACKEND_ENV))
    }

    pub(super) fn from_env_result(
        value: Result<String, std::env::VarError>,
    ) -> Result<Self, String> {
        match value {
            Ok(value) => Self::from_env_value(Some(value.as_str())),
            Err(std::env::VarError::NotPresent) => Self::from_env_value(None),
            Err(std::env::VarError::NotUnicode(_)) => {
                Err(format!("{SURFACE_BACKEND_ENV} must be valid unicode"))
            }
        }
    }

    pub(super) fn from_env_value(value: Option<&str>) -> Result<Self, String> {
        match value.map(str::trim).filter(|value| !value.is_empty()) {
            None | Some("render-api") => Ok(Self::RenderApi),
            Some("window-handle") => Err(format!(
                "{SURFACE_BACKEND_ENV}=window-handle is disabled; playback must use the render-api surface"
            )),
            Some(value) => Err(format!(
                "invalid {SURFACE_BACKEND_ENV} value {value:?}; expected render-api"
            )),
        }
    }
}

pub struct NativeVideoSurface {
    rect: NativeSurfaceRect,
    backend: NativeSurfaceBackend,
    attachment: NativeSurfaceAttachment,
}

enum NativeSurfaceAttachment {
    #[cfg(target_os = "linux")]
    GtkInWindow {
        handle: linux_gtk::GtkInWindowSurfaceHandle,
    },
    #[cfg(target_os = "macos")]
    MacosInWindow {
        handle: macos_appkit::MacosInWindowSurfaceHandle,
    },
    #[cfg(not(any(target_os = "linux", target_os = "macos")))]
    Unsupported,
}

impl NativeVideoSurface {
    pub(super) fn attach(
        parent: &WebviewWindow,
        bounds: NativePlayerBounds,
        mpv: &Mpv,
    ) -> Result<Self, String> {
        match NativeSurfaceBackendPreference::current()? {
            NativeSurfaceBackendPreference::RenderApi => {
                #[cfg(target_os = "linux")]
                {
                    return Self::attach_gtk_in_window(parent, bounds)
                        .and_then(|surface| Self::attach_surface_to_mpv(surface, mpv));
                }

                #[cfg(not(target_os = "linux"))]
                {
                    #[cfg(target_os = "macos")]
                    {
                        return Self::attach_macos_in_window(parent, bounds)
                            .and_then(|surface| Self::attach_surface_to_mpv(surface, mpv));
                    }

                    #[cfg(not(target_os = "macos"))]
                    {
                        let _ = (parent, bounds, mpv);
                        Err(unsupported_platform_surface_error())
                    }
                }
            }
        }
    }

    fn attach_surface_to_mpv(mut surface: Self, mpv: &Mpv) -> Result<Self, String> {
        surface.attach_to_mpv(mpv)?;
        Ok(surface)
    }

    #[cfg(target_os = "linux")]
    fn attach_gtk_in_window(
        parent: &WebviewWindow,
        bounds: NativePlayerBounds,
    ) -> Result<Self, String> {
        let rect = surface_rect_for_local_bounds(bounds)?;
        Ok(Self {
            rect,
            backend: NativeSurfaceBackend::RenderApi("gtk-opengl"),
            attachment: NativeSurfaceAttachment::GtkInWindow {
                handle: linux_gtk::GtkInWindowSurfaceHandle::attach(parent, rect)?,
            },
        })
    }

    #[cfg(target_os = "macos")]
    fn attach_macos_in_window(
        parent: &WebviewWindow,
        bounds: NativePlayerBounds,
    ) -> Result<Self, String> {
        let rect = surface_rect_for_local_bounds(bounds)?;
        Ok(Self {
            rect,
            backend: NativeSurfaceBackend::RenderApi("appkit-opengl"),
            attachment: NativeSurfaceAttachment::MacosInWindow {
                handle: macos_appkit::MacosInWindowSurfaceHandle::attach(parent, rect)?,
            },
        })
    }

    pub(super) fn move_to(
        &mut self,
        _parent: &WebviewWindow,
        bounds: NativePlayerBounds,
    ) -> Result<(), String> {
        #[cfg(target_os = "linux")]
        {
            let NativeSurfaceAttachment::GtkInWindow { handle } = &self.attachment;
            let rect = surface_rect_for_local_bounds(bounds)?;
            handle.move_to(rect)?;
            self.rect = rect;
            return Ok(());
        }

        #[cfg(not(target_os = "linux"))]
        {
            #[cfg(target_os = "macos")]
            {
                let NativeSurfaceAttachment::MacosInWindow { handle } = &self.attachment;
                let rect = surface_rect_for_local_bounds(bounds)?;
                handle.move_to(rect)?;
                self.rect = rect;
                return Ok(());
            }

            #[cfg(not(target_os = "macos"))]
            {
                let _ = bounds;
                return Err(unsupported_platform_surface_error());
            }
        }
    }

    fn attach_to_mpv(&mut self, mpv: &Mpv) -> Result<(), String> {
        match &mut self.attachment {
            #[cfg(target_os = "linux")]
            NativeSurfaceAttachment::GtkInWindow { handle } => {
                mpv.set_property("vo", "libmpv").map_err(|err| {
                    format!("libmpv could not enable gtk render-api video output: {err}")
                })?;
                handle.attach_to_mpv(mpv)
            }
            #[cfg(target_os = "macos")]
            NativeSurfaceAttachment::MacosInWindow { handle } => {
                mpv.set_property("vo", "libmpv").map_err(|err| {
                    format!("libmpv could not enable macos render-api video output: {err}")
                })?;
                handle.attach_to_mpv(mpv)
            }
            #[cfg(not(any(target_os = "linux", target_os = "macos")))]
            NativeSurfaceAttachment::Unsupported => {
                let _ = mpv;
                Err(unsupported_platform_surface_error())
            }
        }
    }

    pub(super) fn set_visible(&self, visible: bool) -> Result<(), String> {
        match &self.attachment {
            #[cfg(target_os = "linux")]
            NativeSurfaceAttachment::GtkInWindow { handle } => handle.set_visible(visible),
            #[cfg(target_os = "macos")]
            NativeSurfaceAttachment::MacosInWindow { handle } => handle.set_visible(visible),
            #[cfg(not(any(target_os = "linux", target_os = "macos")))]
            NativeSurfaceAttachment::Unsupported => {
                let _ = visible;
                Err(unsupported_platform_surface_error())
            }
        }
    }

    pub(super) fn request_render(&self) -> Result<(), String> {
        match &self.attachment {
            #[cfg(target_os = "linux")]
            NativeSurfaceAttachment::GtkInWindow { handle } => handle.request_render(),
            #[cfg(target_os = "macos")]
            NativeSurfaceAttachment::MacosInWindow { handle } => handle.request_render(),
            #[cfg(not(any(target_os = "linux", target_os = "macos")))]
            NativeSurfaceAttachment::Unsupported => Err(unsupported_platform_surface_error()),
        }
    }

    pub(super) fn backend_label(&self) -> String {
        self.backend.label()
    }

    pub(super) fn uses_render_api(&self) -> bool {
        self.backend.uses_render_api()
    }

    pub(super) fn diagnostics(&self) -> NativeSurfaceDiagnostics {
        match &self.attachment {
            #[cfg(target_os = "linux")]
            NativeSurfaceAttachment::GtkInWindow { handle } => handle.diagnostics(),
            #[cfg(target_os = "macos")]
            NativeSurfaceAttachment::MacosInWindow { handle } => handle.diagnostics(),
            #[cfg(not(any(target_os = "linux", target_os = "macos")))]
            NativeSurfaceAttachment::Unsupported => NativeSurfaceDiagnostics::default(),
        }
    }
}

impl Drop for NativeVideoSurface {
    fn drop(&mut self) {
        match &mut self.attachment {
            #[cfg(target_os = "linux")]
            NativeSurfaceAttachment::GtkInWindow { .. } => {}
            #[cfg(target_os = "macos")]
            NativeSurfaceAttachment::MacosInWindow { .. } => {}
            #[cfg(not(any(target_os = "linux", target_os = "macos")))]
            NativeSurfaceAttachment::Unsupported => {}
        }
    }
}

#[cfg(not(any(target_os = "linux", target_os = "macos")))]
fn unsupported_platform_surface_error() -> String {
    let message = "native in-window render surface is not implemented for this platform; normal playback must not create a separate video window".to_string();
    record_native_surface_runtime_log(&message);
    message
}

#[derive(Clone)]
struct RenderApiDiagnostics {
    render_thread_alive: Arc<AtomicBool>,
    rendered_frames: Arc<AtomicU64>,
    last_render_width: Arc<AtomicU64>,
    last_render_height: Arc<AtomicU64>,
    last_render_update_flags: Arc<AtomicU64>,
    last_error: Arc<Mutex<Option<String>>>,
}

impl RenderApiDiagnostics {
    fn new() -> Self {
        Self {
            render_thread_alive: Arc::new(AtomicBool::new(false)),
            rendered_frames: Arc::new(AtomicU64::new(0)),
            last_render_width: Arc::new(AtomicU64::new(0)),
            last_render_height: Arc::new(AtomicU64::new(0)),
            last_render_update_flags: Arc::new(AtomicU64::new(0)),
            last_error: Arc::new(Mutex::new(None)),
        }
    }

    fn set_alive(&self, alive: bool) {
        self.render_thread_alive.store(alive, Ordering::SeqCst);
    }

    fn record_frame(&self, width: u32, height: u32, update_flags: u64) {
        self.rendered_frames.fetch_add(1, Ordering::SeqCst);
        self.last_render_width
            .store(u64::from(width), Ordering::SeqCst);
        self.last_render_height
            .store(u64::from(height), Ordering::SeqCst);
        self.last_render_update_flags
            .store(update_flags, Ordering::SeqCst);
    }

    fn record_error(&self, err: &str) {
        if let Ok(mut last_error) = self.last_error.lock() {
            *last_error = Some(err.to_string());
        }
    }

    fn snapshot(&self) -> NativeSurfaceDiagnostics {
        let last_render_width = self.last_render_width.load(Ordering::SeqCst);
        let last_render_height = self.last_render_height.load(Ordering::SeqCst);
        NativeSurfaceDiagnostics {
            render_thread_alive: self.render_thread_alive.load(Ordering::SeqCst),
            rendered_frames: self.rendered_frames.load(Ordering::SeqCst),
            last_render_width: u32::try_from(last_render_width)
                .ok()
                .filter(|value| *value > 0),
            last_render_height: u32::try_from(last_render_height)
                .ok()
                .filter(|value| *value > 0),
            last_render_update_flags: self.last_render_update_flags.load(Ordering::SeqCst),
            last_error: self.last_error.lock().ok().and_then(|error| error.clone()),
        }
    }
}

pub(super) fn surface_rect_for_local_bounds(
    bounds: NativePlayerBounds,
) -> Result<NativeSurfaceRect, String> {
    if bounds.width <= 0 || bounds.height <= 0 {
        return Err("native player bounds are invalid".to_string());
    }
    if !bounds.scale_factor.is_finite() || bounds.scale_factor <= 0.0 {
        return Err("native player scale factor is invalid".to_string());
    }

    Ok(NativeSurfaceRect {
        x: checked_surface_i32(bounds.x as f64 * bounds.scale_factor, "x")?,
        y: checked_surface_i32(bounds.y as f64 * bounds.scale_factor, "y")?,
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn native_surface_runtime_log_uses_configured_or_home_path() {
        assert_eq!(
            native_surface_runtime_log_path_from_values(
                Some("/tmp/melearner/native-surface.log"),
                Some("/home/example")
            ),
            std::path::PathBuf::from("/tmp/melearner/native-surface.log")
        );
        assert_eq!(
            native_surface_runtime_log_path_from_values(None, Some("/home/example")),
            std::path::PathBuf::from("/home/example/.melearner/native-surface.log")
        );
    }

    #[test]
    fn native_surface_runtime_log_appends_messages() {
        let path = std::env::temp_dir().join(format!(
            "melearner-native-surface-log-{}.log",
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .expect("system time before unix epoch")
                .as_nanos()
        ));

        record_native_surface_runtime_log_at(&path, "render-api initialized")
            .expect("write first log");
        record_native_surface_runtime_log_at(&path, "render-api failed").expect("write second log");

        let contents = std::fs::read_to_string(&path).expect("read log");
        assert!(contents.contains("render-api initialized"));
        assert!(contents.contains("render-api failed"));

        let _ = std::fs::remove_file(path);
    }
}
