INSERT INTO courses (
  id, identity_id, name, path, fingerprint, last_accessed, created_at,
  thumbnail_source_path, last_scanned_at, missing_since
) VALUES
  (
    'course-marker', 'identity-marker', 'Systems 日本語', '/fixtures/library/Systems 日本語',
    'fp-marker', '2026-07-09T12:00:00.000Z', '2026-07-01T00:00:00.000Z',
    '/fixtures/library/Systems 日本語/01 入門/01 welcome.mp4',
    '2026-07-09T12:00:00.000Z', NULL
  ),
  (
    'course-missing', 'identity-missing', 'Archived Course', '/fixtures/library/Archived Course',
    'fp-shared', '2026-06-01T08:00:00.000Z', '2026-07-01T00:00:00.000Z',
    '/fixtures/library/Archived Course/01 Intro/01 archived.mkv',
    '2026-07-09T12:00:00.000Z', '2026-07-08T09:30:00.000Z'
  ),
  (
    'course-copy', 'identity-copy', 'Copied Course', '/fixtures/library/Copied Course',
    'fp-shared', NULL, '2026-07-01T00:00:00.000Z',
    '/fixtures/library/Copied Course/01 Intro/01 archived.mkv',
    '2026-07-09T12:00:00.000Z', '2026-07-08T09:30:00.000Z'
  );

INSERT INTO sections (id, course_id, name, order_index, created_at, updated_at) VALUES
  ('section-marker-intro', 'course-marker', '01 入門', 0, '2026-07-01T00:00:00.000Z', '2026-07-09T12:00:00.000Z'),
  ('section-marker-deep', 'course-marker', '02 Deep Storage', 1, '2026-07-01T00:00:00.000Z', '2026-07-09T12:00:00.000Z'),
  ('section-missing-intro', 'course-missing', '01 Intro', 0, '2026-07-01T00:00:00.000Z', '2026-07-09T12:00:00.000Z'),
  ('section-copy-intro', 'course-copy', '01 Intro', 0, '2026-07-01T00:00:00.000Z', '2026-07-09T12:00:00.000Z');

INSERT INTO lessons (
  id, course_id, section_id, name, path, relative_path, type, duration,
  file_size, watched_time, completed, order_index, last_position, updated_at
) VALUES
  (
    'lesson-video', 'course-marker', 'section-marker-intro', '01 welcome',
    '/fixtures/library/Systems 日本語/01 入門/01 welcome.mp4', '01 入門/01 welcome.mp4',
    'video', 600, 1048576, 320, 0, 0, 318.5, '2026-07-09T12:00:00.000Z'
  ),
  (
    'lesson-document', 'course-marker', 'section-marker-deep', 'reference',
    '/fixtures/library/Systems 日本語/02 Deep Storage/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb/cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc/reference.md',
    '02 Deep Storage/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb/cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc/reference.md',
    'document', 0, 4096, 0, 1, 0, 0, '2026-07-09T12:00:00.000Z'
  ),
  (
    'lesson-missing-video', 'course-missing', 'section-missing-intro', '01 archived',
    '/fixtures/library/Archived Course/01 Intro/01 archived.mkv', '01 Intro/01 archived.mkv',
    'video', 300, 2097152, 300, 1, 0, 300, '2026-07-09T12:00:00.000Z'
  ),
  (
    'lesson-copy-video', 'course-copy', 'section-copy-intro', '01 archived',
    '/fixtures/library/Copied Course/01 Intro/01 archived.mkv', '01 Intro/01 archived.mkv',
    'video', 300, 2097152, 0, 0, 0, 0, '2026-07-09T12:00:00.000Z'
  );

INSERT INTO lesson_subtitles (id, lesson_id, path, language, label, order_index, created_at) VALUES
  ('subtitle-video-en', 'lesson-video', '/fixtures/library/Systems 日本語/01 入門/01 welcome.en.srt', 'en', 'English', 0, '2026-07-01T00:00:00.000Z'),
  ('subtitle-video-ja', 'lesson-video', '/fixtures/library/Systems 日本語/01 入門/01 welcome.ja.vtt', 'ja', '日本語', 1, '2026-07-01T00:00:00.000Z');

INSERT INTO notes (id, lesson_id, timestamp, text, created_at) VALUES
  ('note-video-1', 'lesson-video', 42.5, 'Review the ownership diagram.', '2026-07-08T10:00:00.000Z'),
  ('note-video-2', 'lesson-video', 318.0, '復習: final example', '2026-07-09T10:00:00.000Z');

INSERT INTO lesson_activity (
  id, course_id, lesson_id, activity_date, watched_seconds, completed, created_at
) VALUES
  ('activity-video-1', 'course-marker', 'lesson-video', '2026-07-08', 120, 0, '2026-07-08T10:00:00.000Z'),
  ('activity-video-2', 'course-marker', 'lesson-video', '2026-07-09', 200, 0, '2026-07-09T10:00:00.000Z'),
  ('activity-document-1', 'course-marker', 'lesson-document', '2026-07-09', 0, 1, '2026-07-09T10:05:00.000Z');

INSERT INTO app_settings (key, value, updated_at) VALUES
  ('libraryPath', '/fixtures/library', '2026-07-09T12:00:00.000Z');
