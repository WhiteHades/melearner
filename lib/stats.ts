import type { ActivityDay, Course, Lesson } from "@/types"

export type MediaTypeStats = {
  type: Lesson["type"]
  lessons: number
  bytes: number
  completed: number
  watchedSeconds: number
}

export type CourseStats = {
  id: string
  name: string
  missingSince: string | null
  lessons: number
  completedLessons: number
  bytes: number
  watchedSeconds: number
  totalSeconds: number
  lastAccessed: string | null
}

export type LearningStats = {
  totalCourses: number
  availableCourses: number
  missingCourses: number
  sections: number
  lessons: number
  completedLessons: number
  completionPercent: number
  bytes: number
  watchedSeconds: number
  totalSeconds: number
  mediaTypes: MediaTypeStats[]
  courses: CourseStats[]
  activityDays: ActivityDay[]
  activeDays: number
}

const MEDIA_TYPES: Array<Lesson["type"]> = ["video", "audio", "document", "quiz"]

function sumLessons(courses: Course[]): Lesson[] {
  return courses.flatMap((course) => course.sections.flatMap((section) => section.lessons))
}

function sumBy<T>(items: T[], valueFor: (item: T) => number): number {
  return items.reduce((total, item) => total + valueFor(item), 0)
}

export function buildLearningStats(courses: Course[], activityDays: ActivityDay[] = []): LearningStats {
  const lessons = sumLessons(courses)
  const completedLessons = lessons.filter((lesson) => lesson.completed).length
  const courseStats = courses.map((course) => {
    const courseLessons = course.sections.flatMap((section) => section.lessons)
    return {
      id: course.id,
      name: course.name,
      missingSince: course.missingSince,
      lessons: courseLessons.length,
      completedLessons: courseLessons.filter((lesson) => lesson.completed).length,
      bytes: sumBy(courseLessons, (lesson) => lesson.fileSize),
      watchedSeconds: sumBy(courseLessons, (lesson) => lesson.watchedTime),
      totalSeconds: sumBy(courseLessons, (lesson) => lesson.duration),
      lastAccessed: course.lastAccessed,
    }
  })

  return {
    totalCourses: courses.length,
    availableCourses: courses.filter((course) => !course.missingSince).length,
    missingCourses: courses.filter((course) => course.missingSince).length,
    sections: sumBy(courses, (course) => course.sections.length),
    lessons: lessons.length,
    completedLessons,
    completionPercent: lessons.length > 0 ? Math.round((completedLessons / lessons.length) * 100) : 0,
    bytes: sumBy(lessons, (lesson) => lesson.fileSize),
    watchedSeconds: sumBy(lessons, (lesson) => lesson.watchedTime),
    totalSeconds: sumBy(lessons, (lesson) => lesson.duration),
    mediaTypes: MEDIA_TYPES.map((type) => {
      const typedLessons = lessons.filter((lesson) => lesson.type === type)
      return {
        type,
        lessons: typedLessons.length,
        bytes: sumBy(typedLessons, (lesson) => lesson.fileSize),
        completed: typedLessons.filter((lesson) => lesson.completed).length,
        watchedSeconds: sumBy(typedLessons, (lesson) => lesson.watchedTime),
      }
    }).filter((stats) => stats.lessons > 0),
    courses: courseStats.sort((a, b) =>
      b.watchedSeconds - a.watchedSeconds ||
      b.bytes - a.bytes ||
      a.name.localeCompare(b.name, undefined, { numeric: true })
    ),
    activityDays,
    activeDays: activityDays.filter((day) => day.watchedSeconds > 0 || day.completions > 0).length,
  }
}
