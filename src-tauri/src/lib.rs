mod library_search;
mod media;
mod native_player;
mod scanner;

const DATABASE_PATH_ENV: &str = "MELEARNER_DB_PATH";
const FRONTEND_LOG_ENV: &str = "MELEARNER_FRONTEND_LOG";

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
        Migration {
            version: 15,
            description: "add_durable_course_identity_fields",
            sql: "ALTER TABLE courses ADD COLUMN identity_id TEXT;
                  ALTER TABLE courses ADD COLUMN fingerprint TEXT;
                  ALTER TABLE courses ADD COLUMN missing_since TEXT;
                  ALTER TABLE lessons ADD COLUMN relative_path TEXT;
                  UPDATE courses SET identity_id = id WHERE identity_id IS NULL;
                  CREATE UNIQUE INDEX IF NOT EXISTS idx_courses_identity_id ON courses(identity_id) WHERE identity_id IS NOT NULL;
                  CREATE INDEX IF NOT EXISTS idx_courses_fingerprint ON courses(fingerprint);
                  CREATE INDEX IF NOT EXISTS idx_lessons_course_relative_path ON lessons(course_id, relative_path);",
            kind: MigrationKind::Up,
        },
        Migration {
            version: 16,
            description: "create_lesson_activity",
            sql: "CREATE TABLE IF NOT EXISTS lesson_activity (
                id TEXT PRIMARY KEY,
                course_id TEXT NOT NULL,
                lesson_id TEXT NOT NULL,
                activity_date TEXT NOT NULL,
                watched_seconds INTEGER DEFAULT 0,
                completed INTEGER DEFAULT 0,
                created_at TEXT DEFAULT CURRENT_TIMESTAMP,
                FOREIGN KEY (course_id) REFERENCES courses(id) ON DELETE CASCADE,
                FOREIGN KEY (lesson_id) REFERENCES lessons(id) ON DELETE CASCADE
            );
            CREATE INDEX IF NOT EXISTS idx_lesson_activity_date ON lesson_activity(activity_date);
            CREATE INDEX IF NOT EXISTS idx_lesson_activity_lesson ON lesson_activity(lesson_id);",
            kind: MigrationKind::Up,
        },
    ]
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    let _ = write_startup_log("start");
    let startup_route = startup_route_from_runtime();
    if startup_route.is_some() {
        let _ = write_startup_log("startup.route.ready");
    }
    let startup_auto_scan_path = startup_auto_scan_path_from_runtime();
    if startup_auto_scan_path.is_some() {
        let _ = write_startup_log("startup.auto_scan.ready");
    }

    fn get_db_url() -> String {
        format!("sqlite:{}", get_db_path().display())
    }

    #[tauri::command]
    fn get_database_path() -> String {
        get_db_url()
    }

    #[tauri::command]
    fn write_course_marker(path: String, identity_id: String) -> Result<(), String> {
        let identity_id = identity_id.trim();
        if identity_id.is_empty() {
            return Err("course marker identity is empty".to_string());
        }

        let course_path = std::path::PathBuf::from(path);
        if !course_path.is_dir() {
            return Err(format!(
                "course folder is not available: {}",
                course_path.display()
            ));
        }

        let marker_path = course_path.join(".melearner-course.json");
        if marker_path.exists() {
            let raw = std::fs::read_to_string(&marker_path)
                .map_err(|e| format!("cannot read course marker {}: {e}", marker_path.display()))?;
            let value: serde_json::Value = serde_json::from_str(&raw)
                .map_err(|e| format!("invalid course marker {}: {e}", marker_path.display()))?;
            let existing = value
                .get("identityId")
                .and_then(|value| value.as_str())
                .unwrap_or_default()
                .trim();
            if !existing.is_empty() && existing != identity_id {
                return Err(format!(
                    "course marker already has a different identity: {}",
                    marker_path.display()
                ));
            }
        }

        let marker = serde_json::json!({
            "version": 1,
            "identityId": identity_id,
        });
        let json = serde_json::to_string_pretty(&marker)
            .map_err(|e| format!("cannot serialize course marker: {e}"))?;
        std::fs::write(&marker_path, format!("{json}\n"))
            .map_err(|e| format!("cannot write course marker {}: {e}", marker_path.display()))
    }
    let _ = write_startup_log("paths.ready");

    let db_path = get_db_path();
    if let Some(parent) = db_path.parent() {
        let _ = std::fs::create_dir_all(parent);
    }

    let startup_route_script =
        startup_initialization_script(startup_route.as_ref(), startup_auto_scan_path.as_deref());

    tauri::Builder::default()
        .manage(StartupRouteState(startup_route))
        .plugin(startup_route_plugin(startup_route_script))
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
            media::generate_video_thumbnail,
            library_search::index_library_search,
            library_search::search_library,
            library_search::clear_library_search,
            native_player::native_player_load,
            native_player::native_player_state,
            native_player::native_player_play,
            native_player::native_player_pause,
            native_player::native_player_seek,
            native_player::native_player_set_volume,
            native_player::native_player_set_muted,
            native_player::native_player_set_rate,
            native_player::native_player_select_audio_track,
            native_player::native_player_select_subtitle_track,
            native_player::native_player_select_chapter,
            native_player::native_player_set_bounds,
            native_player::native_player_set_surface_visible,
            native_player::native_player_step_frame,
            native_player::native_player_screenshot,
            native_player::native_player_destroy,
            get_build_info,
            get_database_path,
            get_startup_route,
            write_course_marker,
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}

