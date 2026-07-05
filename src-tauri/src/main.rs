#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

fn main() {
    #[cfg(target_os = "linux")]
    {
        // This is set before Tauri starts app threads, so no other thread can read the env concurrently.
        unsafe { std::env::set_var("GDK_BACKEND", "x11") };
    }
    melearner_lib::run();
}
