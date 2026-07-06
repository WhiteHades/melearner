"use client"

import { useEffect, useMemo } from "react"
import { FileText, FolderOpen } from "lucide-react"
import { CourseViewerSidebar } from "./sidebar"
import { VideoArea } from "./video-area"
import { Button } from "@/components/ui/button"
import { Progress } from "@/components/ui/progress"
import { ScrollArea } from "@/components/ui/scroll-area"
import { Separator } from "@/components/ui/separator"
import { Badge } from "@/components/ui/badge"
import { cleanSectionName, formatDuration } from "@/lib/utils"
import type { Course, Lesson } from "@/types"

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

    return orderedLessons.find((lesson) => !lesson.completed) ?? orderedLessons[0] ?? null
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

  const progress = useMemo(() => {
    const completed = orderedLessons.filter((lesson) => lesson.completed).length
    return {
      completed,
      total: orderedLessons.length,
      percent: orderedLessons.length > 0 ? Math.round((completed / orderedLessons.length) * 100) : 0,
    }
  }, [orderedLessons])

  useEffect(() => {
    const currentLessonId = currentLesson?.id ?? null

    if (currentLessonId !== selectedLessonId) {
      onLessonChange?.(currentLessonId)
    }
  }, [currentLesson, onLessonChange, selectedLessonId])

  const handleLessonSelect = (lesson: Lesson) => {
    onLessonChange?.(lesson.id)
  }

  if (!course) {
    return (
      <div className="flex h-full items-center justify-center bg-background text-foreground">
        <div className="flex flex-col items-center gap-4 text-center">
          <FolderOpen className="size-10 text-muted-foreground" />
          <div className="flex flex-col gap-1">
            <h1 className="text-xl font-semibold">Course not found</h1>
            <p className="text-sm text-muted-foreground">Return to your library and open a course again.</p>
          </div>
          <Button type="button" variant="outline" onClick={onBack}>Back to library</Button>
        </div>
      </div>
    )
  }

  return (
    <div className="relative flex h-full min-h-0 flex-col bg-background text-foreground">
      <div data-tauri-drag-region className="absolute inset-x-0 top-0 z-40 h-6 md:right-32" />

      <main className="grid min-h-0 flex-1 gap-0 overflow-hidden pt-6 md:grid-cols-[21rem_minmax(0,1fr)] 2xl:grid-cols-[21rem_minmax(0,1fr)_24rem]">
        <CourseViewerSidebar
          course={course}
          currentLessonId={currentLesson?.id}
          onSelectLesson={handleLessonSelect}
          onBack={onBack}
        />

        <ScrollArea className="min-h-0 min-w-0 border-x border-border bg-background">
          <div className="mx-auto flex min-h-full w-full min-w-0 max-w-6xl flex-col gap-5 px-4 py-5 lg:px-6">
            <VideoArea
              key={`${currentLesson?.id ?? "empty-lesson"}:${currentLesson?.path ?? ""}`}
              lesson={currentLesson}
              libraryRoot={course.path}
              onNext={nextLesson ? () => onLessonChange?.(nextLesson.id) : undefined}
              onPrevious={previousLesson ? () => onLessonChange?.(previousLesson.id) : undefined}
            />
          </div>
        </ScrollArea>

        <LessonUtilityPanel course={course} lesson={currentLesson} progress={progress} />
      </main>
    </div>
  )
}

function LessonUtilityPanel({
  course,
  lesson,
  progress,
}: {
  course: Course
  lesson: Lesson | null
  progress: { completed: number; total: number; percent: number }
}) {
  const currentSection = course.sections.find((section) => section.lessons.some((item) => item.id === lesson?.id))
  const resourceLessons = currentSection?.lessons.filter((item) => item.type === "document" || item.type === "quiz") ?? []

  return (
    <aside className="hidden min-h-0 min-w-0 border-l border-border bg-card 2xl:flex 2xl:flex-col">
      <ScrollArea className="min-h-0 flex-1">
        <div className="flex flex-col gap-6 p-5">
          <div className="flex items-start justify-between gap-3">
            <div className="min-w-0 flex flex-col gap-1">
              <h2 className="text-base font-semibold">Learning progress</h2>
              <p className="text-sm text-muted-foreground">{progress.completed} of {progress.total} items complete</p>
            </div>
            <Badge variant="outline" className="rounded-md">{progress.percent}%</Badge>
          </div>
          <Progress value={progress.percent} className="h-2" />

          <Separator />

          <div className="flex flex-col gap-3">
            <h3 className="text-sm font-semibold">Current item</h3>
            {lesson ? (
              <div className="min-w-0 rounded-xl border border-border bg-background p-4 shadow-[var(--shadow-whisper)]">
                <div className="flex flex-col gap-2">
                  <Badge variant="secondary" className="w-fit rounded-md">{lesson.type}</Badge>
                  <h4 className="break-words text-sm font-semibold leading-snug">{lesson.name}</h4>
                  <p className="break-words text-xs text-muted-foreground">{cleanSectionName(lesson.sectionName) || "Section"}</p>
                  <p className="text-xs text-muted-foreground">Last position {formatDuration(lesson.lastPosition)}</p>
                  {lesson.subtitles.length > 0 && (
                    <p className="text-xs text-primary">{lesson.subtitles.length} subtitle track{lesson.subtitles.length === 1 ? "" : "s"} available</p>
                  )}
                </div>
              </div>
            ) : (
              <p className="text-sm text-muted-foreground">Select a lesson from the outline.</p>
            )}
          </div>

          <Separator />

          <div className="flex flex-col gap-3">
            <h3 className="break-words text-sm font-semibold leading-snug">Supporting learning items in this section</h3>
            {resourceLessons.length > 0 ? (
              <div className="flex flex-col gap-2">
                {resourceLessons.map((item) => (
                  <div key={item.id} className="flex min-w-0 items-start gap-3 rounded-lg border border-border bg-background p-3 text-sm">
                    <FileText className="size-4 shrink-0 text-muted-foreground" />
                    <span className="min-w-0 flex-1 break-words leading-snug">{item.name}</span>
                  </div>
                ))}
              </div>
            ) : (
              <p className="text-sm text-muted-foreground">No supporting learning items detected in this section.</p>
            )}
          </div>
        </div>
      </ScrollArea>

    </aside>
  )
}
