mod scanner;

use tauri_plugin_sql::{Builder as SqlBuilder, Migration, MigrationKind};

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
        Migration {
            version: 11,
            description: "create_sections_subtitles_settings",
            sql: "CREATE TABLE IF NOT EXISTS sections (
                id TEXT PRIMARY KEY,
                course_id TEXT NOT NULL,
                name TEXT NOT NULL,
                order_index INTEGER DEFAULT 0,
                created_at TEXT DEFAULT CURRENT_TIMESTAMP,
                updated_at TEXT DEFAULT CURRENT_TIMESTAMP,
                metadata TEXT,
                UNIQUE(course_id, name),
                FOREIGN KEY (course_id) REFERENCES courses(id) ON DELETE CASCADE
            );
            CREATE TABLE IF NOT EXISTS lesson_subtitles (
                id TEXT PRIMARY KEY,
                lesson_id TEXT NOT NULL,
                path TEXT NOT NULL,
                language TEXT,
                label TEXT,
                order_index INTEGER DEFAULT 0,
                created_at TEXT DEFAULT CURRENT_TIMESTAMP,
                UNIQUE(lesson_id, path),
                FOREIGN KEY (lesson_id) REFERENCES lessons(id) ON DELETE CASCADE
            );
            CREATE TABLE IF NOT EXISTS app_settings (
                key TEXT PRIMARY KEY,
                value TEXT,
                updated_at TEXT DEFAULT CURRENT_TIMESTAMP
            );
            CREATE INDEX IF NOT EXISTS idx_sections_course ON sections(course_id);
            CREATE INDEX IF NOT EXISTS idx_lesson_subtitles_lesson ON lesson_subtitles(lesson_id);",
            kind: MigrationKind::Up,
        },
        Migration {
            version: 12,
            description: "add_structured_lesson_metadata",
            sql: "ALTER TABLE lessons ADD COLUMN section_id TEXT;
                  ALTER TABLE lessons ADD COLUMN file_size INTEGER DEFAULT 0;
                  ALTER TABLE lessons ADD COLUMN updated_at TEXT;
                  ALTER TABLE lessons ADD COLUMN metadata TEXT;
                  ALTER TABLE courses ADD COLUMN thumbnail_source_path TEXT;
                  ALTER TABLE courses ADD COLUMN last_scanned_at TEXT;",
            kind: MigrationKind::Up,
        },
        Migration {
            version: 13,
            description: "create_structured_metadata_indexes",
            sql: "CREATE INDEX IF NOT EXISTS idx_lessons_section ON lessons(section_id);
                  CREATE INDEX IF NOT EXISTS idx_courses_path ON courses(path);
                  CREATE INDEX IF NOT EXISTS idx_lessons_path ON lessons(path);",
            kind: MigrationKind::Up,
        },
        Migration {
            version: 14,
            description: "backfill_sections_from_existing_lessons",
            sql: "INSERT OR IGNORE INTO sections (id, course_id, name, order_index, updated_at)
                  SELECT course_id || ':section:' || lower(hex(COALESCE(section_name, 'Course'))),
                         course_id,
                         COALESCE(section_name, 'Course'),
                         MIN(COALESCE(order_index, 0)),
                         CURRENT_TIMESTAMP
                  FROM lessons
                  GROUP BY course_id, COALESCE(section_name, 'Course');
                  UPDATE lessons
                  SET section_id = (
                    SELECT sections.id
                    FROM sections
                    WHERE sections.course_id = lessons.course_id
                      AND sections.name = COALESCE(lessons.section_name, 'Course')
                    LIMIT 1
                  )
                  WHERE section_id IS NULL;",
            kind: MigrationKind::Up,
        },
    ]
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    let _ = write_startup_log("start");

    #[cfg(target_os = "linux")]
    {
        // This runs before Tauri starts app threads, so no other thread can read the env concurrently.
        unsafe { std::env::set_var("GST_PLUGIN_FEATURE_RANK", "avdec_h264:MAX") };
    }

fn get_db_path() -> std::path::PathBuf {
    std::env::var("HOME")
        .map(|h| std::path::PathBuf::from(h).join(".local").join("share").join("melearner").join("melearner.db"))
        .unwrap_or_else(|_| std::path::PathBuf::from("melearner.db"))
}

