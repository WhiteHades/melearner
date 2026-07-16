# Stats and Course Identity

This document records the canonical stats and course-identity behavior and the resolved product decisions.

## Product Behavior

- The dashboard shows local Library stats: total Courses, missing Courses, completion percent, position-derived Progress time, storage size, Section count, media split, top Courses, and a 12-week activity heatmap.
- Lesson Progress updates append rows to `lesson_activity` for daily positive position advances, touched Lessons, and completions.
- Course rows are retained when folders are temporarily missing. Missing courses keep progress, notes, sections, lessons, subtitles, stats inputs, and activity history in SQLite.
- Missing courses remain in the main Library with their retained Progress. Activating one opens recovery actions to locate the Course, rescan the current root, or change the root; it does not open stale Lessons.
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

### Canonical snapshot fields

The Rust `LibraryStats` response uses camelCase JSON and contains exactly:

- `revision`: current Library revision; requests for another revision fail as stale.
- `totalCourses`: all retained Course rows in scope, including missing Courses.
- `availableCourses`: Course rows whose `missing_since` is null.
- `missingCourses`: Course rows whose `missing_since` is non-null.
- `sections`: Section rows belonging to in-scope Courses.
- `lessons`: Lesson rows belonging to in-scope Courses.
- `completedLessons`: sum of the boolean `lessons.completed` values.
- `completionPercent`: `0` for no Lessons; otherwise `(completedLessons * 100 + lessons / 2) / lessons`, using nonnegative integer half-up rounding.
- `bytes`: sum of `lessons.file_size`.
- `watchedSeconds`: sum of `lessons.watched_time`.
- `totalSeconds`: sum of known `lessons.duration`; unknown duration contributes `0`.
- `mediaTypes`: rows with `type`, `lessons`, `bytes`, `completed`, and `watchedSeconds`, grouped by Lesson type and ordered `video`, `audio`, `document`, then `quiz`.
- `topCourses`: at most four rows with `id`, `name`, `lessons`, `completedLessons`, `bytes`, and `watchedSeconds`, ordered by `watchedSeconds` descending, `bytes` descending, natural Course name, then ID.

When a root is configured, every aggregate uses the same selected-root Course scope; otherwise it uses all Course rows. Retained missing Courses remain in that scope and therefore retain their contribution to counts, Progress, storage, media, and top-Course summaries.

### Canonical activity fields

The 12-week heatmap requests an 84-day `ActivityDayPage` with `revision`, `offset`, `total`, and `rows`. Each returned active date row has `date`, `watchedSeconds`, `lessonsTouched`, and `completions`, ordered oldest to newest. The UI fills dates with no row as zero-valued cells; it does not infer extra activity.

The database fields named `watched_time` and `watched_seconds`, and the API field `watchedSeconds`, are position-derived Progress. A Progress write replaces `lessons.watched_time`; `lesson_activity.watched_seconds` records only `max(new watched_time - previous watched_time, 0)`. A completion-state change records an activity row even when that delta is zero, and `completions` counts only transitions into completed. These fields do not measure wall-clock time spent playing. melearner does not maintain a separate played-time clock.

The activity heatmap remains a fixed 12-week window. It can be revisited only if configurability improves the learning UI without adding settings complexity.

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
- Available scanned Courses get marker files after the database reconciliation transaction commits and the new Library revision is installed.
- Sync matches marker identity before fingerprint matching.
- Duplicate marker IDs in the same scan are ignored with warnings.
- Existing marker files with a different identity are not overwritten.
- Missing courses are skipped when writing markers.
- Marker writes are nontransactional filesystem side effects. A write failure adds a warning to the committed scan result and does not roll back the new revision.
