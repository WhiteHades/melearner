use super::{NativeSurfaceRect, RenderApiDiagnostics, record_native_surface_runtime_log};
use libmpv2::Mpv;
use libmpv2_sys as mpv_sys;
use std::{
    cell::RefCell,
    collections::HashMap,
    ffi::{CStr, c_char, c_void},
    mem, ptr,
    sync::atomic::{AtomicU64, Ordering},
    sync::mpsc,
    time::Duration,
};
use tauri::WebviewWindow;
use windows::{
    Win32::{
        Foundation::{HMODULE, HWND},
        Graphics::{
            Gdi::{GetDC, HDC, ReleaseDC},
            OpenGL::{
                ChoosePixelFormat, HGLRC, PFD_DOUBLEBUFFER, PFD_DRAW_TO_WINDOW, PFD_MAIN_PLANE,
                PFD_SUPPORT_OPENGL, PFD_TYPE_RGBA, PIXELFORMATDESCRIPTOR, SetPixelFormat,
                SwapBuffers, wglCreateContext, wglDeleteContext, wglGetProcAddress, wglMakeCurrent,
            },
        },
        System::LibraryLoader::{GetModuleHandleA, GetModuleHandleW, GetProcAddress},
        UI::WindowsAndMessaging::{
            CS_OWNDC, CreateWindowExW, DefWindowProcW, DestroyWindow, HWND_TOP, RegisterClassW,
            SW_HIDE, SW_SHOW, SWP_NOACTIVATE, SWP_NOOWNERZORDER, SetWindowPos, ShowWindow,
            WINDOW_EX_STYLE, WNDCLASSW, WS_CHILD, WS_CLIPCHILDREN, WS_CLIPSIBLINGS, WS_VISIBLE,
        },
    },
    core::{PCSTR, PCWSTR, w},
};

static NEXT_WINDOWS_SURFACE_ID: AtomicU64 = AtomicU64::new(1);
const WINDOWS_SURFACE_CLASS: PCWSTR = w!("MelearnNativeVideoSurface");
const WINDOWS_SURFACE_TITLE: PCWSTR = w!("melearner native video");

thread_local! {
    static WINDOWS_SURFACES: RefCell<HashMap<u64, WindowsInWindowSurface>> = RefCell::new(HashMap::new());
}

pub(super) struct WindowsInWindowSurfaceHandle {
    id: u64,
    parent: WebviewWindow,
    diagnostics: RenderApiDiagnostics,
}

impl WindowsInWindowSurfaceHandle {
    pub(super) fn attach(parent: &WebviewWindow, rect: NativeSurfaceRect) -> Result<Self, String> {
        let id = NEXT_WINDOWS_SURFACE_ID.fetch_add(1, Ordering::Relaxed);
        let diagnostics = RenderApiDiagnostics::new();
        let handle = Self {
            id,
            parent: parent.clone(),
            diagnostics: diagnostics.clone(),
        };

        run_on_windows_thread(parent, move |parent| {
            let surface = WindowsInWindowSurface::new(id, parent, rect, diagnostics)?;
            WINDOWS_SURFACES.with(|surfaces| {
                surfaces.borrow_mut().insert(id, surface);
            });
            Ok(())
        })?;

        Ok(handle)
    }

    pub(super) fn attach_to_mpv(&self, mpv: &Mpv) -> Result<(), String> {
        let id = self.id;
        let mpv_client = mpv
            .create_client(None)
            .map_err(|err| format!("libmpv could not create windows render client: {err}"))?;

        run_on_windows_thread(&self.parent, move |_parent| {
            WINDOWS_SURFACES.with(|surfaces| {
                let mut surfaces = surfaces.borrow_mut();
                let surface = surfaces
                    .get_mut(&id)
                    .ok_or_else(|| "windows native video surface is missing".to_string())?;
                surface.attach_to_mpv(mpv_client)
            })
        })
    }

    pub(super) fn move_to(&self, rect: NativeSurfaceRect) -> Result<(), String> {
        let id = self.id;
        run_on_windows_thread(&self.parent, move |_parent| {
            WINDOWS_SURFACES.with(|surfaces| {
                let mut surfaces = surfaces.borrow_mut();
                let surface = surfaces
                    .get_mut(&id)
                    .ok_or_else(|| "windows native video surface is missing".to_string())?;
                surface.move_to(rect)
            })
        })
    }

