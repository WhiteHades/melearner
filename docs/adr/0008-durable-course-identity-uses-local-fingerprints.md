# Durable course identity uses local fingerprints and automatic marker files

melearner needs course progress to survive common local-folder changes: renames, moves, and temporarily unavailable folders. Absolute paths alone are not stable enough for that workflow.

## Decision

melearner stores durable identity groundwork in local SQLite:

- `courses.identity_id` keeps the stable local identity associated with a course row.
- `courses.fingerprint` stores a non-absolute content fingerprint.
- `courses.missing_since` marks courses absent during a scan instead of deleting them.
- `lessons.relative_path` lets moved lessons reconnect to existing lesson progress.
- Automatic `.melearner-course.json` marker files store the same course identity in available course folders.

The course fingerprint is derived from section names, lesson relative paths, lesson file sizes, and lesson file types. It excludes the absolute root path and course folder name, so a course can be renamed or moved without changing the fingerprint when its relative learning items are unchanged.

Matching is conservative:

1. Exact course path match.
2. One unambiguous marker identity match.
3. One unambiguous course fingerprint match.
4. New course.

Lessons use exact path first, then relative path within the resolved course, then section/name/type/file-size metadata only when unambiguous. Ambiguous matches return scan warnings and do not reuse progress.

## Marker Files

The app writes `.melearner-course.json` automatically for available course folders after scans and after loading an existing local library. There is no user-facing marker toggle. Course identity should be durable without asking the user to understand implementation details.

Existing marker files are read during scans. Duplicate marker IDs in one scan are ignored with warnings, and marker files with a different existing identity are not overwritten. Missing courses are skipped when writing markers.

## Consequences

- Renaming or moving a course can preserve course and lesson progress through exact paths, marker IDs, or safe fingerprints.
- Temporarily missing courses keep progress, notes, sections, lessons, and subtitles in SQLite.
- Copied or duplicate courses can produce identical fingerprints or duplicated marker files; melearner must refuse ambiguous progress reuse instead of guessing.
- Fingerprints and marker IDs are local metadata, not telemetry or remote analytics.
