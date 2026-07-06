# Durable course identity uses local fingerprints before marker files

melearner needs course progress to survive common local-folder changes: renames, moves, and temporarily unavailable folders. Absolute paths alone are not stable enough for that workflow.

## Decision

melearner stores durable identity groundwork in local SQLite:

- `courses.identity_id` keeps the stable local identity associated with a course row.
- `courses.fingerprint` stores a non-absolute content fingerprint.
- `courses.missing_since` marks courses absent during a scan instead of deleting them.
- `lessons.relative_path` lets moved lessons reconnect to existing lesson progress.

The course fingerprint is derived from section names, lesson relative paths, lesson file sizes, and lesson file types. It excludes the absolute root path and course folder name, so a course can be renamed or moved without changing the fingerprint when its relative learning items are unchanged.

Matching is conservative:

1. Exact course path match.
2. One unambiguous course fingerprint match.
3. New course.

Lessons use exact path first, then relative path within the resolved course, then section/name/type/file-size metadata only when unambiguous. Ambiguous matches return scan warnings and do not reuse progress.

## Marker Files

The app does not write `.melearner-course.json` or any other marker file into user course folders. Marker files may be added later only behind explicit consent or a clear setting, because they modify user-owned course folders.

## Consequences

- Renaming or moving a course can preserve course and lesson progress without writing to the course folder.
- Temporarily missing courses keep progress, notes, sections, lessons, and subtitles in SQLite.
- Copied or duplicate courses can produce identical fingerprints; melearner must refuse ambiguous progress reuse instead of guessing.
- Fingerprints are local metadata, not telemetry or remote analytics.
