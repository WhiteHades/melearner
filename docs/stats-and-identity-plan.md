# Stats and Course Identity

This document records the current stats and course-identity behavior plus remaining product decisions.

## Implemented Behavior

- The dashboard shows local library stats: total courses, missing courses, completion percent, watched progress, storage size, section count, media split, top courses, and a 12-week activity heatmap.
- Lesson progress updates append rows to `lesson_activity` for daily watched seconds, touched lessons, and completions.
- Course rows are retained when folders are temporarily missing. Missing courses keep progress, notes, sections, lessons, subtitles, stats inputs, and activity history in SQLite.
- Renamed or moved courses can reconnect to existing progress by exact path, marker identity, or one unambiguous fingerprint match.
- Marker files are automatic local metadata. melearner writes `.melearner-course.json` into available course folders and uses its marker ID before fingerprint matching on later scans.
- Marker writing has no dashboard toggle. The app skips missing courses, refuses to overwrite marker files with a different existing identity, and reports warnings instead of guessing.

## Stats Model

Current stats are derived locally from:

- `courses`
- `sections`
- `lessons`
- `lesson_activity`

The app does not maintain a separate `course_stats` table. Aggregate stats are computed from current lesson rows and historical activity rows.

Implemented stats:

- Total courses
- Available and missing courses
- Total sections
- Total lessons
- Completed lessons
- Completion percent
- Total file size
- File size by lesson type
- Watched progress seconds
- Available duration when known
- Per-course lesson, storage, completion, and watched-progress summaries
- Daily activity heatmap from `lesson_activity`

## Identity Model

Course identity is local-first and conservative:

1. Exact course path match.
2. One unambiguous marker identity match from `.melearner-course.json`.
3. One unambiguous fingerprint match.
4. New course.

Lesson identity inside a resolved course uses:

1. Exact lesson path.
2. Relative path within the course.
3. Section/name/type/file-size metadata only when unambiguous.
4. New lesson.

Ambiguous matches produce scan warnings and do not reuse progress. Assigning progress to the wrong course is worse than failing to match.

## Storage Model

Implemented fields and tables:

- `courses.identity_id`: stable local identity associated with the course row
- `courses.fingerprint`: non-absolute course content fingerprint
- `courses.path`: latest known path
- `courses.missing_since`: nullable timestamp for unavailable courses
- `lessons.relative_path`: lesson path relative to its course root
- `lesson_activity`: append-only daily progress events for heatmaps and history

The primary fingerprint is derived from section names, lesson relative paths, lesson file sizes, and lesson file types. It excludes the absolute root path and course folder name.

## Marker Files

Marker files are automatic because durable identity should not require a user-visible implementation setting.

Format:

```json
{
  "version": 1,
  "identityId": "course identity value"
}
```

Rules:

- Scanner reads `.melearner-course.json` if present.
- Available scanned courses get marker files after sync.
- Sync matches marker identity before fingerprint matching.
- Duplicate marker IDs in the same scan are ignored with warnings.
- Existing marker files with a different identity are not overwritten.
- Missing courses are skipped when writing markers.

## Open Decisions

- Should missing courses stay visible in the main library long-term or move to a separate recovery/settings view?
- Keep the activity heatmap at the current 12-week window for now. Revisit configurability only if it improves the learning UI without adding settings complexity.
- Should stats distinguish played time from position-derived progress if a future player records wall-clock playback time?
