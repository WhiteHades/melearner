PRAGMA user_version = 1;

CREATE TABLE schema_info (
    singleton INTEGER NOT NULL PRIMARY KEY CHECK (singleton = 1),
    schema_id TEXT NOT NULL UNIQUE,
    ddl_sha256 TEXT NOT NULL UNIQUE
);

CREATE TABLE library_root (
    singleton INTEGER NOT NULL PRIMARY KEY CHECK (singleton = 1),
    path TEXT NOT NULL UNIQUE,
    updated_at INTEGER NOT NULL
);

CREATE TABLE settings (
    singleton INTEGER NOT NULL PRIMARY KEY CHECK (singleton = 1),
    appearance TEXT NOT NULL CHECK (appearance IN ('light', 'dark', 'cozy')),
    library_presentation TEXT NOT NULL CHECK (library_presentation IN ('comfortable', 'compact')),
    revision INTEGER NOT NULL CHECK (revision >= 0),
    updated_at INTEGER NOT NULL
);

CREATE TABLE courses (
    id TEXT NOT NULL PRIMARY KEY,
    identity_id TEXT NOT NULL UNIQUE,
    name TEXT NOT NULL,
    path TEXT NOT NULL UNIQUE,
    fingerprint TEXT NOT NULL,
    thumbnail_source_path TEXT,
    last_accessed INTEGER,
    last_scanned_at INTEGER NOT NULL,
    missing_since INTEGER
);

CREATE TABLE sections (
    id TEXT NOT NULL PRIMARY KEY,
    course_id TEXT NOT NULL REFERENCES courses(id) ON DELETE CASCADE,
    name TEXT NOT NULL,
    order_index INTEGER NOT NULL CHECK (order_index >= 0),
    UNIQUE (course_id, id),
    UNIQUE (course_id, order_index)
);

CREATE TABLE lessons (
    id TEXT NOT NULL PRIMARY KEY,
    course_id TEXT NOT NULL,
    section_id TEXT NOT NULL,
    name TEXT NOT NULL,
    path TEXT NOT NULL UNIQUE,
    relative_path TEXT NOT NULL,
    type TEXT NOT NULL CHECK (type IN ('video', 'audio', 'document', 'quiz')),
    duration INTEGER NOT NULL DEFAULT 0 CHECK (duration >= 0),
    watched_time INTEGER NOT NULL DEFAULT 0 CHECK (watched_time >= 0),
    last_position REAL NOT NULL DEFAULT 0 CHECK (last_position >= 0),
    file_size INTEGER NOT NULL DEFAULT 0 CHECK (file_size >= 0),
    order_index INTEGER NOT NULL CHECK (order_index >= 0),
    completed INTEGER NOT NULL DEFAULT 0 CHECK (completed IN (0, 1)),
    modified_ns INTEGER NOT NULL DEFAULT 0,
    updated_at INTEGER NOT NULL,
    FOREIGN KEY (course_id, section_id) REFERENCES sections(course_id, id) ON DELETE CASCADE,
    UNIQUE (course_id, id),
    UNIQUE (course_id, relative_path)
);

CREATE TABLE notes (
    id TEXT NOT NULL PRIMARY KEY,
    lesson_id TEXT NOT NULL REFERENCES lessons(id) ON DELETE CASCADE,
    timestamp REAL NOT NULL CHECK (timestamp >= 0),
    text TEXT NOT NULL CHECK (length(text) > 0),
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL
);

CREATE TABLE lesson_subtitles (
    id TEXT NOT NULL PRIMARY KEY,
    lesson_id TEXT NOT NULL REFERENCES lessons(id) ON DELETE CASCADE,
    path TEXT NOT NULL UNIQUE,
    relative_path TEXT NOT NULL,
    language TEXT NOT NULL,
    label TEXT NOT NULL,
    order_index INTEGER NOT NULL CHECK (order_index >= 0),
    UNIQUE (lesson_id, path)
);

CREATE TABLE lesson_activity (
    id TEXT NOT NULL PRIMARY KEY,
    course_id TEXT NOT NULL,
    lesson_id TEXT NOT NULL,
    activity_date TEXT NOT NULL,
    watched_seconds INTEGER NOT NULL CHECK (watched_seconds >= 0),
    completed INTEGER NOT NULL CHECK (completed IN (0, 1)),
    created_at INTEGER NOT NULL,
    FOREIGN KEY (course_id, lesson_id) REFERENCES lessons(course_id, id) ON DELETE CASCADE,
    UNIQUE (course_id, lesson_id, activity_date)
);

CREATE VIRTUAL TABLE library_search USING fts5(
    name,
    kind UNINDEXED,
    object_id UNINDEXED,
    course_id UNINDEXED,
    section_id UNINDEXED,
    tokenize = 'unicode61 remove_diacritics 2',
    prefix = '2 3 4',
    content = ''
);

CREATE INDEX courses_identity_idx ON courses(identity_id);
CREATE INDEX courses_path_idx ON courses(path);
CREATE INDEX courses_fingerprint_idx ON courses(fingerprint);
CREATE INDEX courses_missing_name_idx ON courses(missing_since, name);
CREATE INDEX sections_course_order_idx ON sections(course_id, order_index);
CREATE INDEX lessons_course_section_order_idx ON lessons(course_id, section_id, order_index);
CREATE INDEX lessons_path_idx ON lessons(path);
CREATE INDEX lessons_course_relative_idx ON lessons(course_id, relative_path);
CREATE INDEX notes_lesson_created_idx ON notes(lesson_id, created_at);
CREATE INDEX subtitles_lesson_order_idx ON lesson_subtitles(lesson_id, order_index);
CREATE INDEX activity_date_idx ON lesson_activity(activity_date);
CREATE INDEX activity_course_date_idx ON lesson_activity(course_id, activity_date);
CREATE INDEX activity_lesson_date_idx ON lesson_activity(lesson_id, activity_date);
