use super::NativePlayerBounds;
use glutin::{
    config::{ConfigTemplateBuilder, GlConfig},
    context::{ContextAttributesBuilder, GlProfile, NotCurrentGlContext},
    display::{Display, DisplayApiPreference, GlDisplay},
    prelude::GlSurface,
    surface::{SurfaceAttributesBuilder, WindowSurface},
};
use libmpv2::Mpv;
use libmpv2::render::{OpenGLInitParams, RenderParam, RenderParamApiType};
use raw_window_handle::{HasDisplayHandle, HasWindowHandle, RawDisplayHandle, RawWindowHandle};
use std::{
    ffi::{CString, c_void},
    num::NonZeroU32,
    sync::{
        Arc, Mutex, OnceLock,
        atomic::{AtomicBool, AtomicU64, Ordering},
        mpsc,
    },
    thread::{self, JoinHandle},
};
use tauri::{AppHandle, PhysicalPosition, PhysicalSize, WebviewWindow, Window};

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
    RenderApi(&'static str),
}

impl NativeSurfaceBackend {
    pub(super) fn label(self) -> String {
        match self {
            Self::WindowHandle(name) => format!("window-handle:{name}"),
            Self::RenderApi(name) => format!("render-api:{name}"),
        }
    }

    pub(super) fn uses_render_api(self) -> bool {
        match self {
            Self::WindowHandle(_) => false,
            Self::RenderApi(_) => true,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum NativeSurfaceBackendPreference {
    WindowHandle,
    RenderApi,
}

#[derive(Clone, Debug, Default)]
pub(super) struct NativeSurfaceDiagnostics {
    pub(super) render_thread_alive: bool,
    pub(super) rendered_frames: u64,
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
            Some("window-handle") => Ok(Self::WindowHandle),
            Some(value) => Err(format!(
                "invalid {SURFACE_BACKEND_ENV} value {value:?}; expected render-api or window-handle"
            )),
        }
    }
}

pub struct NativeVideoSurface {
    window: Window,
    rect: NativeSurfaceRect,
    backend: NativeSurfaceBackend,
    attachment: NativeSurfaceAttachment,
}

enum NativeSurfaceAttachment {
    WindowHandle { window_id: i64 },
    RenderApi { renderer: Option<RenderApiSurface> },
}

impl NativeVideoSurface {
    pub(super) fn attach(
        app: &AppHandle,
        parent: &WebviewWindow,
        bounds: NativePlayerBounds,
        mpv: &Mpv,
    ) -> Result<Self, String> {
        match NativeSurfaceBackendPreference::current()? {
            NativeSurfaceBackendPreference::WindowHandle => {
                Self::attach_window_handle(app, parent, bounds)
                    .and_then(|surface| Self::attach_surface_to_mpv(surface, mpv))
            }
            NativeSurfaceBackendPreference::RenderApi => {
                match Self::attach_render_api(app, parent, bounds)
                    .and_then(|surface| Self::attach_surface_to_mpv(surface, mpv))
                {
                    Ok(surface) => Ok(surface),
                    Err(render_err) => {
                        log::warn!(
                            "native render-api surface failed; falling back to window-handle surface: {render_err}"
                        );
                        Self::attach_window_handle(app, parent, bounds)
                        .and_then(|surface| Self::attach_surface_to_mpv(surface, mpv))
                        .map_err(|fallback_err| {
                            format!(
                                "native render-api surface failed ({render_err}); window-handle fallback failed ({fallback_err})"
                            )
                        })
                    }
                }
            }
        }
    }

    fn attach_surface_to_mpv(mut surface: Self, mpv: &Mpv) -> Result<Self, String> {
        surface.attach_to_mpv(mpv)?;
        Ok(surface)
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
            rect,
            backend: NativeSurfaceBackend::WindowHandle(handle.backend),
            attachment: NativeSurfaceAttachment::WindowHandle {
                window_id: handle.id,
            },
        })
    }

    fn attach_render_api(
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

        Ok(Self {
            window,
            rect,
            backend: NativeSurfaceBackend::RenderApi("opengl"),
            attachment: NativeSurfaceAttachment::RenderApi { renderer: None },
        })
    }

    pub(super) fn move_to(
        &mut self,
        parent: &WebviewWindow,
        bounds: NativePlayerBounds,
    ) -> Result<(), String> {
        let origin = parent
            .inner_position()
            .map_err(|err| format!("native player could not read host window position: {err}"))?;
        let rect = surface_rect_for_bounds(origin, bounds)?;
        apply_surface_rect(&self.window, rect)?;
        self.rect = rect;

        if let NativeSurfaceAttachment::RenderApi {
            renderer: Some(renderer),
        } = &self.attachment
        {
            renderer.resize(rect)?;
        }

        Ok(())
    }

    fn attach_to_mpv(&mut self, mpv: &Mpv) -> Result<(), String> {
        match &mut self.attachment {
            NativeSurfaceAttachment::WindowHandle { window_id } => {
                mpv.set_property("wid", *window_id).map_err(|err| {
                    format!("libmpv could not attach to native video surface: {err}")
                })?;
                mpv.set_property("vo", "gpu")
                    .map_err(|err| format!("libmpv could not enable native video output: {err}"))
            }
            NativeSurfaceAttachment::RenderApi { renderer } => {
                mpv.set_property("vo", "libmpv").map_err(|err| {
                    format!("libmpv could not enable render-api video output: {err}")
                })?;
                *renderer = Some(RenderApiSurface::start(&self.window, self.rect, mpv)?);
                Ok(())
            }
        }
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

    pub(super) fn diagnostics(&self) -> NativeSurfaceDiagnostics {
        match &self.attachment {
            NativeSurfaceAttachment::RenderApi {
                renderer: Some(renderer),
            } => renderer.diagnostics(),
            _ => NativeSurfaceDiagnostics::default(),
        }
    }
}

