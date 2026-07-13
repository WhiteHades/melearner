import test from "node:test"
import assert from "node:assert/strict"
import { readFileSync } from "node:fs"
import { join } from "node:path"
import { DatabaseSync } from "node:sqlite"
import {
  __setDatabaseForTests,
  deleteNote,
  listLessonActivityDays,
  listNotesByLesson,
  loadPersistedLibrary,
  saveNote,
} from "./database.ts"
import { processScanResult } from "./course-utils.ts"
import { selectCourseIdentityMatch } from "./course-identity.ts"

const repoRoot = process.cwd()
const fixtureRoot = join(repoRoot, "fixtures/parity")
const oracle = JSON.parse(readFileSync(join(fixtureRoot, "oracle-v1.json"), "utf8"))

function extractMigrations() {
  const source = readFileSync(join(repoRoot, "crates/melearner-core/src/migrations.rs"), "utf8")
  const migrations = []
  const pattern = /MigrationDefinition \{\s*version: (\d+),\s*description: "([^"]+)",\s*sql: "([\s\S]*?)",\s*\}/g
  for (const match of source.matchAll(pattern)) {
    migrations.push({ version: Number(match[1]), description: match[2], sql: match[3] })
  }
  return migrations
}

class FixtureDatabase {
  constructor() {
    this.db = new DatabaseSync(":memory:")
    this.db.exec("PRAGMA foreign_keys = ON")
    for (const migration of extractMigrations()) this.db.exec(migration.sql)
    this.db.exec(readFileSync(join(fixtureRoot, "database-v16.sql"), "utf8"))
  }

  bind(params) {
    return Object.fromEntries(params.map((value, index) => [`$${index + 1}`, value]))
  }

  async execute(sql, params = []) {
    const statement = this.db.prepare(sql)
    if (params.length > 0) statement.run(this.bind(params))
    else statement.run()
  }

  async select(sql, params = []) {
    const statement = this.db.prepare(sql)
    const rows = params.length > 0 ? statement.all(this.bind(params)) : statement.all()
    return rows.map((row) => ({ ...row }))
  }

  close() {
    this.db.close()
  }
}

function courseSummary(course) {
  return {
    id: course.id,
    identityId: course.identityId,
    name: course.name,
    fingerprint: course.fingerprint,
    missingSince: course.missingSince,
    progress: course.progress,
    watchedDuration: course.watchedDuration,
    lessonIds: course.sections.flatMap((section) => section.lessons.map((lesson) => lesson.id)),
  }
}

test("migration 1-16 SQL opens the deterministic migrated database fixture", () => {
  const migrations = extractMigrations()
  assert.deepEqual(
    migrations.map(({ version, description }) => [version, description]),
    oracle.migrations,
  )

  const database = new FixtureDatabase()
  try {
    const tables = database.db.prepare(
      "SELECT name FROM sqlite_master WHERE type = 'table' ORDER BY name",
    ).all().map(({ name }) => name)
    assert.deepEqual(tables, [
      "app_settings",
      "courses",
      "lesson_activity",
      "lesson_subtitles",
      "lessons",
      "notes",
      "sections",
    ])
  } finally {
    database.close()
  }
})

