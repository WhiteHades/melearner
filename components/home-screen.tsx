"use client"

import { useCallback, useEffect, useMemo } from "react"
import { PanelLeft } from "lucide-react"
import { parseAsString, useQueryState } from "nuqs"
import { Button } from "@/components/ui/button"
import { CourseViewerLayout } from "@/components/course-viewer/layout"
import { CourseGrid } from "@/components/course-grid"
import { trpc } from "@/lib/trpc/client"
import type { Course } from "@/types"
import { SidebarInset, SidebarProvider, useSidebar } from "@/components/ui/sidebar"
import { AppSidebar } from "@/components/app-sidebar"
import { Separator } from "@/components/ui/separator"

type View = "library" | "viewer"

function LibraryHeader() {
  const { open, setOpen } = useSidebar()

  return (
    <header
      data-tauri-drag-region
      className="flex h-12 shrink-0 items-center gap-2 border-b transition-[width,height] ease-linear"
    >
      <div className="flex items-center gap-2 px-4">
        <Button
          type="button"
          variant="ghost"
          size="icon"
          onClick={(e) => {
            e.stopPropagation()
            setOpen(!open)
          }}
          onPointerDown={(e) => e.stopPropagation()}
          className="-ml-1 h-7 w-7"
        >
          <PanelLeft className="size-4" />
        </Button>
        <Separator orientation="vertical" className="mr-2 data-[orientation=vertical]:h-4" />
        <span className="text-sm font-medium">Library</span>
      </div>
    </header>
  )
}

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
    <SidebarProvider>
      <AppSidebar variant="inset" />
      <SidebarInset>
        <LibraryHeader />
        <div className="flex flex-1 flex-col">
          <div className="flex flex-1 flex-col gap-2 p-4 md:gap-6 md:p-6">
            <div className="flex flex-col gap-4">
              <CourseGrid onCourseSelect={handleCourseSelect} />
            </div>
          </div>
        </div>
      </SidebarInset>
    </SidebarProvider>
  )
}