fn get_db_url() -> String {
    format!("sqlite:{}", get_db_path().display())
}

#[tauri::command]
fn get_database_path() -> String {
    get_db_url()
}
    let _ = write_startup_log("paths.ready");

    let db_path = get_db_path();
    if let Some(parent) = db_path.parent() {
        let _ = std::fs::create_dir_all(parent);
    }

    tauri::Builder::default()
        .plugin(tauri_plugin_fs::init())
        .plugin(tauri_plugin_dialog::init())
        .plugin(
            SqlBuilder::default()
                .add_migrations(&get_db_url(), get_migrations())
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

            let _ = write_startup_log("builder.setup.exit");
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            scanner::scan_folder,
            scanner::get_file_info,
            log_frontend,
            open_native,
            generate_video_thumbnail,
            get_build_info,
            get_database_path,
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}

fn write_startup_log(event: &str) -> std::io::Result<()> {
    use std::fs::OpenOptions;
    use std::io::Write;

    let log_path = std::env::var("HOME")
        .map(|h| std::path::PathBuf::from(h).join(".melearner").join("startup.log"))
        .unwrap_or_else(|_| std::path::PathBuf::from("/tmp/melearner-startup.log"));

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
        .map(|h| std::path::PathBuf::from(h).join(".melearner").join("frontend.log"))
        .unwrap_or_else(|_| std::path::PathBuf::from("/tmp/melearner-frontend.log"));

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

#[tauri::command]
fn generate_video_thumbnail(path: String, seed: f64) -> Result<Vec<u8>, String> {
    use std::process::{Command, Stdio};

    let path_buf = std::path::PathBuf::from(&path);
    if !path_buf.exists() {
        return Err(format!("file not found: {path}"));
    }

    let duration = Command::new("ffprobe")
        .args([
            "-v",
            "error",
            "-show_entries",
            "format=duration",
            "-of",
            "default=noprint_wrappers=1:nokey=1",
        ])
        .arg(&path_buf)
        .stdin(Stdio::null())
        .stderr(Stdio::null())
        .output()
        .ok()
        .and_then(|output| String::from_utf8(output.stdout).ok())
        .and_then(|raw| raw.trim().parse::<f64>().ok())
        .filter(|value| value.is_finite() && *value > 0.0)
        .unwrap_or(10.0);

    let seed = seed.clamp(0.0, 1.0);
    let timestamp = if duration > 12.0 {
        (duration * (0.08 + seed * 0.45)).clamp(1.0, duration - 1.0)
    } else {
        (duration * 0.5).max(0.0)
    };

    let name = format!(
        "melearner-thumb-{}-{}.jpg",
        std::process::id(),
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|duration| duration.as_nanos())
            .unwrap_or(0)
    );
    let output_path = std::env::temp_dir().join(name);
    let timestamp_arg = format!("{timestamp:.3}");

    let status = Command::new("ffmpeg")
        .args([
            "-hide_banner",
            "-loglevel",
            "error",
            "-ss",
            &timestamp_arg,
            "-i",
        ])
        .arg(&path_buf)
        .args(["-frames:v", "1", "-vf", "scale=640:-1", "-q:v", "5", "-y"])
        .arg(&output_path)
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status()
        .map_err(|e| format!("failed to run ffmpeg: {e}"))?;

    if !status.success() {
        let _ = std::fs::remove_file(&output_path);
        return Err("failed to generate thumbnail".to_string());
    }

    let bytes = std::fs::read(&output_path).map_err(|e| format!("failed to read thumbnail: {e}"))?;
    let _ = std::fs::remove_file(&output_path);
    Ok(bytes)
}

#[derive(serde::Serialize)]
struct BuildInfo {
    version: &'static str,
    git_sha: &'static str,
    git_sha_long: &'static str,
    build_timestamp: &'static str,
    rust_version: &'static str,
}

#[tauri::command]
fn get_build_info() -> BuildInfo {
    BuildInfo {
        version: env!("CARGO_PKG_VERSION"),
        git_sha: env!("MELEARNER_GIT_SHA"),
        git_sha_long: env!("MELEARNER_GIT_SHA_LONG"),
        build_timestamp: env!("MELEARNER_BUILD_TIMESTAMP"),
        rust_version: env!("CARGO_PKG_RUST_VERSION"),
    }
}