    pub(super) fn set_visible(&self, visible: bool) -> Result<(), String> {
        let id = self.id;
        run_on_windows_thread(&self.parent, move |_parent| {
            WINDOWS_SURFACES.with(|surfaces| {
                let surfaces = surfaces.borrow();
                let surface = surfaces
                    .get(&id)
                    .ok_or_else(|| "windows native video surface is missing".to_string())?;
                surface.set_visible(visible);
                Ok(())
            })
        })
    }

    pub(super) fn diagnostics(&self) -> super::NativeSurfaceDiagnostics {
        self.diagnostics.snapshot()
    }
}

impl Drop for WindowsInWindowSurfaceHandle {
    fn drop(&mut self) {
        let id = self.id;
        let _ = run_on_windows_thread(&self.parent, move |_parent| {
            WINDOWS_SURFACES.with(|surfaces| {
                surfaces.borrow_mut().remove(&id);
            });
            Ok(())
        });
    }
}

fn run_on_windows_thread<T: Send + 'static>(
    parent: &WebviewWindow,
    task: impl FnOnce(WebviewWindow) -> Result<T, String> + Send + 'static,
) -> Result<T, String> {
    let parent_for_task = parent.clone();
    let (tx, rx) = mpsc::channel();
    parent
        .run_on_main_thread(move || {
            let _ = tx.send(task(parent_for_task));
        })
        .map_err(|err| format!("native player could not schedule windows surface work: {err}"))?;
    rx.recv()
        .map_err(|err| format!("native player windows surface work did not finish: {err}"))?
}

struct WindowsInWindowSurface {
    id: u64,
    parent: WebviewWindow,
    hwnd: HWND,
    hdc: HDC,
    gl_context: HGLRC,
    render_state: WindowsRenderState,
}

impl WindowsInWindowSurface {
    fn new(
        id: u64,
        parent: WebviewWindow,
        rect: NativeSurfaceRect,
        diagnostics: RenderApiDiagnostics,
    ) -> Result<Self, String> {
        register_surface_class();
        let parent_hwnd = parent
            .hwnd()
            .map_err(|err| format!("native player could not read parent HWND: {err}"))?;
        if parent_hwnd.is_invalid() {
            return Err("native player parent HWND is invalid".to_string());
        }

        let hwnd = unsafe {
            CreateWindowExW(
                WINDOW_EX_STYLE::default(),
                WINDOWS_SURFACE_CLASS,
                WINDOWS_SURFACE_TITLE,
                WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                rect.x,
                rect.y,
                surface_length_to_i32(rect.width),
                surface_length_to_i32(rect.height),
                Some(parent_hwnd),
                None,
                GetModuleHandleW(None).ok().map(|module| module.into()),
                None,
            )
        }
        .map_err(|err| format!("native player could not create child HWND: {err}"))?;

        let hdc = unsafe { GetDC(Some(hwnd)) };
        if hdc.is_invalid() {
            unsafe {
                let _ = DestroyWindow(hwnd);
            }
            return Err("native player could not get child HWND device context".to_string());
        }

        if let Err(err) = configure_pixel_format(hdc) {
            unsafe {
                let _ = ReleaseDC(Some(hwnd), hdc);
                let _ = DestroyWindow(hwnd);
            }
            return Err(err);
        }

        let gl_context = unsafe { wglCreateContext(hdc) }
            .map_err(|err| format!("native player could not create WGL context: {err}"))?;

        let surface = Self {
            id,
            parent,
            hwnd,
            hdc,
            gl_context,
            render_state: WindowsRenderState::new(id, diagnostics),
        };
        surface.move_to(rect)?;
        Ok(surface)
    }

    fn attach_to_mpv(&mut self, mpv_client: Mpv) -> Result<(), String> {
        self.render_state.attach_to_mpv(mpv_client);
        self.realize();
        self.schedule_render();
        Ok(())
    }