test("migrated fixture preserves library, progress, notes, subtitles, long paths, and activity", async (t) => {
  const database = new FixtureDatabase()
  __setDatabaseForTests(database)
  t.after(() => {
    __setDatabaseForTests(null)
    database.close()
  })

  const library = await loadPersistedLibrary()
  assert.equal(library.libraryPath, oracle.database.libraryPath)
  assert.deepEqual(library.courses.map(courseSummary), oracle.database.courseSummaries)

  const video = library.courses[0].sections.flatMap((section) => section.lessons)
    .find((lesson) => lesson.id === "lesson-video")
  assert.deepEqual(
    {
      watchedTime: video.watchedTime,
      lastPosition: video.lastPosition,
      completed: video.completed,
    },
    oracle.database.videoProgress,
  )
  assert.deepEqual(video.subtitles, oracle.database.subtitles)

  const document = library.courses[0].sections.flatMap((section) => section.lessons)
    .find((lesson) => lesson.id === "lesson-document")
  assert.ok(document.path.length > 260, "the parity fixture must retain a long local path")
  assert.match(document.path, /Systems 日本語/)

  assert.deepEqual(await listNotesByLesson("lesson-video"), oracle.database.notes)
  const roundTripNote = {
    id: "note-round-trip",
    lessonId: "lesson-video",
    timestamp: 500,
    text: "Deterministic note round trip.",
    createdAt: "2026-07-09T11:00:00.000Z",
  }
  await saveNote(roundTripNote)
  assert.deepEqual(await listNotesByLesson("lesson-video"), [...oracle.database.notes, roundTripNote])
  await deleteNote(roundTripNote.id)
  assert.deepEqual(await listNotesByLesson("lesson-video"), oracle.database.notes)
  assert.deepEqual(await listLessonActivityDays(100000), oracle.database.activityDays)
})

test("identity fixture preserves marker priority and ambiguous fingerprint refusal", () => {
  const persisted = oracle.identity.persisted
  const marker = oracle.identity.markerMove
  const markerResult = selectCourseIdentityMatch(
    marker.scanned,
    undefined,
    persisted.filter((course) => course.identity_id === marker.scanned.markerIdentityId),
    [],
    new Set(),
  )
  assert.equal(markerResult.match?.id, marker.expectedId)
  assert.equal(markerResult.warning, null)

  const ambiguous = oracle.identity.ambiguousFingerprint
  const ambiguousResult = selectCourseIdentityMatch(
    ambiguous.scanned,
    undefined,
    [],
    persisted.filter((course) => course.fingerprint === ambiguous.scanned.fingerprint),
    new Set(),
  )
  assert.equal(ambiguousResult.match, null)
  assert.match(ambiguousResult.warning ?? "", new RegExp(ambiguous.expectedWarning))
})

test("scan fixture freezes media, document, quiz, and subtitle transformation", () => {
  const courses = processScanResult(oracle.scanContract.input)
  assert.equal(courses.length, 1)
  assert.equal(courses[0].identityId, "identity-marker")

  const lessons = courses[0].sections[0].lessons
  assert.deepEqual(lessons.map((lesson) => lesson.type), oracle.scanContract.expectedLessonTypes)
  assert.deepEqual(lessons[0].subtitles, oracle.scanContract.expectedVideoSubtitles)
  assert.equal(lessons.some((lesson) => lesson.name.endsWith(".srt")), false)
  assert.equal(lessons.some((lesson) => lesson.name.endsWith(".vtt")), false)
})

test("media, document, failure, and UI-flow manifests retain every parity gate", () => {
  assert.deepEqual(
    oracle.media.playbackCases.map((fixture) => fixture.id),
    ["h264-aac-mp4", "h264-multi-audio-mkv", "hevc-10bit", "external-srt-vtt", "chapters"],
  )
  assert.deepEqual(oracle.media.pathCases, ["unicode", "spaces", "long", "moved", "missing"])
  assert.deepEqual(
    oracle.media.typedFailures.map((failure) => failure.code),
    ["media_missing", "media_outside_root", "media_corrupt", "media_unsupported", "surface_detached"],
  )
  assert.deepEqual(
    oracle.documents.map((document) => document.extension),
    [".txt", ".md", ".html", ".docx", ".pdf", ".doc"],
  )
  assert.deepEqual(oracle.uiFlows, [
    "initial-library-load",
    "scan-with-warning",
    "search",
    "course-navigation",
    "lesson-progress-resume",
    "notes-create-list-delete",
    "document-open",
    "media-error-recovery",
  ])

  const nativePlayer = readFileSync(join(repoRoot, "src-tauri/src/native_player.rs"), "utf8")
  for (const failure of oracle.media.typedFailures) {
    assert.equal(
      nativePlayer.includes(failure.currentMessage),
      true,
      `current native-player behavior must retain ${failure.code}`,
    )
  }
})
