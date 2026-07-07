#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

fn main() {
    configure_libmpv_numeric_locale();
    #[cfg(target_os = "linux")]
    configure_linux_gtk_backend();
    melearner_lib::run();
}

fn configure_libmpv_numeric_locale() {
    #[cfg(any(target_os = "linux", target_os = "macos"))]
    unsafe {
        let locale = std::ffi::CString::new("C").expect("static locale should not contain nul");
        libc::setlocale(libc::LC_NUMERIC, locale.as_ptr());
    }
}

#[cfg(target_os = "linux")]
fn configure_linux_gtk_backend() {
    if std::env::var_os("GDK_BACKEND").is_some() {
        return;
    }

    let force_x11 = std::env::var("MELEARNER_FORCE_GDK_X11").ok().as_deref() == Some("1")
        || std::env::var("MELEARNER_SURFACE_BACKEND").ok().as_deref() == Some("window-handle");

    if force_x11 {
        // This is set before Tauri starts app threads, so no other thread can read the env concurrently.
        unsafe { std::env::set_var("GDK_BACKEND", "x11") };
    }
}
