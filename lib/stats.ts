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

export function buildLearningStats(courses: Course[], activityDays: ActivityDay[] = []): LearningStats {
  let availableCourses = 0
  let missingCourses = 0
  let sections = 0
  let lessons = 0
  let completedLessons = 0
  let bytes = 0
  let watchedSeconds = 0
  let totalSeconds = 0
  const mediaTypes = new Map<Lesson["type"], MediaTypeStats>(
    MEDIA_TYPES.map((type) => [type, { type, lessons: 0, bytes: 0, completed: 0, watchedSeconds: 0 }])
  )

  const courseStats = courses.map((course) => {
    if (course.missingSince) {
      missingCourses += 1
    } else {
      availableCourses += 1
    }

    let courseLessons = 0
    let courseCompletedLessons = 0
    let courseBytes = 0
    let courseWatchedSeconds = 0
    let courseTotalSeconds = 0
    sections += course.sections.length

    for (const section of course.sections) {
      for (const lesson of section.lessons) {
        courseLessons += 1
        lessons += 1
        courseBytes += lesson.fileSize
        bytes += lesson.fileSize
        courseWatchedSeconds += lesson.watchedTime
        watchedSeconds += lesson.watchedTime
        courseTotalSeconds += lesson.duration
        totalSeconds += lesson.duration

        if (lesson.completed) {
          courseCompletedLessons += 1
          completedLessons += 1
        }

        const mediaType = mediaTypes.get(lesson.type)
        if (mediaType) {
          mediaType.lessons += 1
          mediaType.bytes += lesson.fileSize
          mediaType.watchedSeconds += lesson.watchedTime
          if (lesson.completed) mediaType.completed += 1
        }
      }
    }

    return {
      id: course.id,
      name: course.name,
      missingSince: course.missingSince,
      lessons: courseLessons,
      completedLessons: courseCompletedLessons,
      bytes: courseBytes,
      watchedSeconds: courseWatchedSeconds,
      totalSeconds: courseTotalSeconds,
      lastAccessed: course.lastAccessed,
    }
  })

  return {
    totalCourses: courses.length,
    availableCourses,
    missingCourses,
    sections,
    lessons,
    completedLessons,
    completionPercent: lessons > 0 ? Math.round((completedLessons / lessons) * 100) : 0,
    bytes,
    watchedSeconds,
    totalSeconds,
    mediaTypes: MEDIA_TYPES.map((type) => mediaTypes.get(type)!).filter((stats) => stats.lessons > 0),
    courses: courseStats.sort((a, b) =>
      b.watchedSeconds - a.watchedSeconds ||
      b.bytes - a.bytes ||
      a.name.localeCompare(b.name, undefined, { numeric: true })
    ),
    activityDays,
    activeDays: activityDays.filter((day) => day.watchedSeconds > 0 || day.completions > 0).length,
  }
}
