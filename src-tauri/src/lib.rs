mod scanner;
mod video_server;

use tauri_plugin_sql::{Builder as SqlBuilder, Migration, MigrationKind};
use video_server::VideoServer;

fn get_migrations() -> Vec<Migration> {
    vec![
        Migration {
            version: 1,
            description: "create_courses_table",
            sql: "CREATE TABLE IF NOT EXISTS courses (
                id TEXT PRIMARY KEY,
                name TEXT NOT NULL,
                path TEXT UNIQUE NOT NULL,
                total_duration INTEGER DEFAULT 0,
                watched_duration INTEGER DEFAULT 0,
                last_accessed TEXT,
                created_at TEXT DEFAULT CURRENT_TIMESTAMP,
                metadata TEXT
            );",
            kind: MigrationKind::Up,
        },
        Migration {
            version: 2,
            description: "create_lessons_table",
            sql: "CREATE TABLE IF NOT EXISTS lessons (
                id TEXT PRIMARY KEY,
                course_id TEXT NOT NULL,
                section_name TEXT,
                name TEXT NOT NULL,
                path TEXT UNIQUE NOT NULL,
                type TEXT CHECK(type IN ('video', 'audio', 'document', 'quiz')),
                duration INTEGER DEFAULT 0,
                watched_time INTEGER DEFAULT 0,
                completed INTEGER DEFAULT 0,
                order_index INTEGER,
                last_position REAL DEFAULT 0,
                FOREIGN KEY (course_id) REFERENCES courses(id) ON DELETE CASCADE
            );",
            kind: MigrationKind::Up,
        },
        Migration {
            version: 3,
            description: "create_notes_table",
            sql: "CREATE TABLE IF NOT EXISTS notes (
                id TEXT PRIMARY KEY,
                lesson_id TEXT NOT NULL,
                timestamp REAL NOT NULL,
                text TEXT NOT NULL,
                created_at TEXT DEFAULT CURRENT_TIMESTAMP,
                FOREIGN KEY (lesson_id) REFERENCES lessons(id) ON DELETE CASCADE
            );",
            kind: MigrationKind::Up,
        },
        Migration {
            version: 4,
            description: "create_bookmarks_table",
            sql: "CREATE TABLE IF NOT EXISTS bookmarks (
                id TEXT PRIMARY KEY,
                lesson_id TEXT NOT NULL,
                timestamp REAL NOT NULL,
                label TEXT,
                created_at TEXT DEFAULT CURRENT_TIMESTAMP,
                FOREIGN KEY (lesson_id) REFERENCES lessons(id) ON DELETE CASCADE
            );",
            kind: MigrationKind::Up,
        },
        Migration {
            version: 5,
            description: "create_settings_table",
            sql: "CREATE TABLE IF NOT EXISTS settings (
                key TEXT PRIMARY KEY,
                value TEXT
            );",
            kind: MigrationKind::Up,
        },
        Migration {
            version: 6,
            description: "create_indexes",
            sql: "CREATE INDEX IF NOT EXISTS idx_lessons_course ON lessons(course_id);
                  CREATE INDEX IF NOT EXISTS idx_notes_lesson ON notes(lesson_id);
                  CREATE INDEX IF NOT EXISTS idx_bookmarks_lesson ON bookmarks(lesson_id);",
            kind: MigrationKind::Up,
        },
        Migration {
            version: 7,
            description: "drop_orphan_tables",
            sql: "DROP TABLE IF EXISTS bookmarks;
                  DROP TABLE IF EXISTS settings;",
            kind: MigrationKind::Up,
        },
        Migration {
            version: 8,
            description: "create_notes_lessons_indexes",
            sql: "CREATE INDEX IF NOT EXISTS idx_lessons_course ON lessons(course_id);
                  CREATE INDEX IF NOT EXISTS idx_notes_lesson ON notes(lesson_id);",
            kind: MigrationKind::Up,
        },
        Migration {
            version: 9,
            description: "noop_migration_9",
            sql: "SELECT 1;",
            kind: MigrationKind::Up,
        },
        Migration {
            version: 10,
            description: "noop_migration_10",
            sql: "SELECT 1;",
            kind: MigrationKind::Up,
        },
    ]
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    let _ = write_startup_log("start");

    #[cfg(target_os = "linux")]
    {
        std::env::set_var("GST_PLUGIN_FEATURE_RANK", "avdec_h264:MAX");
    }

    let db_path = std::env::var("HOME")
        .map(|h| std::path::PathBuf::from(h).join(".local").join("share").join("melearn").join("melearn.db"))
        .unwrap_or_else(|_| std::path::PathBuf::from("melearn.db"));

    if let Some(parent) = db_path.parent() {
        let _ = std::fs::create_dir_all(parent);
    }

    let db_url = format!("sqlite:{}", db_path.display());
    let _ = write_startup_log("paths.ready");

    tauri::Builder::default()
        .plugin(tauri_plugin_fs::init())
        .plugin(tauri_plugin_dialog::init())
        .plugin(
            SqlBuilder::default()
                .add_migrations(&db_url, get_migrations())
                .build(),
        )
        .setup(|app| {
            let _ = write_startup_log("builder.setup.entry");
            if cfg!(debug_assertions) {
                app.handle().plugin(
                    tauri_plugin_log::Builder::default()
                        .level(log::LevelFilter::Info)
                        .build(),
                )?;
            }

            tauri::async_runtime::spawn(async {
                let _ = write_startup_log("video.spawn");
                let port = VideoServer::get_or_start().await;
                let _ = write_startup_log(&format!("video.ready:{port}"));
            });

            let _ = write_startup_log("builder.setup.exit");
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            scanner::scan_folder,
            scanner::get_file_info,
            video_server::get_video_server_port,
            log_frontend,
            open_native,
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}

fn write_startup_log(event: &str) -> std::io::Result<()> {
    use std::fs::OpenOptions;
    use std::io::Write;

    let log_path = std::env::var("HOME")
        .map(|h| std::path::PathBuf::from(h).join(".melearn").join("startup.log"))
        .unwrap_or_else(|_| std::path::PathBuf::from("/tmp/melearn-startup.log"));

    if let Some(parent) = log_path.parent() {
        let _ = std::fs::create_dir_all(parent);
    }

    let ts = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs_f64())
        .unwrap_or(0.0);

    let mut f = OpenOptions::new().create(true).append(true).open(&log_path)?;
    writeln!(f, "[{ts}] {event}")
}

