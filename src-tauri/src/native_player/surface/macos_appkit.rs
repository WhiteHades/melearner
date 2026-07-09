#![allow(deprecated)]

use super::{NativeSurfaceRect, RenderApiDiagnostics, record_native_surface_runtime_log};
use libmpv2::Mpv;
use libmpv2_sys as mpv_sys;
use objc2::{
    ClassType, MainThreadMarker, MainThreadOnly,
    rc::{Retained, autoreleasepool},
};
use objc2_app_kit::{NSOpenGLView, NSView};
use objc2_foundation::{NSPoint, NSRect, NSSize};
use std::{
    cell::RefCell,
    collections::HashMap,
    ffi::{CStr, c_char, c_void},
    ptr,
    sync::atomic::{AtomicU64, Ordering},
    sync::mpsc,
    time::Duration,
};
use tauri::WebviewWindow;

static NEXT_MACOS_SURFACE_ID: AtomicU64 = AtomicU64::new(1);

thread_local! {
    static MACOS_SURFACES: RefCell<HashMap<u64, MacosInWindowSurface>> = RefCell::new(HashMap::new());
}

pub(super) struct MacosInWindowSurfaceHandle {
    id: u64,
    parent: WebviewWindow,
    diagnostics: RenderApiDiagnostics,
}

