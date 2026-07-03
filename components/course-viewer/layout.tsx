"use client"

import { useEffect, useMemo } from "react"
import { ChevronLeft, ChevronRight, FileText, FolderOpen } from "lucide-react"
import { CourseViewerSidebar } from "./sidebar"
import { VideoArea } from "./video-area"
import { Badge } from "@/components/ui/badge"
import { Button } from "@/components/ui/button"
import { Progress } from "@/components/ui/progress"
import { ScrollArea } from "@/components/ui/scroll-area"
import { Separator } from "@/components/ui/separator"
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
    <div className="flex h-full min-h-0 flex-col bg-background text-foreground">
      <header className="relative flex h-16 shrink-0 items-center gap-4 border-b border-border bg-card px-4">
        <div data-tauri-drag-region className="absolute inset-x-0 top-0 h-3" />
        <div className="flex items-center gap-3">
          <div className="text-2xl font-bold tracking-tight">melearner</div>
          <Badge variant="secondary" className="hidden rounded-md sm:inline-flex">local course</Badge>
        </div>

        <div className="mx-auto hidden min-w-0 max-w-md flex-1 items-center gap-3 rounded-lg border border-border bg-background px-4 py-2 lg:flex">
          <span className="shrink-0 text-sm tabular-nums">{progress.completed}/{progress.total} learning items</span>
          <Progress value={progress.percent} className="h-2" />
        </div>

        <div className="ml-auto flex items-center gap-2">
          <Button type="button" variant="ghost" size="sm" onClick={onBack} className="gap-2 rounded-md">
            <ChevronLeft className="size-4" />
            Library
          </Button>
        </div>
      </header>

      <main className="grid min-h-0 flex-1 gap-0 overflow-hidden xl:grid-cols-[360px_minmax(0,1fr)_300px]">
        <CourseViewerSidebar
          course={course}
          currentLessonId={currentLesson?.id}
          onSelectLesson={handleLessonSelect}
          onBack={onBack}
        />

        <ScrollArea className="min-h-0 border-x border-border bg-background">
          <div className="mx-auto flex min-h-full max-w-6xl flex-col gap-5 px-4 py-5 lg:px-6">
            <VideoArea
              key={currentLesson?.id ?? "empty-lesson"}
              lesson={currentLesson}
              onNext={nextLesson ? () => onLessonChange?.(nextLesson.id) : undefined}
              onPrevious={previousLesson ? () => onLessonChange?.(previousLesson.id) : undefined}
            />
          </div>
        </ScrollArea>

        <LessonUtilityPanel course={course} lesson={currentLesson} progress={progress} nextLesson={nextLesson} onNext={nextLesson ? () => onLessonChange?.(nextLesson.id) : undefined} />
      </main>
    </div>
  )
}

function LessonUtilityPanel({
  course,
  lesson,
  progress,
  nextLesson,
  onNext,
}: {
  course: Course
  lesson: Lesson | null
  progress: { completed: number; total: number; percent: number }
  nextLesson: Lesson | null
  onNext?: () => void
}) {
  const currentSection = course.sections.find((section) => section.lessons.some((item) => item.id === lesson?.id))
  const resourceLessons = currentSection?.lessons.filter((item) => item.type === "document" || item.type === "quiz") ?? []

  return (
    <aside className="hidden min-h-0 border-l border-border bg-card xl:flex xl:flex-col">
      <ScrollArea className="min-h-0 flex-1">
        <div className="flex flex-col gap-6 p-5">
          <div className="flex items-start justify-between gap-3">
            <div className="flex flex-col gap-1">
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
              <div className="rounded-xl border border-border bg-background p-4 shadow-[var(--shadow-whisper)]">
                <div className="flex flex-col gap-2">
                  <Badge variant="secondary" className="w-fit rounded-md">{lesson.type}</Badge>
                  <h4 className="text-sm font-semibold leading-snug">{lesson.name}</h4>
                  <p className="text-xs text-muted-foreground">{cleanSectionName(lesson.sectionName) || "Module"}</p>
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
            <h3 className="text-sm font-semibold">Files in this module</h3>
            {resourceLessons.length > 0 ? (
              <div className="flex flex-col gap-2">
                {resourceLessons.map((item) => (
                  <div key={item.id} className="flex items-center gap-3 rounded-lg border border-border bg-background p-3 text-sm">
                    <FileText className="size-4 text-muted-foreground" />
                    <span className="min-w-0 flex-1 truncate">{item.name}</span>
                  </div>
                ))}
              </div>
            ) : (
              <p className="text-sm text-muted-foreground">No extra files detected in this module.</p>
            )}
          </div>
        </div>
      </ScrollArea>

      <div className="border-t border-border p-4">
        <Button type="button" onClick={onNext} disabled={!onNext} className="w-full justify-between rounded-md">
          <span>{nextLesson ? "Go to next item" : "Course complete"}</span>
          <ChevronRight className="size-4" />
        </Button>
      </div>
    </aside>
  )
}
