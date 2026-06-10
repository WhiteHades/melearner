"use client"

import { useCallback, useEffect, useMemo } from "react"
import { parseAsString, useQueryState } from "nuqs"
import { CourseViewerLayout } from "@/components/course-viewer/layout"
import { CourseGrid } from "@/components/course-grid"
import { trpc } from "@/lib/trpc/client"
import type { Course } from "@/types"
import { SidebarProvider } from "@/components/ui/sidebar"

type View = "library" | "viewer"

export function HomeScreen() {
  const [viewParam, setViewParam] = useQueryState("view", parseAsString.withDefault("library"))
  const [courseId, setCourseId] = useQueryState("course", parseAsString)
  const [lessonId, setLessonId] = useQueryState("lesson", parseAsString)

  const view = viewParam === "viewer" ? ("viewer" satisfies View) : "library"
  const { data: courses = [] } = trpc.courses.list.useQuery()
  const markAccessed = trpc.courses.markAccessed.useMutation()

  const selectedCourse = useMemo(() => {
    return courses.find((course: Course) => course.id === courseId) ?? null
  }, [courses, courseId])

  useEffect(() => {
    if (view === "viewer" && !selectedCourse) {
      setViewParam("library")
      setCourseId(null)
      setLessonId(null)
    }
  }, [view, selectedCourse, setViewParam, setCourseId, setLessonId])

  const handleCourseSelect = useCallback(
    (course: Course) => {
      setCourseId(course.id)
      setLessonId(null)
      setViewParam("viewer")
      markAccessed.mutate({ courseId: course.id })
    },
    [setCourseId, setLessonId, setViewParam, markAccessed]
  )

  const handleBack = useCallback(() => {
    setViewParam("library")
    setCourseId(null)
    setLessonId(null)
  }, [setViewParam, setCourseId, setLessonId])

  if (view === "viewer") {
    return (
      <SidebarProvider>
        <CourseViewerLayout
          course={selectedCourse}
          onBack={handleBack}
          selectedLessonId={lessonId}
          onLessonChange={setLessonId}
        />
      </SidebarProvider>
    )
  }

  return (
    <div className="flex flex-1 flex-col gap-2 p-4 md:gap-6 md:p-6">
      <CourseGrid onCourseSelect={handleCourseSelect} />
    </div>
  )
}
