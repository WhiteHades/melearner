import test from "node:test"
import assert from "node:assert/strict"
import { buildLearningStats } from "./stats.ts"

function lesson(overrides) {
  return {
    id: "lesson",
    courseId: "course",
    sectionName: "Intro",
    name: "lesson",
    path: "/library/course/lesson.mp4",
    relativePath: "Intro/lesson.mp4",
    type: "video",
    duration: 100,
    fileSize: 10,
    completed: false,
    watchedTime: 0,
    lastPosition: 0,
    order: 0,
    subtitles: [],
    ...overrides,
  }
}

function course(overrides) {
  return {
    id: "course",
    identityId: "course",
    name: "Course",
    path: "/library/course",
    fingerprint: "fp",
    missingSince: null,
    sections: [
      {
        id: "section",
        name: "Intro",
        order: 0,
        lessons: [
          lesson({ id: "video", type: "video", completed: true, watchedTime: 80, fileSize: 20 }),
          lesson({ id: "doc", type: "document", duration: 0, fileSize: 5 }),
        ],
      },
    ],
    progress: 50,
    totalDuration: 100,
    watchedDuration: 80,
    lastAccessed: null,
    thumbnail: null,
    thumbnailSourcePath: null,
    ...overrides,
  }
}

test("buildLearningStats summarizes library, media, courses, and activity", () => {
  const stats = buildLearningStats(
    [
      course({ id: "course-a", name: "Alpha" }),
      course({
        id: "course-b",
        name: "Missing",
        missingSince: "2026-07-06T00:00:00.000Z",
        sections: [
          {
            id: "section-b",
            name: "Only",
            order: 0,
            lessons: [lesson({ id: "audio", type: "audio", completed: false, watchedTime: 10, fileSize: 15 })],
          },
        ],
      }),
    ],
    [
      { date: "2026-07-05", watchedSeconds: 300, lessonsTouched: 2, completions: 1 },
      { date: "2026-07-06", watchedSeconds: 0, lessonsTouched: 0, completions: 0 },
    ]
  )

  assert.equal(stats.totalCourses, 2)
  assert.equal(stats.availableCourses, 1)
  assert.equal(stats.missingCourses, 1)
  assert.equal(stats.lessons, 3)
  assert.equal(stats.completedLessons, 1)
  assert.equal(stats.completionPercent, 33)
  assert.equal(stats.bytes, 40)
  assert.equal(stats.watchedSeconds, 90)
  assert.equal(stats.activeDays, 1)
  assert.deepEqual(
    stats.mediaTypes.map((item) => [item.type, item.lessons, item.bytes]),
    [["video", 1, 20], ["audio", 1, 15], ["document", 1, 5]]
  )
  assert.equal(stats.courses[0].name, "Alpha")
})
