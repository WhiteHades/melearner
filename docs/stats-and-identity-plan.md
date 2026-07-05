# Stats and Course Identity Plan

This is a design plan, not implemented behavior yet.

## Goals

- Show useful learning stats without sending data anywhere.
- Preserve progress and stats when a course folder is renamed or moved.
- Gracefully handle deleted or unavailable course folders.
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

Current course identity is path-based enough that renames and moves can break continuity. A more durable model needs a stable course identifier.

Preferred approach:

1. Add a small metadata file in each course root, for example `.melearner-course.json`.
2. Store a generated course UUID in that file.
3. Keep path as mutable metadata, not identity.
4. On scan, if the marker file exists, match by marker ID first.
5. If the marker is missing, fall back to a fingerprint of stable signals: folder name, relative file names, file sizes, and maybe duration metadata.
6. If a likely renamed/moved course is found, preserve the old course record and update its path.

Important constraint:

- Writing marker files modifies user course folders. The app should ask first or provide a clear setting before writing hidden metadata into course roots.

Deleted or unavailable courses:

- Do not delete progress immediately.
- Mark the course as missing/unavailable.
- Keep progress and stats in SQLite.
- Let the user reconnect the course to a new folder.

## Storage Model Changes Needed

Likely new fields/tables:

- `courses.identity_id`: stable UUID independent of path
- `courses.path`: latest known path
- `courses.missing_since`: nullable timestamp
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

- Should melearner write `.melearner-course.json` files into course folders by default, ask per library, or never write to user folders?
- Should missing courses stay visible in the library or move to a separate recovery/settings view?
- How much historical activity should be retained?
- Should stats include only completed watch time, all played time, or both?
