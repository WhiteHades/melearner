import test from "node:test"
import assert from "node:assert/strict"
import { readFileSync } from "node:fs"
import { join } from "node:path"
import { processScanResult } from "./course-utils.ts"
import { selectCourseIdentityMatch } from "./course-identity.ts"

const repoRoot = process.cwd()
const fixtureRoot = join(repoRoot, "fixtures/parity")
const oracle = JSON.parse(readFileSync(join(fixtureRoot, "oracle-v1.json"), "utf8"))

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