fn write_startup_log(event: &str) -> std::io::Result<()> {
    use std::fs::OpenOptions;
    use std::io::Write;

    let log_path = std::env::var("HOME")
        .map(|h| {
            std::path::PathBuf::from(h)
                .join(".melearner")
                .join("startup.log")
        })
        .unwrap_or_else(|_| std::path::PathBuf::from("/tmp/melearner-startup.log"));

    if let Some(parent) = log_path.parent() {
        let _ = std::fs::create_dir_all(parent);
    }

    let ts = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs_f64())
        .unwrap_or(0.0);

    let mut f = OpenOptions::new()
        .create(true)
        .append(true)
        .open(&log_path)?;
    writeln!(f, "[{ts}] {event}")
}

#[tauri::command]
fn log_frontend(message: String) {
    use std::fs;
    use std::io::Write;

    let log_path = frontend_log_path();

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

fn frontend_log_path() -> std::path::PathBuf {
    let configured = std::env::var(FRONTEND_LOG_ENV).ok();
    let home = std::env::var("HOME").ok();
    frontend_log_path_from_values(configured.as_deref(), home.as_deref())
}

fn frontend_log_path_from_values(
    configured: Option<&str>,
    home: Option<&str>,
) -> std::path::PathBuf {
    if let Some(configured) = configured.map(str::trim).filter(|value| !value.is_empty()) {
        return std::path::PathBuf::from(configured);
    }

    home.map(|home| {
        std::path::PathBuf::from(home)
            .join(".melearner")
            .join("frontend.log")
    })
    .unwrap_or_else(|| std::path::PathBuf::from("/tmp/melearner-frontend.log"))
}

fn get_db_path() -> std::path::PathBuf {
    let configured = std::env::var(DATABASE_PATH_ENV).ok();
    let home = std::env::var("HOME").ok();
    let local_app_data = std::env::var("LOCALAPPDATA").ok();
    let path = database_path_from_values(
        configured.as_deref(),
        home.as_deref(),
        local_app_data.as_deref(),
        std::env::consts::OS,
    );

    if let Some(parent) = path.parent() {
        let _ = std::fs::create_dir_all(parent);
    }

    path
}

fn database_path_from_values(
    configured: Option<&str>,
    home: Option<&str>,
    local_app_data: Option<&str>,
    target_os: &str,
) -> std::path::PathBuf {
    if let Some(configured) = configured.map(str::trim).filter(|value| !value.is_empty()) {
        return std::path::PathBuf::from(configured);
    }

    match target_os {
        "windows" => local_app_data
            .map(|path| {
                std::path::PathBuf::from(path)
                    .join("melearner")
                    .join("melearner.db")
            })
            .or_else(|| {
                home.map(|home| {
                    std::path::PathBuf::from(home)
                        .join("AppData")
                        .join("Local")
                        .join("melearner")
                        .join("melearner.db")
                })
            })
            .unwrap_or_else(|| std::env::temp_dir().join("melearner").join("melearner.db")),
        "macos" => home
            .map(|home| {
                std::path::PathBuf::from(home)
                    .join("Library")
                    .join("Application Support")
                    .join("melearner")
                    .join("melearner.db")
            })
            .unwrap_or_else(|| std::env::temp_dir().join("melearner").join("melearner.db")),
        _ => home
            .map(|home| {
                std::path::PathBuf::from(home)
                    .join(".local")
                    .join("share")
                    .join("melearner")
                    .join("melearner.db")
            })
            .unwrap_or_else(|| std::env::temp_dir().join("melearner").join("melearner.db")),
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

#[derive(serde::Serialize)]
struct BuildInfo {
    version: &'static str,
    git_sha: &'static str,
    git_sha_long: &'static str,
    build_timestamp: &'static str,
    rust_version: &'static str,
}

#[derive(Debug, Clone, PartialEq, Eq, serde::Serialize)]
#[serde(rename_all = "camelCase")]
struct StartupRoute {
    course_id: String,
    lesson_id: Option<String>,
}

struct StartupRouteState(Option<StartupRoute>);

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

fn startup_route_from_runtime() -> Option<StartupRoute> {
    startup_route_from_sources(
        std::env::args().skip(1),
        std::env::var("MELEARNER_OPEN_COURSE_ID").ok().as_deref(),
        std::env::var("MELEARNER_OPEN_LESSON_ID").ok().as_deref(),
    )
}

fn startup_auto_scan_path_from_runtime() -> Option<String> {
    startup_auto_scan_path_from_sources(
        std::env::args().skip(1),
        std::env::var("MELEARNER_AUTO_SCAN_PATH").ok().as_deref(),
    )
}

fn startup_initialization_script(
    route: Option<&StartupRoute>,
    auto_scan_path: Option<&str>,
) -> String {
    let route_value = serde_json::to_string(&route).unwrap_or_else(|_| "null".to_string());
    let auto_scan_value = serde_json::to_string(&clean_startup_route_value(auto_scan_path))
        .unwrap_or_else(|_| "null".to_string());
    format!(
        "window.__MELEARNER_STARTUP_ROUTE__ = {route_value};\nwindow.__MELEARNER_AUTO_SCAN_PATH__ = {auto_scan_value};"
    )
}

fn startup_route_plugin<R: tauri::Runtime>(script: String) -> tauri::plugin::TauriPlugin<R> {
    tauri::plugin::Builder::<R, ()>::new("startup-route")
        .js_init_script(script)
        .build()
}

#[tauri::command]
fn get_startup_route(state: tauri::State<'_, StartupRouteState>) -> Option<StartupRoute> {
    state.0.clone()
}

fn startup_route_from_sources(
    args: impl IntoIterator<Item = impl AsRef<str>>,
    env_course_id: Option<&str>,
    env_lesson_id: Option<&str>,
) -> Option<StartupRoute> {
    let mut course_id = clean_startup_route_value(env_course_id);
    let mut lesson_id = clean_startup_route_value(env_lesson_id);
    let mut args = args.into_iter();

    while let Some(arg) = args.next() {
        let arg = arg.as_ref();
        if let Some(value) = arg.strip_prefix("--open-course=") {
            course_id = clean_startup_route_value(Some(value));
        } else if arg == "--open-course" {
            course_id = args
                .next()
                .and_then(|value| clean_startup_route_value(Some(value.as_ref())));
        } else if let Some(value) = arg.strip_prefix("--open-lesson=") {
            lesson_id = clean_startup_route_value(Some(value));
        } else if arg == "--open-lesson" {
            lesson_id = args
                .next()
                .and_then(|value| clean_startup_route_value(Some(value.as_ref())));
        }
    }

    course_id.map(|course_id| StartupRoute {
        course_id,
        lesson_id,
    })
}

fn startup_auto_scan_path_from_sources(
    args: impl IntoIterator<Item = impl AsRef<str>>,
    env_path: Option<&str>,
) -> Option<String> {
    let mut path = clean_startup_route_value(env_path);
    let mut args = args.into_iter();

    while let Some(arg) = args.next() {
        let arg = arg.as_ref();
        if let Some(value) = arg.strip_prefix("--auto-scan=") {
            path = clean_startup_route_value(Some(value));
        } else if arg == "--auto-scan" {
            path = args
                .next()
                .and_then(|value| clean_startup_route_value(Some(value.as_ref())));
        }
    }

    path
}

fn clean_startup_route_value(value: Option<&str>) -> Option<String> {
    value.and_then(|value| {
        let trimmed = value.trim();
        if trimmed.is_empty() {
            None
        } else {
            Some(trimmed.to_string())
        }
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn startup_route_uses_cli_args() {
        assert_eq!(
            startup_route_from_sources(
                ["--open-course", "course-1", "--open-lesson", "lesson-1"],
                None,
                None,
            ),
            Some(StartupRoute {
                course_id: "course-1".to_string(),
                lesson_id: Some("lesson-1".to_string()),
            })
        );
    }

    #[test]
    fn startup_route_uses_env_when_cli_is_absent() {
        assert_eq!(
            startup_route_from_sources(
                std::iter::empty::<&str>(),
                Some(" course-2 "),
                Some(" lesson-2 "),
            ),
            Some(StartupRoute {
                course_id: "course-2".to_string(),
                lesson_id: Some("lesson-2".to_string()),
            })
        );
    }

    #[test]
    fn startup_route_requires_course_id() {
        assert_eq!(
            startup_route_from_sources(["--open-lesson", "lesson-1"], None, None),
            None
        );
    }

    #[test]
    fn startup_auto_scan_path_uses_cli_or_env() {
        assert_eq!(
            startup_auto_scan_path_from_sources(["--auto-scan", "/courses"], None),
            Some("/courses".to_string())
        );
        assert_eq!(
            startup_auto_scan_path_from_sources(std::iter::empty::<&str>(), Some(" /library ")),
            Some("/library".to_string())
        );
        assert_eq!(
            startup_auto_scan_path_from_sources(["--auto-scan="], Some("")),
            None
        );
    }

    #[test]
    fn startup_initialization_script_sets_window_values() {
        assert_eq!(
            startup_initialization_script(
                Some(&StartupRoute {
                    course_id: "course-1".to_string(),
                    lesson_id: Some("lesson-1".to_string()),
                }),
                Some(" /courses ")
            ),
            "window.__MELEARNER_STARTUP_ROUTE__ = {\"courseId\":\"course-1\",\"lessonId\":\"lesson-1\"};\nwindow.__MELEARNER_AUTO_SCAN_PATH__ = \"/courses\";"
        );
        assert_eq!(
            startup_initialization_script(None, None),
            "window.__MELEARNER_STARTUP_ROUTE__ = null;\nwindow.__MELEARNER_AUTO_SCAN_PATH__ = null;"
        );
    }

    #[test]
    fn frontend_log_path_uses_explicit_env_path() {
        assert_eq!(
            frontend_log_path_from_values(
                Some(" /tmp/melearner/frontend.log "),
                Some("/home/user")
            ),
            std::path::PathBuf::from("/tmp/melearner/frontend.log")
        );
    }

    #[test]
    fn frontend_log_path_defaults_to_home() {
        assert_eq!(
            frontend_log_path_from_values(None, Some("/home/user")),
            std::path::PathBuf::from("/home/user/.melearner/frontend.log")
        );
    }

    #[test]
    fn database_path_uses_explicit_env_path() {
        assert_eq!(
            database_path_from_values(
                Some(" /tmp/melearner/test.db "),
                Some("/home/user"),
                None,
                "linux"
            ),
            std::path::PathBuf::from("/tmp/melearner/test.db")
        );
    }

    #[test]
    fn database_path_preserves_linux_location() {
        assert_eq!(
            database_path_from_values(None, Some("/home/user"), None, "linux"),
            std::path::PathBuf::from("/home/user/.local/share/melearner/melearner.db")
        );
    }

    #[test]
    fn database_path_uses_windows_local_app_data() {
        assert_eq!(
            database_path_from_values(
                None,
                Some("C:/Users/Ada"),
                Some("C:/Users/Ada/AppData/Local"),
                "windows"
            ),
            std::path::PathBuf::from("C:/Users/Ada/AppData/Local/melearner/melearner.db")
        );
    }

    #[test]
    fn database_path_uses_macos_application_support() {
        assert_eq!(
            database_path_from_values(None, Some("/Users/ada"), None, "macos"),
            std::path::PathBuf::from(
                "/Users/ada/Library/Application Support/melearner/melearner.db"
            )
        );
    }
}
