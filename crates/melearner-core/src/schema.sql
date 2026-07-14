CREATE TABLE courses (
    id TEXT PRIMARY KEY,
    identity_id TEXT UNIQUE NOT NULL,
    name TEXT NOT NULL,
    path TEXT UNIQUE NOT NULL,
    fingerprint TEXT NOT NULL,
    last_accessed TEXT,
    created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    thumbnail_source_path TEXT,
    last_scanned_at TEXT NOT NULL,
    missing_since TEXT
);

CREATE TABLE sections (
    id TEXT PRIMARY KEY,
    course_id TEXT NOT NULL,
    name TEXT NOT NULL,
    order_index INTEGER NOT NULL DEFAULT 0,
    created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    UNIQUE(course_id, name),
    FOREIGN KEY (course_id) REFERENCES courses(id) ON DELETE CASCADE
);

CREATE TABLE lessons (
    id TEXT PRIMARY KEY,
    course_id TEXT NOT NULL,
    section_id TEXT NOT NULL,
    name TEXT NOT NULL,
    path TEXT UNIQUE NOT NULL,
    relative_path TEXT NOT NULL,
    type TEXT NOT NULL CHECK(type IN ('video', 'audio', 'document', 'quiz')),
    duration INTEGER NOT NULL DEFAULT 0 CHECK(duration >= 0),
    watched_time INTEGER NOT NULL DEFAULT 0 CHECK(watched_time >= 0),
    completed INTEGER NOT NULL DEFAULT 0 CHECK(completed IN (0, 1)),
    order_index INTEGER NOT NULL DEFAULT 0,
    last_position REAL NOT NULL DEFAULT 0 CHECK(last_position >= 0),
    file_size INTEGER NOT NULL DEFAULT 0 CHECK(file_size >= 0),
    updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (course_id) REFERENCES courses(id) ON DELETE CASCADE,
    FOREIGN KEY (section_id) REFERENCES sections(id) ON DELETE CASCADE
);

CREATE TABLE notes (
    id TEXT PRIMARY KEY,
    lesson_id TEXT NOT NULL,
    timestamp REAL NOT NULL CHECK(timestamp >= 0),
    text TEXT NOT NULL,
    created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (lesson_id) REFERENCES lessons(id) ON DELETE CASCADE
);

CREATE TABLE lesson_subtitles (
    id TEXT PRIMARY KEY,
    lesson_id TEXT NOT NULL,
    path TEXT NOT NULL,
    language TEXT NOT NULL DEFAULT '',
    label TEXT NOT NULL DEFAULT '',
    order_index INTEGER NOT NULL DEFAULT 0,
    created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    UNIQUE(lesson_id, path),
    FOREIGN KEY (lesson_id) REFERENCES lessons(id) ON DELETE CASCADE
);

CREATE TABLE app_settings (
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL,
    updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE lesson_activity (
    id TEXT PRIMARY KEY,
    course_id TEXT NOT NULL,
    lesson_id TEXT NOT NULL,
    activity_date TEXT NOT NULL,
    watched_seconds INTEGER NOT NULL DEFAULT 0 CHECK(watched_seconds >= 0),
    completed INTEGER NOT NULL DEFAULT 0 CHECK(completed IN (0, 1)),
    created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (course_id) REFERENCES courses(id) ON DELETE CASCADE,
    FOREIGN KEY (lesson_id) REFERENCES lessons(id) ON DELETE CASCADE
);

CREATE INDEX idx_lessons_course ON lessons(course_id);
CREATE INDEX idx_notes_lesson ON notes(lesson_id);
CREATE INDEX idx_sections_course ON sections(course_id);
CREATE INDEX idx_lesson_subtitles_lesson ON lesson_subtitles(lesson_id);
CREATE INDEX idx_lessons_section ON lessons(section_id);
CREATE INDEX idx_courses_path ON courses(path);
CREATE INDEX idx_lessons_path ON lessons(path);
CREATE INDEX idx_courses_fingerprint ON courses(fingerprint);
CREATE INDEX idx_lessons_course_relative_path ON lessons(course_id, relative_path);
CREATE INDEX idx_lesson_activity_date ON lesson_activity(activity_date);
CREATE INDEX idx_lesson_activity_lesson ON lesson_activity(lesson_id);