impl Drop for NativeVideoSurface {
    fn drop(&mut self) {
        if let NativeSurfaceAttachment::RenderApi { renderer } = &mut self.attachment {
            renderer.take();
        }
        let _ = self.window.close();
    }
}

struct RenderApiSurface {
    commands: mpsc::Sender<RenderApiCommand>,
    thread: Option<JoinHandle<()>>,
    diagnostics: RenderApiDiagnostics,
}

impl RenderApiSurface {
    fn start(window: &Window, rect: NativeSurfaceRect, mpv: &Mpv) -> Result<Self, String> {
        let handles = RenderApiRawHandles {
            display: window
                .display_handle()
                .map_err(|err| format!("native render-api could not read display handle: {err}"))?
                .as_raw(),
            window: window
                .window_handle()
                .map_err(|err| format!("native render-api could not read window handle: {err}"))?
                .as_raw(),
        };
        let mpv_client = mpv
            .create_client(None)
            .map_err(|err| format!("libmpv could not create render-api client: {err}"))?;
        let (commands, command_rx) = mpsc::channel();
        let (init_tx, init_rx) = mpsc::channel();
        let callback_tx = commands.clone();
        let diagnostics = RenderApiDiagnostics::new();
        let thread_diagnostics = diagnostics.clone();
        let thread = thread::Builder::new()
            .name("melearner-render-api".to_string())
            .spawn(move || {
                run_render_api_thread(
                    handles,
                    rect,
                    mpv_client,
                    command_rx,
                    callback_tx,
                    init_tx,
                    thread_diagnostics,
                );
            })
            .map_err(|err| format!("native render-api could not start render thread: {err}"))?;

        match init_rx.recv() {
            Ok(Ok(())) => Ok(Self {
                commands,
                thread: Some(thread),
                diagnostics,
            }),
            Ok(Err(err)) => {
                let _ = thread.join();
                Err(err)
            }
            Err(_) => {
                let _ = thread.join();
                Err("native render-api render thread exited before initialization".to_string())
            }
        }
    }

    fn resize(&self, rect: NativeSurfaceRect) -> Result<(), String> {
        self.commands
            .send(RenderApiCommand::Resize(rect))
            .map_err(|err| format!("native render-api could not send resize command: {err}"))
    }

    fn diagnostics(&self) -> NativeSurfaceDiagnostics {
        self.diagnostics.snapshot()
    }
}

impl Drop for RenderApiSurface {
    fn drop(&mut self) {
        let _ = self.commands.send(RenderApiCommand::Shutdown);
        if let Some(thread) = self.thread.take() {
            let _ = thread.join();
        }
    }
}

#[derive(Clone)]
struct RenderApiDiagnostics {
    render_thread_alive: Arc<AtomicBool>,
    rendered_frames: Arc<AtomicU64>,
    last_error: Arc<Mutex<Option<String>>>,
}

impl RenderApiDiagnostics {
    fn new() -> Self {
        Self {
            render_thread_alive: Arc::new(AtomicBool::new(false)),
            rendered_frames: Arc::new(AtomicU64::new(0)),
            last_error: Arc::new(Mutex::new(None)),
        }
    }

    fn set_alive(&self, alive: bool) {
        self.render_thread_alive.store(alive, Ordering::SeqCst);
    }

    fn record_frame(&self) {
        self.rendered_frames.fetch_add(1, Ordering::SeqCst);
    }

    fn record_error(&self, err: &str) {
        if let Ok(mut last_error) = self.last_error.lock() {
            *last_error = Some(err.to_string());
        }
    }

