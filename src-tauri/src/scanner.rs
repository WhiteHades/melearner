use melearner_core::scanner::{ScanResult, scan_library};

#[tauri::command]
pub async fn scan_folder(path: String) -> Result<ScanResult, String> {
    tokio::task::spawn_blocking(move || scan_library(&path))
        .await
        .map_err(|error| format!("scan task panicked: {error}"))
}
