import { useEffect, useRef, useState } from "react"
import { createStore, type StoreApi } from "zustand/vanilla"
import type { Course, Lesson } from "@/types"

interface CourseState {
  courses: Course[]
  libraryPath: string | null
  scanMode: "idle" | "selecting" | "refreshing"
  hasHydrated: boolean
  startupRoute: { courseId: string; lessonId: string | null } | null
}

interface CourseActions {
  setCourses: (courses: Course[]) => void
  hydrateLibrary: (courses: Course[], libraryPath: string | null) => void
  updateLessonProgress: (lessonId: string, watchedTime: number, lastPosition: number) => void
  markLessonComplete: (lessonId: string, completed: boolean) => void
  setLibraryPath: (path: string | null) => void
  setScanMode: (scanMode: CourseState["scanMode"]) => void
  setHasHydrated: (hasHydrated: boolean) => void
  setStartupRoute: (startupRoute: CourseState["startupRoute"]) => void
}

type CourseStore = CourseState & CourseActions
type UseCourseStore = {
  <T>(selector: (state: CourseStore) => T): T
  getState: StoreApi<CourseStore>["getState"]
  setState: StoreApi<CourseStore>["setState"]
  subscribe: StoreApi<CourseStore>["subscribe"]
}

const initialState: CourseState = {
  courses: [],
  libraryPath: null,
  scanMode: "idle",
  hasHydrated: false,
  startupRoute: null,
}

type CourseStoreSet = StoreApi<CourseStore>["setState"]

type LessonPath = readonly [courseIndex: number, sectionIndex: number, lessonIndex: number]

let lessonIndex: ReadonlyArray<Course> = []
const lessonPathById = new Map<string, LessonPath>()

function rebuildLessonIndex(courses: ReadonlyArray<Course>) {
  lessonIndex = courses
  lessonPathById.clear()
  for (let ci = 0; ci < courses.length; ci++) {
    const sections = courses[ci].sections
    for (let si = 0; si < sections.length; si++) {
      const lessons = sections[si].lessons
      for (let li = 0; li < lessons.length; li++) {
        lessonPathById.set(lessons[li].id, [ci, si, li] as const)
      }
    }
  }
}

function applyLessonUpdate(
  courses: ReadonlyArray<Course>,
  path: LessonPath,
  patch: Partial<Lesson>,
): Course[] {
  const [ci, si, li] = path
  const course = courses[ci]
  const section = course.sections[si]
  const lesson = section.lessons[li]
  const updatedLesson: Lesson = { ...lesson, ...patch }
  const updatedSection = {
    ...section,
    lessons: section.lessons.map((l, i) => (i === li ? updatedLesson : l)),
  }
  const updatedCourse = {
    ...course,
    sections: course.sections.map((s, i) => (i === si ? updatedSection : s)),
  }
  const out = courses.slice()
  out[ci] = updatedCourse
  return out
}

const createCourseStore = (set: CourseStoreSet): CourseStore => ({
  ...initialState,

  setCourses: (courses: Course[]) => {
    rebuildLessonIndex(courses)
    set({ courses })
  },

  hydrateLibrary: (courses: Course[], libraryPath: string | null) => {
    rebuildLessonIndex(courses)
    set({ courses, libraryPath, hasHydrated: true })
  },

  updateLessonProgress: (lessonId: string, watchedTime: number, lastPosition: number) => {
    const state = useCourseStoreInternal.getState()
    if (state.courses !== lessonIndex) {
      rebuildLessonIndex(state.courses)
    }
    const path = lessonPathById.get(lessonId)
    if (!path) return
    const updated = applyLessonUpdate(state.courses, path, { watchedTime, lastPosition })
    set({ courses: updated })
  },

  markLessonComplete: (lessonId: string, completed: boolean) => {
    const state = useCourseStoreInternal.getState()
    if (state.courses !== lessonIndex) {
      rebuildLessonIndex(state.courses)
    }
    const path = lessonPathById.get(lessonId)
    if (!path) return
    const updated = applyLessonUpdate(state.courses, path, { completed })
    set({ courses: updated })
  },

  setLibraryPath: (libraryPath: string | null) => set({ libraryPath }),

  setScanMode: (scanMode) => set({ scanMode }),

  setHasHydrated: (hasHydrated) => set({ hasHydrated }),

  setStartupRoute: (startupRoute) => set({ startupRoute }),
})

const useCourseStoreInternal = createStore<CourseStore>()(createCourseStore)

export const useCourseStore = (<T>(selector: (state: CourseStore) => T): T => {
  const selectorRef = useRef(selector)
  selectorRef.current = selector

  const [selected, setSelected] = useState(() => selector(useCourseStoreInternal.getState()))
  const selectedRef = useRef(selected)
  selectedRef.current = selected

  useEffect(() => {
    const update = (state: CourseStore) => {
      const next = selectorRef.current(state)
      if (Object.is(selectedRef.current, next)) return
      selectedRef.current = next
      setSelected(next)
    }
    const unsubscribe = useCourseStoreInternal.subscribe(update)
    update(useCourseStoreInternal.getState())
    return unsubscribe
  }, [])

  return selected
}) as UseCourseStore

useCourseStore.getState = useCourseStoreInternal.getState
useCourseStore.setState = useCourseStoreInternal.setState
useCourseStore.subscribe = useCourseStoreInternal.subscribe