    fn snapshot(&self) -> NativeSurfaceDiagnostics {
        NativeSurfaceDiagnostics {
            render_thread_alive: self.render_thread_alive.load(Ordering::SeqCst),
            rendered_frames: self.rendered_frames.load(Ordering::SeqCst),
            last_error: self.last_error.lock().ok().and_then(|error| error.clone()),
        }
    }
}

#[derive(Clone, Copy)]
struct RenderApiRawHandles {
    display: RawDisplayHandle,
    window: RawWindowHandle,
}

unsafe impl Send for RenderApiRawHandles {}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum RenderApiDisplayHandle {
    X11(*const c_void),
    Wayland(*const c_void),
}

enum RenderApiCommand {
    Render,
    Resize(NativeSurfaceRect),
    Shutdown,
}

fn render_api_command_counts_frame(command: &RenderApiCommand) -> bool {
    matches!(command, RenderApiCommand::Render)
}

fn run_render_api_thread(
    handles: RenderApiRawHandles,
    rect: NativeSurfaceRect,
    mpv_client: Mpv,
    command_rx: mpsc::Receiver<RenderApiCommand>,
    callback_tx: mpsc::Sender<RenderApiCommand>,
    init_tx: mpsc::Sender<Result<(), String>>,
    diagnostics: RenderApiDiagnostics,
) {
    let mut renderer = match RenderApiRenderer::new(handles, rect, &mpv_client) {
        Ok(renderer) => {
            diagnostics.set_alive(true);
            let _ = init_tx.send(Ok(()));
            renderer
        }
        Err(err) => {
            diagnostics.record_error(&err);
            diagnostics.set_alive(false);
            let _ = init_tx.send(Err(err));
            return;
        }
    };

    renderer.render_context.set_update_callback(move || {
        let _ = callback_tx.send(RenderApiCommand::Render);
    });

    while let Ok(command) = command_rx.recv() {
        let result = match command {
            RenderApiCommand::Render => renderer.render(),
            RenderApiCommand::Resize(rect) => renderer.resize(rect),
            RenderApiCommand::Shutdown => break,
        };

        if let Err(err) = result {
            log::error!("native render-api failed: {err}");
            diagnostics.record_error(&err);
            diagnostics.set_alive(false);
            break;
        } else if render_api_command_counts_frame(&command) {
            diagnostics.record_frame();
        }
    }

    diagnostics.set_alive(false);
}

struct RenderApiRenderer<'a> {
    render_context: libmpv2::render::RenderContext<'a>,
    gl_surface: glutin::surface::Surface<WindowSurface>,
    gl_context: glutin::context::PossiblyCurrentContext,
    _display: Box<Display>,
    width: i32,
    height: i32,
}

impl<'a> RenderApiRenderer<'a> {
    fn new(
        handles: RenderApiRawHandles,
        rect: NativeSurfaceRect,
        mpv_client: &'a Mpv,
    ) -> Result<Self, String> {
        let width = nonzero_surface_dimension(rect.width, "width")?;
        let height = nonzero_surface_dimension(rect.height, "height")?;
        let display = Box::new(unsafe {
            Display::new(handles.display, display_api_preference(handles.window))
                .map_err(|err| format!("native render-api could not create GL display: {err}"))?
        });
        let template = ConfigTemplateBuilder::new()
            .with_alpha_size(8)
            .with_depth_size(0)
            .prefer_hardware_accelerated(Some(true))
            .compatible_with_native_window(handles.window)
            .build();
        let config = unsafe {
            display
                .find_configs(template)
                .map_err(|err| format!("native render-api could not query GL configs: {err}"))?
                .max_by_key(|config| (config.hardware_accelerated(), config.num_samples()))
                .ok_or_else(|| "native render-api did not find a GL config".to_string())?
        };
        let surface_attributes =
            SurfaceAttributesBuilder::<WindowSurface>::new().build(handles.window, width, height);
        let gl_surface = unsafe {
            display
                .create_window_surface(&config, &surface_attributes)
                .map_err(|err| format!("native render-api could not create GL surface: {err}"))?
        };
        let context_attributes = ContextAttributesBuilder::new()
            .with_profile(GlProfile::Core)
            .build(Some(handles.window));
        let gl_context = unsafe {
            display
                .create_context(&config, &context_attributes)
                .map_err(|err| format!("native render-api could not create GL context: {err}"))?
        }
        .make_current(&gl_surface)
        .map_err(|err| format!("native render-api could not make GL context current: {err}"))?;
        let display_ptr = display.as_ref() as *const Display as usize;
        let mut render_params: Vec<RenderParam<usize>> = vec![
            RenderParam::ApiType(RenderParamApiType::OpenGl),
            RenderParam::InitParams(OpenGLInitParams {
                get_proc_address: render_api_get_proc_address,
                ctx: display_ptr,
            }),
        ];
        if let Some(display_handle) = render_api_display_handle(handles.display) {
            match display_handle {
                RenderApiDisplayHandle::X11(display) => {
                    render_params.push(RenderParam::X11Display(display));
                }
                RenderApiDisplayHandle::Wayland(display) => {
                    render_params.push(RenderParam::WaylandDisplay(display));
                }
            }
        }
        let render_context = mpv_client
            .create_render_context(render_params)
            .map_err(|err| format!("libmpv could not create render-api context: {err}"))?;

        Ok(Self {
            render_context,
            gl_surface,
            gl_context,
            _display: display,
            width: width.get() as i32,
            height: height.get() as i32,
        })
    }

