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

function findCourse(courses: Course[], courseId: string): Course | null {
  return courses.find((course) => course.id === courseId) ?? null
}

function findLesson(courses: Course[], lessonId: string): Lesson | null {
  for (const course of courses) {
    for (const section of course.sections) {
      const lesson = section.lessons.find((entry) => entry.id === lessonId)
      if (lesson) {
        return lesson
      }
    }
  }

  return null
}

export const appRouter = t.router({
  courses: t.router({
    list: t.procedure.query(() => getStore().courses),
    byId: t.procedure.input(courseIdSchema).query(({ input }) => {
      return findCourse(getStore().courses, input)
    }),
    markAccessed: t.procedure
      .input(z.object({ courseId: courseIdSchema }))
      .mutation(async ({ input }) => {
        const { courses, setCourses } = getStore()
        const existingCourse = findCourse(courses, input.courseId)

        if (!existingCourse) {
          throw new TRPCError({
            code: "NOT_FOUND",
            message: "Course not found",
          })
        }

        const timestamp = new Date().toISOString()
        const updated = courses.map((course: Course) =>
          course.id === input.courseId
            ? { ...course, lastAccessed: timestamp }
            : course
        )
        const current = findCourse(updated, input.courseId)

        if (isTauri()) {
          await updateCourseLastAccessed(input.courseId, timestamp)
        }

        setCourses(updated)
        return current
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
        const store = getStore()
        const lesson = findLesson(store.courses, input.lessonId)

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

        store.updateLessonProgress(input.lessonId, input.watchedTime, input.lastPosition)
        store.markLessonComplete(input.lessonId, input.completed)

        return true
      }),
  }),
  notes: t.router({
    list: t.procedure
      .input(z.object({ lessonId: lessonIdSchema }))
      .query(async ({ input }) => {
        const lesson = findLesson(getStore().courses, input.lessonId)
        if (!lesson) {
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
        const lesson = findLesson(getStore().courses, input.lessonId)

        if (!lesson) {
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
