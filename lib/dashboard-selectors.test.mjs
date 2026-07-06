import test from "node:test"
import assert from "node:assert/strict"
import {
  buildDashboardCourseCards,
  selectCommandLessons,
  selectResumeCourseCards,
  selectVisibleCourseCards,
} from "./dashboard-selectors.ts"

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
    markerIdentityId: null,
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
          lesson({ id: `${overrides.id}-done`, completed: true }),
          lesson({ id: `${overrides.id}-next`, completed: false }),
        ],
      },
    ],
    progress: 0,
    totalDuration: 0,
    watchedDuration: 0,
    lastAccessed: null,
    thumbnail: null,
    thumbnailSourcePath: null,
    ...overrides,
  }
}

test("dashboard course cards carry summaries and next lessons", () => {
  const cards = buildDashboardCourseCards([
    course({ id: "alpha", name: "Alpha" }),
    course({
      id: "complete",
      name: "Complete",
      sections: [
        {
          id: "section-complete",
          name: "Intro",
          order: 0,
          lessons: [lesson({ id: "complete-one", completed: true })],
        },
      ],
    }),
  ])

  assert.deepEqual(
    cards.map((card) => [card.course.id, card.summary.totalLessons, card.summary.completedLessons, card.summary.progress, card.nextLesson?.id]),
    [
      ["alpha", 2, 1, 50, "alpha-next"],
      ["complete", 1, 1, 100, "complete-one"],
    ]
  )
})

test("resume cards prefer recent or in-progress available courses", () => {
  const cards = buildDashboardCourseCards([
    course({ id: "missing", name: "Missing", missingSince: "2026-07-06T00:00:00.000Z" }),
    course({ id: "recent", name: "Recent", lastAccessed: "2026-07-06T10:00:00.000Z" }),
    course({ id: "older", name: "Older", lastAccessed: "2026-07-05T10:00:00.000Z" }),
  ])

  assert.deepEqual(
    selectResumeCourseCards(cards).map((card) => card.course.id),
    ["recent", "older"]
  )
})

test("visible cards follow search result course order and dedupe courses", () => {
  const cards = buildDashboardCourseCards([
    course({ id: "alpha", name: "Alpha" }),
    course({ id: "beta", name: "Beta" }),
  ])

  const visible = selectVisibleCourseCards(cards, "intro", [
    { id: "beta-next", courseId: "beta" },
    { id: "beta-done", courseId: "beta" },
    { id: "alpha-next", courseId: "alpha" },
  ])

  assert.deepEqual(visible.map((card) => card.course.id), ["beta", "alpha"])
})

test("command lessons are capped before rendering", () => {
  const courses = [
    course({ id: "alpha", name: "Alpha" }),
    course({ id: "beta", name: "Beta" }),
  ]

  assert.deepEqual(
    selectCommandLessons(courses, "", [], 3).map(({ lesson }) => lesson.id),
    ["alpha-done", "alpha-next", "beta-done"]
  )
  assert.deepEqual(
    selectCommandLessons(courses, "next", [
      { id: "beta-next", courseId: "beta" },
      { id: "alpha-next", courseId: "alpha" },
    ], 1).map(({ lesson }) => lesson.id),
    ["beta-next"]
  )
})
