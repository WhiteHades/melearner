# Plan 001: Add durable course identity groundwork without writing course folders

> **Executor instructions**: Follow this plan step by step. Run every verification command and confirm the expected result before moving to the next step. If anything in the "STOP Conditions" section occurs, stop and report. Do not improvise around product decisions.
>
> **Drift check (run first)**: `git diff --stat 98a1633..HEAD -- src-tauri/src/lib.rs src-tauri/src/scanner.rs lib/database.ts lib/course-utils.ts lib/tauri.ts types/index.ts docs/stats-and-identity-plan.md docs/adr`
> If any in-scope file changed since this plan was written, compare the "Current State" excerpts against the live code before proceeding. On a mismatch, treat it as a STOP condition.

## Status

- **Priority**: P1
- **Effort**: L
- **Risk**: MED
- **Depends on**: none
- **Category**: direction
- **Planned at**: commit `98a1633`, 2026-07-06
- **Result**: DONE. Implemented with local fingerprints, lesson relative paths, missing-course state, ambiguity warnings, minimal missing-course UI, and docs/ADR updates.

## Implementation Notes

- Added `courses.identity_id`, `courses.fingerprint`, `courses.missing_since`, and `lessons.relative_path`.
- Added scanner fingerprints that exclude absolute roots and course folder names.
- Added conservative course and lesson matching with warnings for ambiguous reuse.
- Added missing-folder cards that preserve progress but cannot be opened until the folder is scanned again.
- Did not write marker files into user course folders.

## Why This Matters

melearner currently preserves progress only when scanned course and lesson paths stay stable. The product docs already name this as the next important gap: renamed, moved, or temporarily unavailable course folders should not silently break progress continuity. This plan adds the non-writing groundwork first: stable database identity, fingerprint-based rename/move matching, missing-course state, and tests. It explicitly does not write marker files into user course folders, because the repo docs say that needs user consent or a setting.

## Current State

Relevant files:

- `src-tauri/src/scanner.rs` scans folders and currently derives course, section, and lesson IDs from paths.
- `src-tauri/src/lib.rs` owns SQLite migrations.
- `lib/database.ts` hydrates and syncs scanned courses into SQLite, currently by exact `path`.
- `lib/course-utils.ts`, `lib/tauri.ts`, and `types/index.ts` define scan and domain shapes crossing Rust and TypeScript.
- `docs/stats-and-identity-plan.md` records the durable identity and stats direction.

Original path-derived identity at planning time:

```rust
// src-tauri/src/scanner.rs:111
fn hash_path_to_id(path: &Path) -> Box<str> {
    let mut h = new_hasher();
    h.write(path.to_string_lossy().as_bytes());
    format!("{:016x}", h.finish()).into()
}

// src-tauri/src/scanner.rs:315
Some(FileEntry {
    id: hash_path_to_id(path),
    path: path.to_string_lossy().into_owned().into_boxed_str(),
    name,
    file_type,
    size,
})

// src-tauri/src/scanner.rs:419
CourseData {
    id: hash_path_to_id(course_path),
    name: course_path.file_name()...
}
```

Original persistence lookup used exact paths:

```ts
// lib/database.ts:153
async function selectPersistedCourses(paths: string[]): Promise<PersistedCourseRow[]> {
  ...
  `SELECT id, name, path, total_duration, watched_duration, last_accessed, thumbnail_source_path
   FROM courses
   WHERE path IN (${createPlaceholders(batch.length)})`
}

// lib/database.ts:174
async function selectPersistedLessons(paths: string[]): Promise<PersistedLessonRow[]> {
  ...
  `SELECT id, course_id, section_id, section_name, name, path, type, duration, file_size,
          watched_time, last_position, completed, order_index
   FROM lessons
   WHERE path IN (${createPlaceholders(batch.length)})`
}

// lib/database.ts:442
const resolvedCourses = courses.map((course) => {
  const persistedCourse = persistedCourseByPath.get(course.path)
  const courseId = persistedCourse?.id ?? course.id
  ...
  const persistedLesson = persistedLessonByPath.get(lesson.path)
```

Original sync deleted absent courses inside the scanned root:

```ts
// lib/database.ts:614
await database.execute(
  `DELETE FROM courses WHERE (path = $1 OR path LIKE $2 ESCAPE '~') AND (last_scanned_at IS NULL OR last_scanned_at <> $3)`,
  [libraryPath, childPathPattern(libraryPath), scanStamp]
)
```

Product intent and constraints:

```md
// docs/stats-and-identity-plan.md:55
Course identity previously depended enough on paths that renames and moves could break continuity.

// docs/stats-and-identity-plan.md:70
Writing marker files modifies user course folders. The app should ask first or provide a clear setting before writing hidden metadata into course roots.

// docs/stats-and-identity-plan.md:72
Deleted or unavailable courses:
- Do not delete progress immediately.
- Mark the course as missing/unavailable.
- Keep progress and stats in SQLite.
```

Domain vocabulary:

```md
// CONTEXT.md:47
Course identity:
The local identity model that keeps a course connected to its progress when its folder is renamed, moved, temporarily missing, or scanned again.

// CONTEXT.md:51
Learning activity:
Historical watch/read events used for future stats and heatmaps.
```

## Commands You Will Need

| Purpose | Command | Expected On Success |
|---------|---------|---------------------|
| Typecheck | `pnpm type-check` | exit 0, no TypeScript errors |
| Lint | `rtk lint` | `ESLint: No issues found` |
| Web build | `pnpm build` | exit 0, Next.js build succeeds |
| Rust tests | `rtk cargo test --manifest-path src-tauri/Cargo.toml` | all tests pass |
| Rust check | `rtk cargo check --manifest-path src-tauri/Cargo.toml` | exit 0 |
| Diff check | `git diff --check` | no output, exit 0 |

## Scope

In scope:

- `src-tauri/src/lib.rs`
- `src-tauri/src/scanner.rs`
- `lib/database.ts`
- `lib/course-utils.ts`
- `lib/tauri.ts`
- `types/index.ts`
- New focused tests in existing Rust test modules and TypeScript test files if a local test pattern exists. If there is no TypeScript test runner, keep TypeScript verification to `pnpm type-check`, `rtk lint`, and `pnpm build`.
- `docs/adr/0008-*.md` if a new decision record is needed for the non-writing identity policy.
- `docs/stats-and-identity-plan.md` only to mark what this plan implements or defers.

Out of scope:

- Writing `.melearner-course.json` or any other marker file into user course folders.
- Building stats dashboards, charts, or heatmaps.
- Changing playback fallback behavior.
- Changing release packaging.
- Deleting persisted progress for missing courses.
- Broad UI redesign of the library or course viewer.

## Git Workflow

- Branch: `advisor/001-durable-course-identity-groundwork`
- Commit message style: conventional one-line lowercase messages, matching recent history such as `fix: harden playback fallback` and `chore: bump 0.1.8`.
- Do not push unless the operator asks.

## Steps

### Step 1: Add database fields for stable identity and missing state

Add a new SQLite migration after the current latest migration in `src-tauri/src/lib.rs`.

Target shape:

- `courses.identity_id TEXT`
- `courses.fingerprint TEXT`
- `courses.missing_since TEXT`
- indexes for `courses.identity_id` and `courses.fingerprint`

Backfill existing rows:

- Set `identity_id` to the existing `id` for current rows.
- Set `fingerprint` to `NULL` for old rows if no fingerprint can be reconstructed cheaply.
- Leave `missing_since` as `NULL`.

Update `PersistedCourseRow` in `lib/database.ts` and every course SELECT to include the new columns. Keep old rows compatible by treating null `identity_id` as `id`.

Verify:

```bash
pnpm type-check
rtk cargo check --manifest-path src-tauri/Cargo.toml
```

Expected result: both commands exit 0.

### Step 2: Emit a deterministic non-path course fingerprint from scans

Extend Rust `CourseData` and TypeScript `CourseData` with `fingerprint: string`.

Compute the primary fingerprint in `src-tauri/src/scanner.rs` from stable, non-absolute-path signals:

- section names
- lesson relative paths from the course root
- lesson file sizes
- lesson file types

Do not include the absolute root path or the course folder basename in the primary fingerprint. A folder rename should preserve identity when relative contents are unchanged. If you want the basename as a weak disambiguation signal, store it separately from the primary fingerprint and do not require it for an automatic exact match.

Add Rust tests proving:

- Moving a course folder to a different parent preserves the same fingerprint when relative contents are unchanged.
- Renaming the course folder preserves the same fingerprint when relative contents are unchanged.
- Adding/removing a lesson changes the fingerprint.

Verify:

```bash
rtk cargo test --manifest-path src-tauri/Cargo.toml scanner
pnpm type-check
```

Expected result: scanner tests pass and TypeScript exits 0.

### Step 3: Resolve scanned courses by path first, then fingerprint

In `lib/database.ts`, update `syncLibrary` so it resolves persisted courses in this order:

1. Exact path match.
2. Fingerprint match among existing non-missing courses under the same library.
3. New scanned course identity.