    fn move_to(&self, rect: NativeSurfaceRect) -> Result<(), String> {
        unsafe {
            SetWindowPos(
                self.hwnd,
                Some(HWND_TOP),
                rect.x,
                rect.y,
                surface_length_to_i32(rect.width),
                surface_length_to_i32(rect.height),
                SWP_NOACTIVATE | SWP_NOOWNERZORDER,
            )
        }
        .map_err(|err| format!("native player could not move child HWND: {err}"))
    }

    fn set_visible(&self, visible: bool) {
        let command = if visible { SW_SHOW } else { SW_HIDE };
        unsafe {
            let _ = ShowWindow(self.hwnd, command);
        }
        if visible {
            self.schedule_render();
        }
    }

    fn realize(&mut self) {
        self.render_state
            .realize(&self.parent, self.hdc, self.gl_context);
    }

    fn render_now(&mut self) {
        self.render_state
            .render(&self.parent, self.hdc, self.gl_context);
    }

    fn schedule_render(&self) {
        let id = self.id;
        let parent = self.parent.clone();
        std::thread::spawn(move || {
            std::thread::sleep(Duration::from_millis(16));
            let _ = parent.run_on_main_thread(move || {
                dispatch_windows_render(id);
            });
        });
    }
}

impl Drop for WindowsInWindowSurface {
    fn drop(&mut self) {
        self.render_state.unrealize();
        unsafe {
            let _ = wglMakeCurrent(HDC::default(), HGLRC::default());
            let _ = wglDeleteContext(self.gl_context);
            let _ = ReleaseDC(Some(self.hwnd), self.hdc);
            let _ = DestroyWindow(self.hwnd);
        }
    }
}

fn dispatch_windows_render(id: u64) {
    WINDOWS_SURFACES.with(|surfaces| {
        let Ok(mut surfaces) = surfaces.try_borrow_mut() else {
            return;
        };
        if let Some(surface) = surfaces.get_mut(&id) {
            surface.render_now();
        }
    });
}

struct WindowsRenderState {
    id: u64,
    mpv_client: Option<Mpv>,
    renderer: Option<WindowsMpvRenderer>,
    diagnostics: RenderApiDiagnostics,
    logged_first_frame: bool,
}

impl WindowsRenderState {
    fn new(id: u64, diagnostics: RenderApiDiagnostics) -> Self {
        Self {
            id,
            mpv_client: None,
            renderer: None,
            diagnostics,
            logged_first_frame: false,
        }
    }

    fn attach_to_mpv(&mut self, mpv_client: Mpv) {
        self.mpv_client = Some(mpv_client);
    }

    fn realize(&mut self, parent: &WebviewWindow, hdc: HDC, gl_context: HGLRC) {
        if let Err(err) = unsafe { wglMakeCurrent(hdc, gl_context) } {
            self.record_error(format!(
                "native player could not make WGL context current: {err}"
            ));
            return;
        }

        if self.renderer.is_none() {
            let Some(mpv_client) = self.mpv_client.as_ref() else {
                return;
            };
            match WindowsMpvRenderer::new(self.id, parent, mpv_client) {
                Ok(renderer) => {
                    self.renderer = Some(renderer);
                    self.diagnostics.set_alive(true);
                }
                Err(err) => {
                    self.record_error(err);
                }
            }
        }
    }

    fn render(&mut self, parent: &WebviewWindow, hdc: HDC, gl_context: HGLRC) {
        self.realize(parent, hdc, gl_context);
        let Some(renderer) = self.renderer.as_mut() else {
            return;
        };

        let (width, height) = match window_size_for_hdc(hdc) {
            Some(size) => size,
            None => {
                self.record_error("native player could not read WGL drawable size".to_string());
                return;
            }
        };

        match renderer.render(width, height) {
            Ok(update_flags) => {
                if let Err(err) = unsafe { SwapBuffers(hdc) } {
                    self.record_error(format!("native player could not swap WGL buffers: {err}"));
                    return;
                }
                self.diagnostics.set_alive(true);
                self.diagnostics
                    .record_frame(width as u32, height as u32, update_flags);
                if !self.logged_first_frame {
                    record_native_surface_runtime_log(&format!(
                        "native windows render-api submitted first frame: {width}x{height}, update_flags={update_flags}"
                    ));
                    self.logged_first_frame = true;
                }
            }
            Err(err) => self.record_error(err),
        }
    }

