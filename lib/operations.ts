import { nanoid } from "nanoid"
import type { Course, Note } from "@/types"
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

type CourseIndex = {
  courses: ReadonlyArray<Course>
  byId: Map<string, Course>
  lessonsById: Map<string, { lesson: Course["sections"][number]["lessons"][number]; courseId: string }>
}

const courseIndex: CourseIndex = {
  courses: [],
  byId: new Map(),
  lessonsById: new Map(),
}

function buildCourseIndex(courses: ReadonlyArray<Course>): CourseIndex {
  const byId = new Map<string, Course>()
  const lessonsById = new Map<
    string,
    { lesson: Course["sections"][number]["lessons"][number]; courseId: string }
  >()
  for (const course of courses) {
    byId.set(course.id, course)
    for (const section of course.sections) {
      for (const lesson of section.lessons) {
        lessonsById.set(lesson.id, { lesson, courseId: course.id })
      }
    }
  }
  return { courses, byId, lessonsById }
}

function getCourseIndex(): CourseIndex {
  const courses = useCourseStore.getState().courses
  if (courses !== courseIndex.courses) {
    const next = buildCourseIndex(courses)
    courseIndex.courses = courses
    courseIndex.byId = next.byId
    courseIndex.lessonsById = next.lessonsById
  }
  return courseIndex
}

function mapCourseLastAccessed(
  courses: ReadonlyArray<Course>,
  courseId: string,
  timestamp: string
): Course[] {
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

export class OperationError extends Error {
  constructor(public code: "not_found", message: string) {
    super(message)
    this.name = "OperationError"
  }
}

export async function scanLibraryAt(path: string): Promise<{ courses: Course[]; warnings: string[] }> {
  const { frontendLog } = await import("@/lib/frontend-log")
  frontendLog("info", `scanLibraryAt start: path=${path}`)
  const result = await scanFolder(path)
  frontendLog("info", `scanFolder returned: ${result.courses.length} courses, ${result.warnings.length} warnings: ${result.warnings.join(" | ")}`)
  const scanned = processScanResult(result)
  frontendLog("info", `processScanResult returned: ${scanned.length} courses`)
  const hydrated = isTauri() ? await syncLibrary(scanned, path) : scanned
  frontendLog("info", `syncLibrary returned: ${hydrated.length} courses`)
  const store = useCourseStore.getState()
  store.setCourses(hydrated)
  store.setLibraryPath(path)
  indexCourses(hydrated)
  return { courses: hydrated, warnings: result.warnings }
}

export async function markCourseAccessed(courseId: string): Promise<Course> {
  const existing = getCourseIndex().byId.get(courseId)
  if (!existing) throw new OperationError("not_found", "Course not found")

  const timestamp = new Date().toISOString()
  if (isTauri()) await updateCourseLastAccessed(courseId, timestamp)

  const { courses, setCourses } = useCourseStore.getState()
  setCourses(mapCourseLastAccessed(courses, courseId, timestamp))
  return { ...existing, lastAccessed: timestamp }
}

export interface LessonProgress {
  watchedTime: number
  lastPosition: number
  completed: boolean
}

export async function recordLessonProgress(lessonId: string, progress: LessonProgress): Promise<void> {
  const entry = getCourseIndex().lessonsById.get(lessonId)
  if (!entry) throw new OperationError("not_found", "Lesson not found")

  if (isTauri()) {
    await persistLessonProgress(lessonId, progress.watchedTime, progress.lastPosition, progress.completed)
  }

  const { updateLessonProgress, markLessonComplete } = useCourseStore.getState()
  updateLessonProgress(lessonId, progress.watchedTime, progress.lastPosition)
  markLessonComplete(lessonId, progress.completed)
}

export async function listNotes(lessonId: string): Promise<Note[]> {
  if (!getCourseIndex().lessonsById.has(lessonId)) return []
  return getNoteStore().list(lessonId)
}

export async function addNote(input: { lessonId: string; text: string; timestamp: number }): Promise<Note> {
  if (!getCourseIndex().lessonsById.has(input.lessonId)) {
    throw new OperationError("not_found", "Lesson not found")
  }

  const text = input.text.trim()
  if (text.length < 1 || text.length > 2000) {
    throw new Error("Note text must be between 1 and 2000 characters.")
  }

  const note: Note = {
    id: nanoid(12),
    lessonId: input.lessonId,
    timestamp: input.timestamp,
    text,
    createdAt: new Date().toISOString(),
  }

  await getNoteStore().save(note)
  return note
}

export async function removeNote(noteId: string): Promise<void> {
  await getNoteStore().remove(noteId)
}