When a fingerprint match is found:

- Reuse the existing course `id` and `identity_id`.
- Update `path` to the new path.
- Clear `missing_since`.
- Preserve `last_accessed`, progress, and thumbnail source when still valid.

Do not overwrite an existing course if the same fingerprint matches multiple persisted rows. That is ambiguous. Return a scan warning or STOP if the current scan warning path cannot carry this safely without UI work.

Verify:

```bash
pnpm type-check
rtk lint
pnpm build
```

Expected result: all commands exit 0.

### Step 4: Preserve lesson progress across course moves

Exact lesson path matching will fail after a course move. Add a non-absolute lesson match inside a resolved course:

- Use relative lesson path from the course root when available.
- Otherwise use `(section name, lesson name, type, file size)` as a fallback.
- Keep exact path match as the first choice.

Add the minimal fields needed to scanned lesson/course shapes. Prefer deriving relative paths in Rust once rather than recomputing with fragile string replacement in TypeScript.

When a lesson is matched after a move:

- Reuse the existing lesson `id`.
- Update `path` to the new absolute path.
- Preserve `watched_time`, `last_position`, and `completed`.

Verify:

```bash
pnpm type-check
pnpm build
```

Expected result: both commands exit 0.

### Step 5: Mark missing courses instead of deleting them

Replace the final course deletion in `syncLibrary` for rows under the scanned library with a missing-state update:

- Set `missing_since = scanStamp` for absent courses.
- Keep lessons, sections, notes, subtitles, and progress.
- Keep rows visible to `loadPersistedLibrary` only if the UI can represent missing state. If the UI cannot yet represent missing state, load missing rows but add a minimal `missingSince` field to the `Course` type and render a restrained disabled/missing label in the library card.

Do not cascade delete course progress as part of this plan.

Verify:

```bash
pnpm type-check
rtk lint
pnpm build
```

Expected result: all commands exit 0.

### Step 6: Add regression coverage for rename/move preservation

Add tests at the best available seam:

- Rust scanner tests for fingerprint behavior.
- TypeScript database tests for `syncLibrary` if the repo already has a test runner or an easy local harness. If no TypeScript test runner exists, add a small documented manual verification script under `plans/` only if directed by the operator; otherwise STOP and report that the repo lacks a database test seam.

Minimum cases:

- Same course moved to a different parent preserves course `id`.
- Same lesson moved with the course preserves lesson `id`, `watchedTime`, `lastPosition`, and `completed`.
- Course absent from a later scan is marked missing instead of deleted.
- A new unrelated course with similar names does not steal another course's progress.

Verify:

```bash
rtk cargo test --manifest-path src-tauri/Cargo.toml
pnpm type-check
rtk lint
pnpm build
git diff --check
```

Expected result: all commands exit 0.

## Test Plan

- Add scanner unit tests near the existing `src-tauri/src/scanner.rs` tests.
- Add database sync coverage only if the repo has or can accept a focused test seam without adding a broad new test framework.
- Use current command gates as the minimum verification baseline:
  - `rtk cargo test --manifest-path src-tauri/Cargo.toml`
  - `pnpm type-check`
  - `rtk lint`
  - `pnpm build`
  - `git diff --check`

## Done Criteria

All must hold:

- [x] Course rows have stable `identity_id`, `fingerprint`, and nullable `missing_since` fields.
- [x] Scanning the same course contents from a different parent can preserve the existing course row.
- [x] Lessons moved with a course preserve progress fields.
- [x] Missing courses are retained in SQLite and marked missing instead of being deleted.
- [x] No code writes marker files into user course folders.
- [x] Verification commands in the Test Plan exit 0.
- [x] `plans/README.md` status row for this plan is updated.

## Historical STOP Conditions

These applied while executing the plan:

Stop and report if:

- The live code no longer matches the excerpts above.
- The implementation appears to require writing marker files into user folders.
- Fingerprint matching produces ambiguous matches and there is no existing warning/UI path to surface that safely.
- Preserving missing courses requires a broad library UI redesign.
- The repo lacks a reasonable test seam for database sync behavior and the change would otherwise be verified only by manual inspection.
- Any verification command fails twice after a focused fix attempt.

## Maintenance Notes

- This plan deliberately chooses a non-writing identity baseline. A later plan can add opt-in `.melearner-course.json` marker files after a consent setting or prompt exists.
- Stats dashboards and heatmaps should build on `identity_id` and future `lesson_activity`, not on absolute paths.
- Reviewers should scrutinize collision and ambiguity handling. Reusing progress for the wrong course is worse than failing to match.
