use super::{NativeSurfaceRect, RenderApiDiagnostics, record_native_surface_runtime_log};
use gtk::OverlaySignals;
use gtk::prelude::*;
use libmpv2::Mpv;
use libmpv2_sys as mpv_sys;
use std::{
    cell::RefCell,
    collections::HashMap,
    ffi::{CStr, c_char, c_void},
    ptr,
    rc::Rc,
    sync::atomic::{AtomicU64, Ordering},
    sync::mpsc,
    time::Duration,
};
use tauri::WebviewWindow;

static NEXT_GTK_SURFACE_ID: AtomicU64 = AtomicU64::new(1);

thread_local! {
    static GTK_HOST: RefCell<Option<GtkOverlayHost>> = const { RefCell::new(None) };
    static GTK_SURFACES: RefCell<HashMap<u64, GtkInWindowSurface>> = RefCell::new(HashMap::new());
}

pub(super) struct GtkInWindowSurfaceHandle {
    id: u64,
    parent: WebviewWindow,
    diagnostics: RenderApiDiagnostics,
}

impl GtkInWindowSurfaceHandle {
    pub(super) fn attach(parent: &WebviewWindow, rect: NativeSurfaceRect) -> Result<Self, String> {
        let id = NEXT_GTK_SURFACE_ID.fetch_add(1, Ordering::Relaxed);
        let diagnostics = RenderApiDiagnostics::new();
        let handle = Self {
            id,
            parent: parent.clone(),
            diagnostics: diagnostics.clone(),
        };

        run_on_gtk_thread(parent, move |parent| {
            let host = ensure_host(&parent)?;
            let surface = GtkInWindowSurface::new(id, host, rect, diagnostics)?;
            GTK_SURFACES.with(|surfaces| {
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
            .map_err(|err| format!("libmpv could not create gtk render client: {err}"))?;

        run_on_gtk_thread(&self.parent, move |_parent| {
            GTK_SURFACES.with(|surfaces| {
                let mut surfaces = surfaces.borrow_mut();
                let surface = surfaces
                    .get_mut(&id)
                    .ok_or_else(|| "gtk native video surface is missing".to_string())?;
                surface.attach_to_mpv(mpv_client)
            })
        })
    }

    pub(super) fn move_to(&self, rect: NativeSurfaceRect) -> Result<(), String> {
        let id = self.id;
        run_on_gtk_thread(&self.parent, move |_parent| {
            GTK_SURFACES.with(|surfaces| {
                let mut surfaces = surfaces.borrow_mut();
                let surface = surfaces
                    .get_mut(&id)
                    .ok_or_else(|| "gtk native video surface is missing".to_string())?;
                surface.move_to(rect);
                Ok(())
            })
        })
    }

    pub(super) fn set_visible(&self, visible: bool) -> Result<(), String> {
        let id = self.id;
        run_on_gtk_thread(&self.parent, move |_parent| {
            GTK_SURFACES.with(|surfaces| {
                let surfaces = surfaces.borrow();
                let surface = surfaces
                    .get(&id)
                    .ok_or_else(|| "gtk native video surface is missing".to_string())?;
                surface.set_visible(visible);
                Ok(())
            })
        })
    }

    pub(super) fn request_render(&self) -> Result<(), String> {
        let id = self.id;
        run_on_gtk_thread(&self.parent, move |_parent| {
            GTK_SURFACES.with(|surfaces| {
                let surfaces = surfaces.borrow();
                let surface = surfaces
                    .get(&id)
                    .ok_or_else(|| "gtk native video surface is missing".to_string())?;
                surface.request_render()
            })
        })
    }

    pub(super) fn diagnostics(&self) -> super::NativeSurfaceDiagnostics {
        self.diagnostics.snapshot()
    }
}

impl Drop for GtkInWindowSurfaceHandle {
    fn drop(&mut self) {
        let id = self.id;
        let _ = run_on_gtk_thread(&self.parent, move |_parent| {
            GTK_SURFACES.with(|surfaces| {
                surfaces.borrow_mut().remove(&id);
            });
            Ok(())
        });
    }
}

fn run_on_gtk_thread<T: Send + 'static>(
    parent: &WebviewWindow,
    task: impl FnOnce(WebviewWindow) -> Result<T, String> + Send + 'static,
) -> Result<T, String> {
    let parent_for_task = parent.clone();
    let (tx, rx) = mpsc::channel();
    parent
        .run_on_main_thread(move || {
            let _ = tx.send(task(parent_for_task));
        })
        .map_err(|err| format!("native player could not schedule gtk surface work: {err}"))?;
    rx.recv()
        .map_err(|err| format!("native player gtk surface work did not finish: {err}"))?
}

#[derive(Clone)]
struct GtkOverlayHost {
    overlay: gtk::Overlay,
    layer_allocation: Rc<RefCell<gtk::Rectangle>>,
}

fn ensure_host(parent: &WebviewWindow) -> Result<GtkOverlayHost, String> {
    GTK_HOST.with(|host| {
        if let Some(host) = host.borrow().clone() {
            return Ok(host);
        }

        let vbox = parent
            .default_vbox()
            .map_err(|err| format!("native player could not read gtk webview container: {err}"))?;
        let children = vbox.children();
        if children.is_empty() {
            return Err("native player gtk webview container is empty".to_string());
        }

        let overlay = gtk::Overlay::new();
        overlay.set_hexpand(true);
        overlay.set_vexpand(true);

        let content = gtk::Box::new(gtk::Orientation::Vertical, 0);
        content.set_hexpand(true);
        content.set_vexpand(true);

        for child in children {
            vbox.remove(&child);
            content.pack_start(&child, true, true, 0);
        }

        overlay.add(&content);

        let layer_allocation = Rc::new(RefCell::new(gtk::Rectangle::new(0, 0, 1, 1)));
        let layer_allocation_for_signal = layer_allocation.clone();
        overlay.connect_get_child_position(move |_overlay, _child| {
            Some(layer_allocation_for_signal.borrow().clone())
        });

        vbox.pack_start(&overlay, true, true, 0);
        overlay.show_all();

        let new_host = GtkOverlayHost {
            overlay,
            layer_allocation,
        };
        *host.borrow_mut() = Some(new_host.clone());
        Ok(new_host)
    })
}

struct GtkInWindowSurface {
    overlay: gtk::Overlay,
    gl_area: gtk::GLArea,
    layer_allocation: Rc<RefCell<gtk::Rectangle>>,
    render_state: Rc<RefCell<GtkRenderState>>,
}

impl GtkInWindowSurface {
    fn new(
        id: u64,
        host: GtkOverlayHost,
        rect: NativeSurfaceRect,
        diagnostics: RenderApiDiagnostics,
    ) -> Result<Self, String> {
        let gl_area = gtk::GLArea::new();
        gl_area.set_has_alpha(false);
        gl_area.set_auto_render(false);
        gl_area.set_halign(gtk::Align::Start);
        gl_area.set_valign(gtk::Align::Start);
        gl_area.set_hexpand(false);
        gl_area.set_vexpand(false);
        gl_area.set_no_show_all(true);

        let render_state = Rc::new(RefCell::new(GtkRenderState::new(id, diagnostics)));

        {
            let state = render_state.clone();
            gl_area.connect_realize(move |area| {
                state.borrow_mut().realize(area);
            });
        }
        {
            let state = render_state.clone();
            gl_area.connect_render(move |area, _context| {
                state.borrow_mut().render(area);
                gtk::glib::Propagation::Proceed
            });
        }
        {
            let state = render_state.clone();
            gl_area.connect_unrealize(move |_area| {
                state.borrow_mut().unrealize();
            });
        }

        apply_rect(&host.overlay, &host.layer_allocation, &gl_area, rect, true);
        Ok(Self {
            overlay: host.overlay,
            gl_area,
            layer_allocation: host.layer_allocation,
            render_state,
        })
    }

    fn attach_to_mpv(&mut self, mpv_client: Mpv) -> Result<(), String> {
        self.render_state.borrow_mut().attach_to_mpv(mpv_client);
        self.gl_area.show();
        if !self.gl_area.is_realized() {
            self.gl_area.realize();
        }
        self.render_state.borrow_mut().realize(&self.gl_area);
        self.schedule_render();
        Ok(())
    }

    fn move_to(&mut self, rect: NativeSurfaceRect) {
        apply_rect(
            &self.overlay,
            &self.layer_allocation,
            &self.gl_area,
            rect,
            false,
        );
        self.schedule_render();
    }

    fn set_visible(&self, visible: bool) {
        if visible {
            self.gl_area.show();
            self.schedule_render();
        } else {
            self.gl_area.hide();
        }
    }

    fn request_render(&self) -> Result<(), String> {
        if !self.gl_area.is_realized() {
            self.gl_area.realize();
        }
        {
            let mut state = self.render_state.borrow_mut();
            state.realize(&self.gl_area);
        }
        self.schedule_render();

        match self.render_state.borrow().diagnostics.snapshot().last_error {
            Some(err) => Err(err),
            None => Ok(()),
        }
    }

    fn schedule_render(&self) {
        self.gl_area.queue_resize();
        let gl_area = self.gl_area.clone();
        let layer_allocation = self.layer_allocation.clone();
        let render_state = self.render_state.clone();
        gtk::glib::timeout_add_local_once(Duration::from_millis(50), move || {
            gl_area.size_allocate(&layer_allocation.borrow());
            {
                let mut state = render_state.borrow_mut();
                state.realize(&gl_area);
                state.render(&gl_area);
            }
            queue_gl_area_render(&gl_area);
        });
    }
}

impl Drop for GtkInWindowSurface {
    fn drop(&mut self) {
        self.render_state.borrow_mut().unrealize();
        self.overlay.remove(&self.gl_area);
    }
}

fn apply_rect(
    overlay: &gtk::Overlay,
    layer_allocation: &Rc<RefCell<gtk::Rectangle>>,
    gl_area: &gtk::GLArea,
    rect: NativeSurfaceRect,
    add: bool,
) {
    let width = surface_length_to_i32(rect.width);
    let height = surface_length_to_i32(rect.height);
    record_native_surface_runtime_log(&format!(
        "native gtk render-api requested rect: x={}, y={}, width={}, height={}, add={add}",
        rect.x, rect.y, width, height
    ));
    *layer_allocation.borrow_mut() = gtk::Rectangle::new(rect.x, rect.y, width, height);
    gl_area.set_size_request(width, height);
    gl_area.size_allocate(&layer_allocation.borrow());
    if add {
        overlay.add_overlay(gl_area);
        overlay.set_overlay_pass_through(gl_area, true);
    }
    overlay.queue_resize();
    gl_area.queue_resize();
}

fn surface_length_to_i32(value: u32) -> i32 {
    i32::try_from(value).unwrap_or(i32::MAX)
}

struct GtkRenderState {
    id: u64,
    mpv_client: Option<Mpv>,
    renderer: Option<GtkMpvRenderer>,
    diagnostics: RenderApiDiagnostics,
    logged_first_frame: bool,
    logged_frame_size: Option<(i32, i32)>,
}

impl GtkRenderState {
    fn new(id: u64, diagnostics: RenderApiDiagnostics) -> Self {
        Self {
            id,
            mpv_client: None,
            renderer: None,
            diagnostics,
            logged_first_frame: false,
            logged_frame_size: None,
        }
    }

    fn attach_to_mpv(&mut self, mpv_client: Mpv) {
        self.mpv_client = Some(mpv_client);
    }

    fn realize(&mut self, gl_area: &gtk::GLArea) {
        gl_area.make_current();
        if let Some(err) = gl_area.error() {
            self.record_error(format!("native gtk GLArea could not become current: {err}"));
            return;
        }

        if self.renderer.is_none() {
            let Some(mpv_client) = self.mpv_client.as_ref() else {
                return;
            };
            match GtkMpvRenderer::new(self.id, mpv_client) {
                Ok(renderer) => {
                    self.renderer = Some(renderer);
                    self.diagnostics.set_alive(true);
                }
                Err(err) => {
                    self.record_error(err);
                    return;
                }
            }
        }

        queue_gl_area_render(gl_area);
    }

    fn render(&mut self, gl_area: &gtk::GLArea) {
        gl_area.make_current();
        if let Some(err) = gl_area.error() {
            self.record_error(format!("native gtk GLArea render context failed: {err}"));
            return;
        }

        let width = gl_area.allocated_width().max(1);
        let height = gl_area.allocated_height().max(1);
        let Some(renderer) = self.renderer.as_mut() else {
            self.realize(gl_area);
            return;
        };
        if self.logged_frame_size != Some((width, height)) {
            record_native_surface_runtime_log(&format!(
                "native gtk render-api frame allocation: {width}x{height}"
            ));
            self.logged_frame_size = Some((width, height));
        }

        match renderer.render(width, height) {
            Ok(update_flags) => {
                self.diagnostics.set_alive(true);
                self.diagnostics
                    .record_frame(width as u32, height as u32, update_flags);
                if !self.logged_first_frame {
                    record_native_surface_runtime_log(&format!(
                        "native gtk render-api submitted first frame: {width}x{height}, update_flags={update_flags}"
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

struct GtkMpvRenderer {
    context: *mut mpv_sys::mpv_render_context,
    callback_ctx: *mut c_void,
}

impl GtkMpvRenderer {
    fn new(id: u64, mpv_client: &Mpv) -> Result<Self, String> {
        let mut init_params = mpv_sys::mpv_opengl_init_params {
            get_proc_address: Some(gtk_get_proc_address),
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
                "libmpv could not create gtk render context: {}",
                mpv_error_message(result)
            ));
        }

        let callback_ctx = Box::into_raw(Box::new(id)).cast::<c_void>();
        unsafe {
            mpv_sys::mpv_render_context_set_update_callback(
                context,
                Some(gtk_mpv_update_callback),
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
                "libmpv gtk render failed: {}",
                mpv_error_message(result)
            ));
        }
        unsafe {
            mpv_sys::mpv_render_context_report_swap(self.context);
        }
        Ok(update_flags)
    }
}

impl Drop for GtkMpvRenderer {
    fn drop(&mut self) {
        unsafe {
            mpv_sys::mpv_render_context_set_update_callback(self.context, None, ptr::null_mut());
            mpv_sys::mpv_render_context_free(self.context);
            drop(Box::from_raw(self.callback_ctx.cast::<u64>()));
        }
    }
}

unsafe extern "C" fn gtk_mpv_update_callback(ctx: *mut c_void) {
    if ctx.is_null() {
        return;
    }
    let id = unsafe { *ctx.cast::<u64>() };
    gtk::glib::idle_add_once(move || {
        GTK_SURFACES.with(|surfaces| {
            if let Some(surface) = surfaces.borrow().get(&id) {
                surface.schedule_render();
            }
        });
    });
}

fn queue_gl_area_render(gl_area: &gtk::GLArea) {
    gl_area.queue_render();
    gl_area.queue_draw();
}

unsafe extern "C" fn gtk_get_proc_address(_ctx: *mut c_void, name: *const c_char) -> *mut c_void {
    if name.is_null() {
        return ptr::null_mut();
    }

    find_gl_symbol(name).unwrap_or(ptr::null_mut())
}

fn find_gl_symbol(name: *const c_char) -> Option<*mut c_void> {
    type GlGetProcAddress = unsafe extern "C" fn(*const c_char) -> *const c_void;

    for (library, symbol) in [
        ("libGL.so.1", b"glXGetProcAddress\0".as_slice()),
        ("libEGL.so.1", b"eglGetProcAddress\0".as_slice()),
    ] {
        if let Ok(library) = unsafe { libloading::Library::new(library) } {
            if let Ok(get_proc_address) = unsafe { library.get::<GlGetProcAddress>(symbol) } {
                let ptr = unsafe { get_proc_address(name) };
                if !ptr.is_null() {
                    return Some(ptr.cast_mut());
                }
            }
        }
    }

    None
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
