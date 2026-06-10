import { create, type StoreApi } from "zustand"
import { persist } from "zustand/middleware"
import type { Course, Lesson, Section } from "@/types"

interface CourseState {
  courses: Course[]
  isScanning: boolean
  libraryPath: string | null
}

interface CourseActions {
  setCourses: (courses: Course[]) => void
  updateLessonProgress: (lessonId: string, watchedTime: number, lastPosition: number) => void
  markLessonComplete: (lessonId: string, completed: boolean) => void
  setIsScanning: (isScanning: boolean) => void
  setLibraryPath: (path: string | null) => void
}

type CourseStore = CourseState & CourseActions

const initialState: CourseState = {
  courses: [],
  isScanning: false,
  libraryPath: null,
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

  setIsScanning: (isScanning: boolean) => set({ isScanning }),

  setLibraryPath: (libraryPath: string | null) => set({ libraryPath }),
})

export const useCourseStore = create<CourseStore>()(
  persist(createCourseStore, {
    name: "melearn-storage",
    partialize: (state: CourseStore) => ({
      libraryPath: state.libraryPath,
    }),
  })
)
