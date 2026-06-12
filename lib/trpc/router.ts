import { TRPCError, initTRPC } from "@trpc/server"
import { z } from "zod"
import { nanoid } from "nanoid"
import type { Course, Lesson } from "@/types"
import { useCourseStore } from "@/lib/stores/course-store"
import { processScanResult } from "@/lib/course-utils"
import { indexCourses } from "@/lib/search"
import { scanFolder, isTauri } from "@/lib/tauri"
import {
  syncLibrary,
  updateCourseLastAccessed,
  updateLessonProgress as persistLessonProgress,
} from "@/lib/database"
import { getNoteStore } from "@/lib/notes-store"

const t = initTRPC.create({
  allowOutsideOfServer: true,
})

const getStore = () => useCourseStore.getState()

const courseIdSchema = z.string().min(1)
const lessonIdSchema = z.string().min(1)

type CourseIndex = {
  courses: ReadonlyArray<Course>
  byId: Map<string, Course>
  lessonsById: Map<string, Lesson>
}

const courseIndex: CourseIndex = {
  courses: [],
  byId: new Map(),
  lessonsById: new Map(),
}

function buildCourseIndex(courses: ReadonlyArray<Course>): CourseIndex {
  const byId = new Map<string, Course>()
  const lessonsById = new Map<string, Lesson>()
  for (const course of courses) {
    byId.set(course.id, course)
    for (const section of course.sections) {
      for (const lesson of section.lessons) {
        lessonsById.set(lesson.id, lesson)
      }
    }
  }
  return { courses, byId, lessonsById }
}

function getCourseIndex(): CourseIndex {
  const courses = getStore().courses
  if (courses !== courseIndex.courses) {
    const next = buildCourseIndex(courses)
    courseIndex.courses = courses
    courseIndex.byId = next.byId
    courseIndex.lessonsById = next.lessonsById
  }
  return courseIndex
}

function findCourseById(courseId: string): Course | null {
  return getCourseIndex().byId.get(courseId) ?? null
}

function findLessonById(lessonId: string): Lesson | null {
  return getCourseIndex().lessonsById.get(lessonId) ?? null
}

function mapCourseLastAccessed(courses: ReadonlyArray<Course>, courseId: string, timestamp: string): Course[] {
  let changed = false
  const out = new Array<Course>(courses.length)
  for (let i = 0; i < courses.length; i++) {
    const c = courses[i]
    if (c.id === courseId && c.lastAccessed !== timestamp) {
      out[i] = { ...c, lastAccessed: timestamp }
      changed = true
    } else {
      out[i] = c
    }
  }
  return changed ? out : (courses as Course[])
}

export const appRouter = t.router({
  courses: t.router({
    list: t.procedure.query(() => getStore().courses),
    byId: t.procedure.input(courseIdSchema).query(({ input }) => {
      return findCourseById(input)
    }),
    markAccessed: t.procedure
      .input(z.object({ courseId: courseIdSchema }))
      .mutation(async ({ input }) => {
        const idx = getCourseIndex()
        const existingCourse = idx.byId.get(input.courseId)

        if (!existingCourse) {
          throw new TRPCError({
            code: "NOT_FOUND",
            message: "Course not found",
          })
        }

        const timestamp = new Date().toISOString()
        const { courses, setCourses } = getStore()
        const updated = mapCourseLastAccessed(courses, input.courseId, timestamp)

        if (isTauri()) {
          await updateCourseLastAccessed(input.courseId, timestamp)
        }

        setCourses(updated)
        return { ...existingCourse, lastAccessed: timestamp }
      }),
  }),
  library: t.router({
    scan: t.procedure
      .input(z.object({ path: z.string().min(1) }))
      .mutation(async ({ input }) => {
        const result = await scanFolder(input.path)
        const courses = processScanResult(result)
        const hydratedCourses = isTauri() ? await syncLibrary(courses) : courses
        const store = getStore()
        store.setCourses(hydratedCourses)
        store.setLibraryPath(input.path)
        indexCourses(hydratedCourses)
        return { courses: hydratedCourses, warnings: result.warnings }
      }),
  }),
  lessons: t.router({
    updateProgress: t.procedure
      .input(
        z.object({
          lessonId: lessonIdSchema,
          watchedTime: z.number().min(0),
          lastPosition: z.number().min(0),
          completed: z.boolean(),
        })
      )
      .mutation(async ({ input }) => {
        const lesson = findLessonById(input.lessonId)

        if (!lesson) {
          throw new TRPCError({
            code: "NOT_FOUND",
            message: "Lesson not found",
          })
        }

        if (isTauri()) {
          await persistLessonProgress(
            input.lessonId,
            input.watchedTime,
            input.lastPosition,
            input.completed
          )
        }

        const { updateLessonProgress, markLessonComplete } = getStore()
        updateLessonProgress(input.lessonId, input.watchedTime, input.lastPosition)
        markLessonComplete(input.lessonId, input.completed)

        return true
      }),
  }),
  notes: t.router({
    list: t.procedure
      .input(z.object({ lessonId: lessonIdSchema }))
      .query(async ({ input }) => {
        if (!findLessonById(input.lessonId)) {
          return []
        }

        return getNoteStore().list(input.lessonId)
      }),
    add: t.procedure
      .input(
        z.object({
          lessonId: lessonIdSchema,
          text: z.string().trim().min(1).max(2000),
          timestamp: z.number().min(0),
        })
      )
      .mutation(async ({ input }) => {
        if (!findLessonById(input.lessonId)) {
          throw new TRPCError({
            code: "NOT_FOUND",
            message: "Lesson not found",
          })
        }

        const note = {
          id: nanoid(12),
          lessonId: input.lessonId,
          timestamp: input.timestamp,
          text: input.text,
          createdAt: new Date().toISOString(),
        }

        await getNoteStore().save(note)

        return note
      }),
    remove: t.procedure
      .input(z.object({ noteId: z.string().min(1) }))
      .mutation(async ({ input }) => {
        await getNoteStore().remove(input.noteId)
        return true
      }),
  }),
})

export type AppRouter = typeof appRouter
