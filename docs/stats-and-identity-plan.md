# Stats and Course Identity Plan

Durable course identity groundwork is implemented. Stats dashboards, heatmaps, optional marker files, and historical learning activity are still planned work.

## Goals

- Show useful learning stats without sending data anywhere.
- Preserve progress when a course folder is renamed or moved. Implemented for courses and lessons through local fingerprints and relative lesson paths.
- Gracefully handle deleted or unavailable course folders. Implemented by marking missing courses instead of deleting their progress.
- Use shadcn-compatible chart blocks where they fit the design system.

## User Stories

- As a learner, I want total library stats so I know how much content I have and how much I have completed.
- As a learner, I want per-course stats so I can decide what to continue next.
- As a learner, I want storage stats so I can see which courses take the most disk space.
- As a learner, I want watch-time stats so I can see how much I actually studied.
- As a learner, I want an activity heatmap so I can see my learning consistency.
- As a learner, I want my progress preserved if I rename a course folder.
- As a learner, I want deleted or missing folders handled clearly instead of losing progress silently.

## Candidate Stats

Total stats:

- Total courses
- Total sections
- Total lessons
- Total storage used, formatted as MB/GB/TB
- Video/audio/document storage split
- Total watch time available
- Total watch time viewed
- Completion percent
- Active streak or recent active days

Per-course stats:

- Course storage size
- Lesson count by type
- Completed lessons
- Watch time viewed
- Watch time remaining
- Last accessed
- Largest files
- Missing-file state if the course folder is unavailable

Activity stats:

- Daily watch minutes
- Daily lessons touched
- Weekly totals
- Heatmap by day
- Course contribution to each week/month

## Identity Model

Current behavior:

- `courses.identity_id` stores the stable local identity associated with a course row.
- `courses.fingerprint` stores a non-absolute fingerprint derived from section names, lesson relative paths, lesson file sizes, and lesson file types.
- `lessons.relative_path` stores the lesson path relative to the course root so lesson progress can survive a course-folder move.
- `courses.missing_since` marks courses that were absent during a scan. Missing courses keep progress, notes, sections, lessons, and subtitles in SQLite.
- The primary fingerprint excludes the absolute root path and course folder name. Renaming or moving a course can preserve progress when its relative learning items are unchanged.
- Matching is conservative: exact path first, then one unambiguous fingerprint match, then a new course. Ambiguous matches produce scan warnings and do not reuse progress.
- The app does not write marker files or hidden metadata into user course folders.

Future optional marker-file approach:

1. Ask for consent or provide a clear setting before writing to course folders.
2. Add a small metadata file in each course root, for example `.melearner-course.json`.
3. Store a generated course UUID in that file.
4. Keep path as mutable metadata, not identity.
5. On scan, if the marker file exists, match by marker ID first.
6. If the marker is missing, fall back to the existing local fingerprint model.

Important constraint:

- Writing marker files modifies user course folders. The app must ask first or provide a clear setting before writing hidden metadata into course roots.

Deleted or unavailable courses:

- Do not delete progress immediately. Implemented.
- Mark the course as missing/unavailable. Implemented with `missing_since`.
- Keep progress and stats in SQLite. Progress is retained; aggregate stats are still planned.
- Let the user reconnect the course to a new folder. Implemented through safe fingerprint matching.

## Storage Model

Implemented fields:

- `courses.identity_id`: stable local identity value independent of path
- `courses.fingerprint`: non-absolute course content fingerprint
- `courses.path`: latest known path
- `courses.missing_since`: nullable timestamp
- `lessons.relative_path`: lesson path relative to its course root

Likely future fields/tables:

- `course_stats`: aggregated storage and duration values
- `lesson_activity`: append-only progress events for heatmaps and history

Existing progress fields can keep serving resume playback, but heatmaps need historical events. Current state-only progress is not enough to reconstruct old daily activity.

## Bklit UI Components

Local reference repo: `~/Codes/bklit-ui`.

Registry setup:

```json
{
  "registries": {
    "@bklit": "https://ui.bklit.com/r/{name}.json"
  }
}
```

Most relevant components:

- `@bklit/heatmap-chart` for daily watch-time activity.
- `@bklit/bar-chart` for per-course storage and watch-time comparisons.
- `@bklit/gauge-chart` for completion or storage-used gauges.
- `@bklit/ring-chart` or `@bklit/pie-chart` for storage split by media type.
- `@bklit/composed-chart` for combined watch-time and completion trends.
- `@bklit/stat-card-line-01` and `@bklit/stat-card-area-01` for top-level stat cards.

Implementation note:

- These blocks may bring dependencies such as `motion`, `@visx/*`, `d3-*`, and `@number-flow/react`.
- Add only the components actually used by the stats page.
- Keep imports aligned with this repo's `components.json` aliases and lucide icon setup.

## Open Decisions

- Should melearner offer opt-in `.melearner-course.json` files, and if so should consent be per library or global?
- Should missing courses stay visible in the library long-term or move to a separate recovery/settings view?
- How much historical activity should be retained?
- Should stats include only completed watch time, all played time, or both?