    fn unrealize(&mut self) {
        self.renderer.take();
        self.diagnostics.set_alive(false);
    }

    fn record_error(&self, err: String) {
        log::error!("{err}");
        self.diagnostics.record_error(&err);
        self.diagnostics.set_alive(false);
    }
}

struct WindowsMpvRenderer {
    context: *mut mpv_sys::mpv_render_context,
    callback_ctx: *mut c_void,
}

impl WindowsMpvRenderer {
    fn new(id: u64, parent: &WebviewWindow, mpv_client: &Mpv) -> Result<Self, String> {
        let mut init_params = mpv_sys::mpv_opengl_init_params {
            get_proc_address: Some(windows_get_proc_address),
            get_proc_address_ctx: ptr::null_mut(),
        };
        let mut params = [
            mpv_sys::mpv_render_param {
                type_: mpv_sys::mpv_render_param_type_MPV_RENDER_PARAM_API_TYPE,
                data: mpv_sys::MPV_RENDER_API_TYPE_OPENGL.as_ptr() as *mut c_void,
            },
            mpv_sys::mpv_render_param {
                type_: mpv_sys::mpv_render_param_type_MPV_RENDER_PARAM_OPENGL_INIT_PARAMS,
                data: (&mut init_params as *mut mpv_sys::mpv_opengl_init_params).cast(),
            },
            mpv_sys::mpv_render_param {
                type_: mpv_sys::mpv_render_param_type_MPV_RENDER_PARAM_INVALID,
                data: ptr::null_mut(),
            },
        ];
        let mut context = ptr::null_mut();
        let result = unsafe {
            mpv_sys::mpv_render_context_create(
                &mut context,
                mpv_client.ctx.as_ptr(),
                params.as_mut_ptr(),
            )
        };
        if result < 0 || context.is_null() {
            return Err(format!(
                "libmpv could not create windows render context: {}",
                mpv_error_message(result)
            ));
        }

        let callback_ctx = Box::into_raw(Box::new(WindowsRenderCallback {
            id,
            parent: parent.clone(),
        }))
        .cast::<c_void>();
        unsafe {
            mpv_sys::mpv_render_context_set_update_callback(
                context,
                Some(windows_mpv_update_callback),
                callback_ctx,
            );
        }

        Ok(Self {
            context,
            callback_ctx,
        })
    }

    fn render(&mut self, width: i32, height: i32) -> Result<u64, String> {
        let update_flags = unsafe { mpv_sys::mpv_render_context_update(self.context) };
        let mut fbo = mpv_sys::mpv_opengl_fbo {
            fbo: 0,
            w: width,
            h: height,
            internal_format: 0,
        };
        let mut flip_y = 1;
        let mut params = [
            mpv_sys::mpv_render_param {
                type_: mpv_sys::mpv_render_param_type_MPV_RENDER_PARAM_OPENGL_FBO,
                data: (&mut fbo as *mut mpv_sys::mpv_opengl_fbo).cast(),
            },
            mpv_sys::mpv_render_param {
                type_: mpv_sys::mpv_render_param_type_MPV_RENDER_PARAM_FLIP_Y,
                data: (&mut flip_y as *mut i32).cast(),
            },
            mpv_sys::mpv_render_param {
                type_: mpv_sys::mpv_render_param_type_MPV_RENDER_PARAM_INVALID,
                data: ptr::null_mut(),
            },
        ];
        let result =
            unsafe { mpv_sys::mpv_render_context_render(self.context, params.as_mut_ptr()) };
        if result < 0 {
            return Err(format!(
                "libmpv windows render failed: {}",
                mpv_error_message(result)
            ));
        }
        unsafe {
            mpv_sys::mpv_render_context_report_swap(self.context);
        }
        Ok(update_flags)
    }
}

impl Drop for WindowsMpvRenderer {
    fn drop(&mut self) {
        unsafe {
            mpv_sys::mpv_render_context_set_update_callback(self.context, None, ptr::null_mut());
            mpv_sys::mpv_render_context_free(self.context);
            drop(Box::from_raw(
                self.callback_ctx.cast::<WindowsRenderCallback>(),
            ));
        }
    }
}