impl MacosInWindowSurfaceHandle {
    pub(super) fn attach(parent: &WebviewWindow, rect: NativeSurfaceRect) -> Result<Self, String> {
        let id = NEXT_MACOS_SURFACE_ID.fetch_add(1, Ordering::Relaxed);
        let diagnostics = RenderApiDiagnostics::new();
        let handle = Self {
            id,
            parent: parent.clone(),
            diagnostics: diagnostics.clone(),
        };

        let parent_for_surface = parent.clone();
        run_on_macos_webview_thread(parent, move |webview| {
            let surface =
                MacosInWindowSurface::new(id, parent_for_surface, webview, rect, diagnostics)?;
            MACOS_SURFACES.with(|surfaces| {
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
            .map_err(|err| format!("libmpv could not create macos render client: {err}"))?;

        run_on_macos_webview_thread(&self.parent, move |_webview| {
            MACOS_SURFACES.with(|surfaces| {
                let mut surfaces = surfaces.borrow_mut();
                let surface = surfaces
                    .get_mut(&id)
                    .ok_or_else(|| "macos native video surface is missing".to_string())?;
                surface.attach_to_mpv(mpv_client)
            })
        })
    }

    pub(super) fn move_to(&self, rect: NativeSurfaceRect) -> Result<(), String> {
        let id = self.id;
        run_on_macos_webview_thread(&self.parent, move |_webview| {
            MACOS_SURFACES.with(|surfaces| {
                let mut surfaces = surfaces.borrow_mut();
                let surface = surfaces
                    .get_mut(&id)
                    .ok_or_else(|| "macos native video surface is missing".to_string())?;
                surface.move_to(rect);
                Ok(())
            })
        })
    }

    pub(super) fn set_visible(&self, visible: bool) -> Result<(), String> {
        let id = self.id;
        run_on_macos_webview_thread(&self.parent, move |_webview| {
            MACOS_SURFACES.with(|surfaces| {
                let surfaces = surfaces.borrow();
                let surface = surfaces
                    .get(&id)
                    .ok_or_else(|| "macos native video surface is missing".to_string())?;
                surface.set_visible(visible);
                Ok(())
            })
        })
    }

    pub(super) fn diagnostics(&self) -> super::NativeSurfaceDiagnostics {
        self.diagnostics.snapshot()
    }
}

impl Drop for MacosInWindowSurfaceHandle {
    fn drop(&mut self) {
        let id = self.id;
        let _ = run_on_macos_webview_thread(&self.parent, move |_webview| {
            MACOS_SURFACES.with(|surfaces| {
                surfaces.borrow_mut().remove(&id);
            });
            Ok(())
        });
    }
}

fn run_on_macos_webview_thread<T: Send + 'static>(
    parent: &WebviewWindow,
    task: impl FnOnce(tauri::webview::PlatformWebview) -> Result<T, String> + Send + 'static,
) -> Result<T, String> {
    let (tx, rx) = mpsc::channel();
    parent
        .with_webview(move |webview| {
            let _ = tx.send(task(webview));
        })
        .map_err(|err| format!("native player could not schedule macos surface work: {err}"))?;
    rx.recv()
        .map_err(|err| format!("native player macos surface work did not finish: {err}"))?
}

struct MacosInWindowSurface {
    id: u64,
    parent: WebviewWindow,
    view: Retained<NSOpenGLView>,
    render_state: MacosRenderState,
}

impl MacosInWindowSurface {
    fn new(
        id: u64,
        parent: WebviewWindow,
        webview: tauri::webview::PlatformWebview,
        rect: NativeSurfaceRect,
        diagnostics: RenderApiDiagnostics,
    ) -> Result<Self, String> {
        autoreleasepool(|_| {
            let mtm = MainThreadMarker::new().ok_or_else(|| {
                "macos native video surface must be created on the main thread".to_string()
            })?;
            let webview = unsafe { &*webview.inner().cast::<NSView>() };
            let frame = ns_rect_for_surface_in_view(webview, rect);
            let pixel_format = NSOpenGLView::defaultPixelFormat(mtm);
            let view = NSOpenGLView::initWithFrame_pixelFormat(
                NSOpenGLView::alloc(mtm),
                frame,
                Some(&pixel_format),
            )
            .ok_or_else(|| {
                "macos native video surface could not create NSOpenGLView".to_string()
            })?;

            view.setWantsBestResolutionOpenGLSurface(true);
            view.setHidden(false);
            webview.addSubview(view.as_super());

            Ok(Self {
                id,
                parent,
                view,
                render_state: MacosRenderState::new(id, diagnostics),
            })
        })
    }

    fn attach_to_mpv(&mut self, mpv_client: Mpv) -> Result<(), String> {
        self.render_state.attach_to_mpv(mpv_client);
        self.render_state.realize(&self.parent, &self.view);
        self.schedule_render();
        Ok(())
    }

    fn move_to(&mut self, rect: NativeSurfaceRect) {
        if let Some(superview) = unsafe { self.view.superview() } {
            self.view
                .setFrame(ns_rect_for_surface_in_view(&superview, rect));
        } else {
            self.view.setFrame(ns_rect_for_surface(rect));
        }
        self.view.update();
        self.schedule_render();
    }

    fn set_visible(&self, visible: bool) {
        self.view.setHidden(!visible);
        if visible {
            self.schedule_render();
        }
    }

    fn schedule_render(&self) {
        let id = self.id;
        let parent = self.parent.clone();
        std::thread::spawn(move || {
            std::thread::sleep(Duration::from_millis(16));
            let _ = parent.run_on_main_thread(move || {
                dispatch_macos_render(id);
            });
        });
    }
}

impl Drop for MacosInWindowSurface {
    fn drop(&mut self) {
        self.render_state.unrealize();
        self.view.removeFromSuperview();
    }
}

fn dispatch_macos_render(id: u64) {
    MACOS_SURFACES.with(|surfaces| {
        if let Some(surface) = surfaces.borrow_mut().get_mut(&id) {
            surface.render_now();
        }
    });
}

struct MacosRenderState {
    id: u64,
    mpv_client: Option<Mpv>,
    renderer: Option<MacosMpvRenderer>,
    diagnostics: RenderApiDiagnostics,
    logged_first_frame: bool,
}

impl MacosRenderState {
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

    fn realize(&mut self, parent: &WebviewWindow, view: &NSOpenGLView) {
        let Some(context) = view.openGLContext() else {
            self.record_error("macos native video surface has no OpenGL context".to_string());
            return;
        };
        context.makeCurrentContext();
        context.update(view.mtm());

        if self.renderer.is_none() {
            let Some(mpv_client) = self.mpv_client.as_ref() else {
                return;
            };
            match MacosMpvRenderer::new(self.id, parent, mpv_client) {
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

    fn render(&mut self, parent: &WebviewWindow, view: &NSOpenGLView) {
        let Some(context) = view.openGLContext() else {
            self.record_error("macos native video surface has no OpenGL context".to_string());
            return;
        };
        context.makeCurrentContext();
        context.update(view.mtm());

        let frame = view.frame();
        let width = (frame.size.width.round() as i32).max(1);
        let height = (frame.size.height.round() as i32).max(1);
        let Some(renderer) = self.renderer.as_mut() else {
            self.realize(parent, view);
            return;
        };

        match renderer.render(width, height) {
            Ok(update_flags) => {
                context.flushBuffer();
                self.diagnostics.set_alive(true);
                self.diagnostics
                    .record_frame(width as u32, height as u32, update_flags);
                if !self.logged_first_frame {
                    record_native_surface_runtime_log(&format!(
                        "native macos render-api submitted first frame: {width}x{height}, update_flags={update_flags}"
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

struct MacosMpvRenderer {
    context: *mut mpv_sys::mpv_render_context,
    callback_ctx: *mut c_void,
}

impl MacosMpvRenderer {
    fn new(id: u64, parent: &WebviewWindow, mpv_client: &Mpv) -> Result<Self, String> {
        let mut init_params = mpv_sys::mpv_opengl_init_params {
            get_proc_address: Some(macos_get_proc_address),
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
                "libmpv could not create macos render context: {}",
                mpv_error_message(result)
            ));
        }

        let callback_ctx = Box::into_raw(Box::new(MacosRenderCallback {
            id,
            parent: parent.clone(),
        }))
        .cast::<c_void>();
        unsafe {
            mpv_sys::mpv_render_context_set_update_callback(
                context,
                Some(macos_mpv_update_callback),
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
                "libmpv macos render failed: {}",
                mpv_error_message(result)
            ));
        }
        unsafe {
            mpv_sys::mpv_render_context_report_swap(self.context);
        }
        Ok(update_flags)
    }
}

impl Drop for MacosMpvRenderer {
    fn drop(&mut self) {
        unsafe {
            mpv_sys::mpv_render_context_set_update_callback(self.context, None, ptr::null_mut());
            mpv_sys::mpv_render_context_free(self.context);
            drop(Box::from_raw(
                self.callback_ctx.cast::<MacosRenderCallback>(),
            ));
        }
    }
}

struct MacosRenderCallback {
    id: u64,
    parent: WebviewWindow,
}

unsafe extern "C" fn macos_mpv_update_callback(ctx: *mut c_void) {
    if ctx.is_null() {
        return;
    }
    let callback = unsafe { &*ctx.cast::<MacosRenderCallback>() };
    let id = callback.id;
    let parent = callback.parent.clone();
    let _ = parent.run_on_main_thread(move || {
        dispatch_macos_render(id);
    });
}

unsafe extern "C" fn macos_get_proc_address(_ctx: *mut c_void, name: *const c_char) -> *mut c_void {
    if name.is_null() {
        return ptr::null_mut();
    }
    find_gl_symbol(name).unwrap_or(ptr::null_mut())
}

fn find_gl_symbol(name: *const c_char) -> Option<*mut c_void> {
    unsafe {
        let library = libloading::os::unix::Library::open(
            Some("/System/Library/Frameworks/OpenGL.framework/OpenGL"),
            libloading::os::unix::RTLD_NOW,
        )
        .ok()?;
        let symbol = CStr::from_ptr(name).to_bytes_with_nul();
        library
            .get::<*mut c_void>(symbol)
            .ok()
            .map(|symbol| *symbol)
            .filter(|symbol| !symbol.is_null())
    }
}

fn ns_rect_for_surface(rect: NativeSurfaceRect) -> NSRect {
    NSRect::new(
        NSPoint::new(rect.x as f64, rect.y as f64),
        NSSize::new(rect.width as f64, rect.height as f64),
    )
}

fn ns_rect_for_surface_in_view(view: &NSView, rect: NativeSurfaceRect) -> NSRect {
    let y = if view.isFlipped() {
        rect.y as f64
    } else {
        let frame = view.frame();
        frame.size.height - rect.y as f64 - rect.height as f64
    };
    NSRect::new(
        NSPoint::new(rect.x as f64, y),
        NSSize::new(rect.width as f64, rect.height as f64),
    )
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
