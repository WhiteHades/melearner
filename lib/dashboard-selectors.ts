import type { SearchResult } from "@/lib/search"
import type { Course, Lesson } from "@/types"

export type CourseSummary = {
  totalLessons: number
  completedLessons: number
  progress: number
}

export type DashboardCourseCard = {
  course: Course
  summary: CourseSummary
  nextLesson: Lesson | null
}

export function buildDashboardCourseCards(courses: Course[]): DashboardCourseCard[] {
  return courses.map((course) => {
    let totalLessons = 0
    let completedLessons = 0
    let nextLesson: Lesson | null = null
    let firstLesson: Lesson | null = null

    for (const section of course.sections) {
      for (const lesson of section.lessons) {
        totalLessons += 1
        if (!firstLesson) firstLesson = lesson
        if (lesson.completed) {
          completedLessons += 1
        } else if (!nextLesson) {
          nextLesson = lesson
        }
      }
    }

    return {
      course,
      summary: {
        totalLessons,
        completedLessons,
        progress: totalLessons > 0 ? Math.round((completedLessons / totalLessons) * 100) : 0,
      },
      nextLesson: nextLesson ?? firstLesson,
    }
  })
}

export function selectResumeCourseCards(cards: DashboardCourseCard[]): DashboardCourseCard[] {
  const sorted = cards
    .filter(({ course }) => !course.missingSince)
    .slice()
    .sort((a, b) => {
      const accessOrder = (b.course.lastAccessed ?? "").localeCompare(a.course.lastAccessed ?? "")
      if (accessOrder !== 0) return accessOrder
      return b.summary.progress - a.summary.progress
    })

  const activeOrRecent = sorted.filter(({ course, summary }) => course.lastAccessed || summary.progress > 0)
  const incomplete = sorted.filter(({ summary }) => summary.progress < 100)
  return (activeOrRecent.length > 0 ? activeOrRecent : incomplete).slice(0, 3)
}

export function selectVisibleCourseCards(
  cards: DashboardCourseCard[],
  query: string,
  results: LibrarySearchResultLike[]
): DashboardCourseCard[] {
  if (!query.trim()) return cards

  const byId = new Map(cards.map((card) => [card.course.id, card]))
  const seen = new Set<string>()
  const matched: DashboardCourseCard[] = []

  for (const result of results) {
    if (seen.has(result.courseId)) continue
    const card = byId.get(result.courseId)
    if (!card) continue
    seen.add(result.courseId)
    matched.push(card)
  }

  return matched
}

type LibrarySearchResultLike = Pick<SearchResult, "courseId" | "id">

export function selectCommandLessons(
  courses: Course[],
  query: string,
  results: LibrarySearchResultLike[],
  limit: number
): Array<{ course: Course; lesson: Lesson }> {
  const cappedLimit = Math.max(0, limit)
  if (cappedLimit === 0) return []

  if (!query.trim()) {
    const lessons: Array<{ course: Course; lesson: Lesson }> = []
    for (const course of courses) {
      if (course.missingSince) continue
      for (const section of course.sections) {
        for (const lesson of section.lessons) {
          lessons.push({ course, lesson })
          if (lessons.length >= cappedLimit) return lessons
        }
      }
    }
    return lessons
  }

  const wantedLessonIds = new Set(results.slice(0, cappedLimit).map((result) => result.id))
  if (wantedLessonIds.size === 0) return []

  const matches = new Map<string, { course: Course; lesson: Lesson }>()
  for (const course of courses) {
    if (course.missingSince) continue
    for (const section of course.sections) {
      for (const lesson of section.lessons) {
        if (wantedLessonIds.has(lesson.id)) {
          matches.set(lesson.id, { course, lesson })
          if (matches.size === wantedLessonIds.size) break
        }
      }
      if (matches.size === wantedLessonIds.size) break
    }
    if (matches.size === wantedLessonIds.size) break
  }

  return results.flatMap((result) => matches.get(result.id) ?? []).slice(0, cappedLimit)
}
