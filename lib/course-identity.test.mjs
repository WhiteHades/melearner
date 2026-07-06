import test from "node:test"
import assert from "node:assert/strict"
import {
  persistedLessonIdentitySignature,
  scannedLessonIdentitySignature,
  selectCourseIdentityMatch,
  selectLessonIdentityMatch,
} from "./course-identity.ts"

const scannedCourse = {
  id: "scanned-course",
  identityId: "scanned-course",
  name: "Arm Assembly",
  path: "/new-root/Renamed",
  fingerprint: "fp-1",
}

const persistedCourse = {
  id: "persisted-course",
  identity_id: "identity-1",
  path: "/old-root/Arm Assembly",
  fingerprint: "fp-1",
  last_accessed: "2026-07-06T00:00:00.000Z",
}

const persistedLesson = {
  id: "persisted-lesson",
  course_id: "persisted-course",
  section_name: "Intro",
  name: "welcome",
  path: "/old-root/Arm Assembly/Intro/welcome.mp4",
  relative_path: "Intro/welcome.mp4",
  type: "video",
  file_size: 7,
}

test("selectCourseIdentityMatch reuses one unambiguous fingerprint match", () => {
  const claimed = new Set()

  const result = selectCourseIdentityMatch(scannedCourse, undefined, [persistedCourse], claimed)

  assert.equal(result.match?.id, "persisted-course")
  assert.equal(result.warning, null)
  assert.deepEqual([...claimed], ["persisted-course"])
})

test("selectCourseIdentityMatch refuses ambiguous fingerprint matches", () => {
  const result = selectCourseIdentityMatch(
    scannedCourse,
    undefined,
    [persistedCourse, { ...persistedCourse, id: "copied-course" }],
    new Set()
  )

  assert.equal(result.match, null)
  assert.match(result.warning ?? "", /multiple existing courses/)
})

test("selectLessonIdentityMatch reuses relative paths after a course move", () => {
  const claimed = new Set()
  const scannedLesson = {
    name: "welcome",
    path: "/new-root/Renamed/Intro/welcome.mp4",
    relativePath: "Intro/welcome.mp4",
    sectionName: "Intro",
    type: "video",
    fileSize: 7,
  }

  const result = selectLessonIdentityMatch(
    "Arm Assembly",
    "persisted-course",
    scannedLesson,
    undefined,
    [persistedLesson],
    [],
    claimed
  )

  assert.equal(result.match?.id, "persisted-lesson")
  assert.equal(result.warning, null)
  assert.deepEqual([...claimed], ["persisted-lesson"])
})

test("selectLessonIdentityMatch refuses ambiguous lesson metadata", () => {
  const scannedLesson = {
    name: "welcome",
    path: "/new-root/Renamed/Intro/welcome.mp4",
    relativePath: null,
    sectionName: "Intro",
    type: "video",
    fileSize: 7,
  }

  const result = selectLessonIdentityMatch(
    "Arm Assembly",
    "persisted-course",
    scannedLesson,
    undefined,
    [],
    [persistedLesson, { ...persistedLesson, id: "duplicate-lesson" }],
    new Set()
  )

  assert.equal(result.match, null)
  assert.match(result.warning ?? "", /multiple existing lessons/)
})

test("lesson identity signatures match scanned and persisted metadata", () => {
  assert.equal(
    scannedLessonIdentitySignature({
      name: "welcome",
      path: "/new-root/Renamed/Intro/welcome.mp4",
      relativePath: "Intro/welcome.mp4",
      sectionName: "Intro",
      type: "video",
      fileSize: 7,
    }),
    persistedLessonIdentitySignature(persistedLesson)
  )
})
