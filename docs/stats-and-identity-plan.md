# Stats and Course Identity

This document records canonical stats and Learning activity behavior. ADR 0008 is the sole authority for Course identity, fingerprints, retained missing Courses, and marker files.

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

The C++ `LibraryStats` value contains exactly:

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

The 12-week heatmap requests an 84-day `ActivityDayPage` with `revision`, `throughDate`, `offset`, `total`, and `rows`. `throughDate` is the UTC date captured by the database transaction and anchors the returned window. Each returned active date row has `date`, `watchedSeconds`, `lessonsTouched`, and `completions`, ordered oldest to newest. The UI fills dates with no row as zero-valued cells; it does not infer extra activity.

The database fields named `watched_time` and `watched_seconds`, and the API field `watchedSeconds`, are position-derived Progress. A Progress write replaces `lessons.watched_time`; `lesson_activity.watched_seconds` records only `max(new watched_time - previous watched_time, 0)`. A completion-state change records an activity row even when that delta is zero, and `completions` counts only transitions into completed. These fields do not measure wall-clock time spent playing. melearner does not maintain a separate played-time clock.

The activity heatmap remains a fixed 12-week window. It can be revisited only if configurability improves the learning UI without adding settings complexity.

## Identity dependency

Stats use the Course and Lesson rows retained by ADR 0008. Identity matching and marker writes happen before a new Library revision becomes visible. Stats do not implement another identity order, fingerprint, marker parser, or missing-Course policy.
