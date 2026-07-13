#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct MigrationDefinition {
    pub version: i64,
    pub description: &'static str,
    pub sql: &'static str,
}

pub const MIGRATIONS: &[MigrationDefinition] = &[
    MigrationDefinition {
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
    },
    MigrationDefinition {
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
    },
    MigrationDefinition {
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
    },
    MigrationDefinition {
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
    },
    MigrationDefinition {
        version: 5,
        description: "create_settings_table",
        sql: "CREATE TABLE IF NOT EXISTS settings (
                key TEXT PRIMARY KEY,
                value TEXT
            );",
    },
    MigrationDefinition {
        version: 6,
        description: "create_indexes",
        sql: "CREATE INDEX IF NOT EXISTS idx_lessons_course ON lessons(course_id);
                  CREATE INDEX IF NOT EXISTS idx_notes_lesson ON notes(lesson_id);
                  CREATE INDEX IF NOT EXISTS idx_bookmarks_lesson ON bookmarks(lesson_id);",
    },
    MigrationDefinition {
        version: 7,
        description: "drop_orphan_tables",
        sql: "DROP TABLE IF EXISTS bookmarks;
                  DROP TABLE IF EXISTS settings;",
    },
    MigrationDefinition {
        version: 8,
        description: "create_notes_lessons_indexes",
        sql: "CREATE INDEX IF NOT EXISTS idx_lessons_course ON lessons(course_id);
                  CREATE INDEX IF NOT EXISTS idx_notes_lesson ON notes(lesson_id);",
    },
    MigrationDefinition {
        version: 9,
        description: "noop_migration_9",
        sql: "SELECT 1;",
    },
    MigrationDefinition {
        version: 10,
        description: "noop_migration_10",
        sql: "SELECT 1;",
    },
    MigrationDefinition {
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
    },
    MigrationDefinition {
        version: 12,
        description: "add_structured_lesson_metadata",
        sql: "ALTER TABLE lessons ADD COLUMN section_id TEXT;
                  ALTER TABLE lessons ADD COLUMN file_size INTEGER DEFAULT 0;
                  ALTER TABLE lessons ADD COLUMN updated_at TEXT;
                  ALTER TABLE lessons ADD COLUMN metadata TEXT;
                  ALTER TABLE courses ADD COLUMN thumbnail_source_path TEXT;
                  ALTER TABLE courses ADD COLUMN last_scanned_at TEXT;",
    },
    MigrationDefinition {
        version: 13,
        description: "create_structured_metadata_indexes",
        sql: "CREATE INDEX IF NOT EXISTS idx_lessons_section ON lessons(section_id);
                  CREATE INDEX IF NOT EXISTS idx_courses_path ON courses(path);
                  CREATE INDEX IF NOT EXISTS idx_lessons_path ON lessons(path);",
    },
    MigrationDefinition {
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
    },
    MigrationDefinition {
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
    },
    MigrationDefinition {
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
    },
];