#[tauri::command]
fn log_frontend(message: String) {
    use std::fs;
    use std::io::Write;

    let log_path = std::env::var("HOME")
        .map(|h| std::path::PathBuf::from(h).join(".melearn").join("frontend.log"))
        .unwrap_or_else(|_| std::path::PathBuf::from("/tmp/melearn-frontend.log"));

    if let Some(parent) = log_path.parent() {
        let _ = fs::create_dir_all(parent);
    }

    let timestamp = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs_f64())
        .unwrap_or(0.0);

    if let Ok(mut f) = fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(&log_path)
    {
        let _ = writeln!(f, "[{timestamp}] {message}");
    }
}

#[tauri::command]
fn open_native(path: String) -> Result<(), String> {
    use std::process::Command;

    let path_buf = std::path::PathBuf::from(&path);
    if !path_buf.exists() {
        return Err(format!("file not found: {path}"));
    }

    #[cfg(target_os = "linux")]
    let mut cmd = {
        let mut c = Command::new("xdg-open");
        c.arg(&path_buf);
        c
    };

    #[cfg(target_os = "macos")]
    let mut cmd = {
        let mut c = Command::new("open");
        c.arg(&path_buf);
        c
    };

    #[cfg(target_os = "windows")]
    let mut cmd = {
        let mut c = Command::new("cmd");
        c.args(["/C", "start", "", &path]);
        c
    };

    cmd.stdin(std::process::Stdio::null())
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null())
        .spawn()
        .map(|_| ())
        .map_err(|e| format!("failed to open file: {e}"))
}
