import { create, type StoreApi } from "zustand"
import { persist } from "zustand/middleware"
import type { Course, Lesson, Section } from "@/types"

interface CourseState {
  courses: Course[]
  libraryPath: string | null
  scanMode: "idle" | "selecting" | "refreshing"
  hasHydrated: boolean
}

interface CourseActions {
  setCourses: (courses: Course[]) => void
  updateLessonProgress: (lessonId: string, watchedTime: number, lastPosition: number) => void
  markLessonComplete: (lessonId: string, completed: boolean) => void
  setLibraryPath: (path: string | null) => void
  setScanMode: (scanMode: CourseState["scanMode"]) => void
  setHasHydrated: (hasHydrated: boolean) => void
}

type CourseStore = CourseState & CourseActions

const initialState: CourseState = {
  courses: [],
  libraryPath: null,
  scanMode: "idle",
  hasHydrated: false,
}

type CourseStoreSet = StoreApi<CourseStore>["setState"]

const createCourseStore = (set: CourseStoreSet): CourseStore => ({
  ...initialState,

  setCourses: (courses: Course[]) => set({ courses }),

  updateLessonProgress: (lessonId: string, watchedTime: number, lastPosition: number) =>
    set((state: CourseStore) => {
      const updatedCourses = state.courses.map((course: Course) => ({
        ...course,
        sections: course.sections.map((section: Section) => ({
          ...section,
          lessons: section.lessons.map((lesson: Lesson) =>
            lesson.id === lessonId
              ? { ...lesson, watchedTime, lastPosition }
              : lesson
          ),
        })),
      }))
      return { courses: updatedCourses }
    }),

  markLessonComplete: (lessonId: string, completed: boolean) =>
    set((state: CourseStore) => {
      const updatedCourses = state.courses.map((course: Course) => ({
        ...course,
        sections: course.sections.map((section: Section) => ({
          ...section,
          lessons: section.lessons.map((lesson: Lesson) =>
            lesson.id === lessonId ? { ...lesson, completed } : lesson
          ),
        })),
      }))
      return { courses: updatedCourses }
    }),

  setLibraryPath: (libraryPath: string | null) => set({ libraryPath }),

  setScanMode: (scanMode) => set({ scanMode }),

  setHasHydrated: (hasHydrated) => set({ hasHydrated }),
})

export const useCourseStore = create<CourseStore>()(
  persist(createCourseStore, {
    name: "melearn-storage",
    partialize: (state: CourseStore) => ({
      courses: state.courses,
      libraryPath: state.libraryPath,
    }),
  })
)
