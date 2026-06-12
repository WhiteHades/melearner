"use client"

import { useEffect, useMemo } from "react"
import { NotesPanel } from "./notes-panel"
import { CourseViewerSidebar } from "./sidebar"
import { VideoArea } from "./video-area"
import type { Course, Lesson } from "@/types"
import { SidebarInset, SidebarProvider } from "@/components/ui/sidebar"

interface CourseViewerLayoutProps {
  course: Course | null
  onBack: () => void
  selectedLessonId?: string | null
  onLessonChange?: (lessonId: string | null) => void
}

export function CourseViewerLayout({
  course,
  onBack,
  selectedLessonId,
  onLessonChange,
}: CourseViewerLayoutProps) {
  const orderedLessons = useMemo(() => {
    if (!course) return []
    return course.sections.flatMap((section) => section.lessons)
  }, [course])

  const currentLesson = useMemo(() => {
    if (!course) return null

    if (selectedLessonId) {
      const selectedLesson = orderedLessons.find((lesson) => lesson.id === selectedLessonId)
      if (selectedLesson) return selectedLesson
    }

    return orderedLessons[0] ?? null
  }, [course, orderedLessons, selectedLessonId])

  const currentLessonIndex = useMemo(() => {
    if (!currentLesson) return -1
    return orderedLessons.findIndex((lesson) => lesson.id === currentLesson.id)
  }, [currentLesson, orderedLessons])

  const previousLesson = currentLessonIndex > 0 ? orderedLessons[currentLessonIndex - 1] : null
  const nextLesson =
    currentLessonIndex >= 0 && currentLessonIndex < orderedLessons.length - 1
      ? orderedLessons[currentLessonIndex + 1]
      : null

  useEffect(() => {
    const currentLessonId = currentLesson?.id ?? null

    if (currentLessonId !== selectedLessonId) {
      onLessonChange?.(currentLessonId)
    }
  }, [currentLesson, onLessonChange, selectedLessonId])

  const handleLessonSelect = (lesson: Lesson) => {
    onLessonChange?.(lesson.id)
  }

  return (
    <SidebarProvider
      className="h-full min-h-0"
      style={
        {
          "--sidebar-width": "clamp(9rem, 18vw, 14rem)",
          "--sidebar-width-icon": "3rem",
        } as React.CSSProperties
      }
    >
      <CourseViewerSidebar
        course={course}
        currentLessonId={currentLesson?.id}
        onSelectLesson={handleLessonSelect}
        onBack={onBack}
      />
      <SidebarInset className="min-h-0 overflow-hidden">
        <div className="mx-auto flex h-full min-h-0 w-full max-w-5xl flex-col gap-6 overflow-y-auto px-4 py-6 sm:px-6 lg:px-8">
          <VideoArea
            key={currentLesson?.id ?? "empty-lesson"}
            lesson={currentLesson}
            onNext={nextLesson ? () => onLessonChange?.(nextLesson.id) : undefined}
            onPrevious={previousLesson ? () => onLessonChange?.(previousLesson.id) : undefined}
          />
          <NotesPanel lesson={currentLesson} />
        </div>
      </SidebarInset>
    </SidebarProvider>
  )
}
