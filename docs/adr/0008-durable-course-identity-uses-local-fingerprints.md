# Durable course identity uses local fingerprints and opt-in marker files

melearner needs course progress to survive common local-folder changes: renames, moves, and temporarily unavailable folders. Absolute paths alone are not stable enough for that workflow.

## Decision

melearner stores durable identity groundwork in local SQLite:

- `courses.identity_id` keeps the stable local identity associated with a course row.
- `courses.fingerprint` stores a non-absolute content fingerprint.
- `courses.missing_since` marks courses absent during a scan instead of deleting them.
- `lessons.relative_path` lets moved lessons reconnect to existing lesson progress.
- Optional `.melearner-course.json` marker files store the same course identity in the course folder after the user enables marker writing.

The course fingerprint is derived from section names, lesson relative paths, lesson file sizes, and lesson file types. It excludes the absolute root path and course folder name, so a course can be renamed or moved without changing the fingerprint when its relative learning items are unchanged.

Matching is conservative:

1. Exact course path match.
2. One unambiguous marker identity match.
3. One unambiguous course fingerprint match.
4. New course.

Lessons use exact path first, then relative path within the resolved course, then section/name/type/file-size metadata only when unambiguous. Ambiguous matches return scan warnings and do not reuse progress.

## Marker Files

The app writes `.melearner-course.json` only when marker files are enabled from the dashboard. Marker writing is opt-in because it modifies user-owned course folders.

Existing marker files are read during scans. Duplicate marker IDs in one scan are ignored with warnings, and marker files with a different existing identity are not overwritten.

## Consequences

- Renaming or moving a course can preserve course and lesson progress through exact paths, marker IDs, or safe fingerprints.
- Temporarily missing courses keep progress, notes, sections, lessons, and subtitles in SQLite.
- Copied or duplicate courses can produce identical fingerprints or duplicated marker files; melearner must refuse ambiguous progress reuse instead of guessing.
- Fingerprints and marker IDs are local metadata, not telemetry or remote analytics.