    fn resize(&mut self, rect: NativeSurfaceRect) -> Result<(), String> {
        let width = nonzero_surface_dimension(rect.width, "width")?;
        let height = nonzero_surface_dimension(rect.height, "height")?;
        self.gl_surface.resize(&self.gl_context, width, height);
        self.width = width.get() as i32;
        self.height = height.get() as i32;
        self.render()
    }

    fn render(&mut self) -> Result<(), String> {
        self.render_context
            .render::<usize>(0, self.width, self.height, true)
            .map_err(|err| format!("libmpv render-api render failed: {err}"))?;
        self.gl_surface
            .swap_buffers(&self.gl_context)
            .map_err(|err| format!("native render-api could not swap buffers: {err}"))?;
        self.render_context.report_swap();
        Ok(())
    }
}

fn nonzero_surface_dimension(value: u32, label: &str) -> Result<NonZeroU32, String> {
    NonZeroU32::new(value)
        .ok_or_else(|| format!("native render-api surface {label} must be non-zero"))
}

fn render_api_get_proc_address(display_ptr: &usize, name: &str) -> *mut c_void {
    let Ok(name) = CString::new(name) else {
        return std::ptr::null_mut();
    };
    let display = unsafe { &*(*display_ptr as *const Display) };
    display.get_proc_address(&name).cast_mut()
}

fn render_api_display_handle(display: RawDisplayHandle) -> Option<RenderApiDisplayHandle> {
    match display {
        RawDisplayHandle::Xlib(handle) => handle
            .display
            .map(|display| RenderApiDisplayHandle::X11(display.as_ptr().cast_const())),
        RawDisplayHandle::Wayland(handle) => Some(RenderApiDisplayHandle::Wayland(
            handle.display.as_ptr().cast_const(),
        )),
        _ => None,
    }
}

#[cfg(target_os = "macos")]
fn display_api_preference(_window: RawWindowHandle) -> DisplayApiPreference {
    DisplayApiPreference::Cgl
}

#[cfg(windows)]
fn display_api_preference(window: RawWindowHandle) -> DisplayApiPreference {
    DisplayApiPreference::Wgl(Some(window))
}

#[cfg(all(unix, not(target_os = "macos")))]
fn display_api_preference(_window: RawWindowHandle) -> DisplayApiPreference {
    DisplayApiPreference::Egl
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

#[cfg(test)]
mod tests {
    use super::*;
    use raw_window_handle::{WaylandDisplayHandle, XcbDisplayHandle, XlibDisplayHandle};
    use std::ptr::NonNull;

    fn display_ptr() -> NonNull<c_void> {
        NonNull::new(1 as *mut c_void).expect("non-null display pointer")
    }

    #[test]
    fn render_api_uses_platform_display_params_when_available() {
        let xlib = display_ptr();
        let wayland = display_ptr();

        assert_eq!(
            render_api_display_handle(RawDisplayHandle::Xlib(XlibDisplayHandle::new(
                Some(xlib),
                0,
            ))),
            Some(RenderApiDisplayHandle::X11(xlib.as_ptr().cast_const()))
        );
        assert_eq!(
            render_api_display_handle(RawDisplayHandle::Wayland(WaylandDisplayHandle::new(
                wayland,
            ))),
            Some(RenderApiDisplayHandle::Wayland(
                wayland.as_ptr().cast_const()
            ))
        );
        assert_eq!(
            render_api_display_handle(RawDisplayHandle::Xcb(XcbDisplayHandle::new(
                Some(display_ptr()),
                0,
            ))),
            None
        );
    }

    #[test]
    fn render_diagnostics_count_only_render_callbacks_as_frames() {
        assert!(render_api_command_counts_frame(&RenderApiCommand::Render));
        assert!(!render_api_command_counts_frame(&RenderApiCommand::Resize(
            NativeSurfaceRect {
                x: 0,
                y: 0,
                width: 640,
                height: 360,
            }
        )));
        assert!(!render_api_command_counts_frame(
            &RenderApiCommand::Shutdown
        ));
    }
}