struct WindowsRenderCallback {
    id: u64,
    parent: WebviewWindow,
}

unsafe extern "C" fn windows_mpv_update_callback(ctx: *mut c_void) {
    if ctx.is_null() {
        return;
    }
    let callback = unsafe { &*ctx.cast::<WindowsRenderCallback>() };
    let id = callback.id;
    let parent = callback.parent.clone();
    std::thread::spawn(move || {
        std::thread::sleep(Duration::from_millis(1));
        let _ = parent.run_on_main_thread(move || {
            dispatch_windows_render(id);
        });
    });
}

unsafe extern "C" fn windows_get_proc_address(
    _ctx: *mut c_void,
    name: *const c_char,
) -> *mut c_void {
    if name.is_null() {
        return ptr::null_mut();
    }
    find_gl_symbol(name).unwrap_or(ptr::null_mut())
}

fn find_gl_symbol(name: *const c_char) -> Option<*mut c_void> {
    let proc = unsafe { wglGetProcAddress(PCSTR(name.cast())) };
    if let Some(proc) = proc {
        let ptr = proc as *mut c_void;
        if !ptr.is_null() {
            return Some(ptr);
        }
    }

    let module = unsafe { GetModuleHandleA(PCSTR(c"opengl32.dll".as_ptr().cast())) }.ok()?;
    let proc = unsafe { GetProcAddress(module, PCSTR(name.cast())) }?;
    Some(proc as *mut c_void)
}

unsafe extern "system" fn surface_window_proc(
    hwnd: HWND,
    msg: u32,
    wparam: windows::Win32::Foundation::WPARAM,
    lparam: windows::Win32::Foundation::LPARAM,
) -> windows::Win32::Foundation::LRESULT {
    unsafe { DefWindowProcW(hwnd, msg, wparam, lparam) }
}

fn register_surface_class() {
    let instance: Option<HMODULE> = unsafe { GetModuleHandleW(None).ok() };
    let class = WNDCLASSW {
        style: CS_OWNDC,
        lpfnWndProc: Some(surface_window_proc),
        hInstance: instance.map(Into::into).unwrap_or_default(),
        lpszClassName: WINDOWS_SURFACE_CLASS,
        ..Default::default()
    };
    unsafe {
        RegisterClassW(&class);
    }
}

fn configure_pixel_format(hdc: HDC) -> Result<(), String> {
    let pfd = PIXELFORMATDESCRIPTOR {
        nSize: mem::size_of::<PIXELFORMATDESCRIPTOR>() as u16,
        nVersion: 1,
        dwFlags: PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
        iPixelType: PFD_TYPE_RGBA,
        cColorBits: 32,
        cDepthBits: 0,
        cStencilBits: 0,
        iLayerType: PFD_MAIN_PLANE.0 as u8,
        ..Default::default()
    };
    let format = unsafe { ChoosePixelFormat(hdc, &pfd) };
    if format <= 0 {
        return Err("native player could not choose WGL pixel format".to_string());
    }
    unsafe { SetPixelFormat(hdc, format, &pfd) }
        .map_err(|err| format!("native player could not set WGL pixel format: {err}"))
}

fn window_size_for_hdc(hdc: HDC) -> Option<(i32, i32)> {
    let hwnd = unsafe { windows::Win32::Graphics::Gdi::WindowFromDC(hdc) };
    if hwnd.is_invalid() {
        return None;
    }
    let mut rect = windows::Win32::Foundation::RECT::default();
    unsafe { windows::Win32::UI::WindowsAndMessaging::GetClientRect(hwnd, &mut rect).ok()? };
    Some((
        (rect.right - rect.left).max(1),
        (rect.bottom - rect.top).max(1),
    ))
}

fn surface_length_to_i32(value: u32) -> i32 {
    i32::try_from(value).unwrap_or(i32::MAX)
}

fn mpv_error_message(code: i32) -> String {
    let message = unsafe { mpv_sys::mpv_error_string(code) };
    if message.is_null() {
        return code.to_string();
    }
    unsafe { CStr::from_ptr(message) }
        .to_string_lossy()
        .into_owned()
}
