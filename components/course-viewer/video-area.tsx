"use client"

import { useCallback, useRef, useState } from "react"
import { CheckCircle2, ChevronLeft, ChevronRight, Circle } from "lucide-react"
import { Badge } from "@/components/ui/badge"
import { Button } from "@/components/ui/button"
import { ContentViewer } from "@/components/content-viewer"
import { VideoPlayer } from "@/components/video-player"
import { recordLessonProgress } from "@/lib/operations"
import { cleanSectionName, cn, formatDuration } from "@/lib/utils"
import type { Lesson } from "@/types"

interface VideoAreaProps {
  className?: string
  lesson?: Lesson | null
  onNext?: () => void
  onPrevious?: () => void
}

export function VideoArea({ className, lesson, onNext, onPrevious }: VideoAreaProps) {
  const lastUpdateRef = useRef(0)
  const lessonId = lesson?.id ?? ""
  const lessonDuration = lesson?.duration ?? 0
  const lessonLastPosition = lesson?.lastPosition ?? 0
  const [playhead, setPlayhead] = useState(lessonLastPosition)

  const handleProgress = useCallback(
    (currentTime: number, duration: number) => {
      if (!lessonId) return
      setPlayhead(currentTime)
      const now = Date.now()
      const shouldUpdate = now - lastUpdateRef.current > 5000 || (duration > 0 && currentTime >= duration - 1)

      if (!shouldUpdate) return
      lastUpdateRef.current = now

      void recordLessonProgress(lessonId, {
        watchedTime: currentTime,
        lastPosition: currentTime,
        completed: duration > 0 ? currentTime >= duration - 1 : false,
      })
    },
    [lessonId]
  )

  const handleComplete = useCallback(() => {
    if (!lessonId) return
    const completionTime = lessonDuration > 0 ? lessonDuration : lessonLastPosition
    void recordLessonProgress(lessonId, {
      watchedTime: completionTime,
      lastPosition: completionTime,
      completed: true,
    })
  }, [lessonDuration, lessonId, lessonLastPosition])

  const toggleComplete = useCallback(() => {
    if (!lesson || !lessonId) return
    const completionTime = Math.max(playhead, lesson.lastPosition)
    void recordLessonProgress(lessonId, {
      watchedTime: completionTime,
      lastPosition: completionTime,
      completed: !lesson.completed,
    })
  }, [lesson, lessonId, playhead])

  if (!lesson) {
    return (
      <div className={cn("flex min-h-[70vh] items-center justify-center rounded-2xl border border-border bg-card px-6 py-12 text-center", className)}>
        <div className="flex flex-col gap-2">
          <h3 className="text-xl font-semibold tracking-tight">Select a lesson to start learning</h3>
          <p className="text-sm text-muted-foreground">Pick any item from the outline.</p>
        </div>
      </div>
    )
  }

  const isPlayable = lesson.type === "video" || lesson.type === "audio"

  return (
    <div className={cn("flex min-h-[calc(100vh-7rem)] flex-col gap-5", className)}>
      <div className="paper-panel-strong overflow-hidden rounded-2xl">
        {isPlayable ? (
          <VideoPlayer
            lesson={lesson}
            onProgress={handleProgress}
            onComplete={handleComplete}
            onNext={onNext}
            onPrevious={onPrevious}
          />
        ) : (
          <ContentViewer lesson={lesson} onNext={onNext} onPrevious={onPrevious} />
        )}
      </div>

      <section className="paper-panel flex flex-col gap-5 rounded-2xl p-5">
        <div className="flex flex-col gap-4 lg:flex-row lg:items-start lg:justify-between">
          <div className="min-w-0 flex-1">
            <div className="mb-3 flex flex-wrap items-center gap-2">
              <Badge variant="secondary" className="rounded-md">{cleanSectionName(lesson.sectionName) || "Module"}</Badge>
              <Badge variant="outline" className="rounded-md uppercase tracking-wide">{lesson.type}</Badge>
              {lesson.subtitles.length > 0 && <Badge variant="outline" className="rounded-md">Subtitles</Badge>}
            </div>
            <h1 className="text-2xl font-semibold tracking-tight md:text-3xl">{lesson.name}</h1>
            <p className="mt-2 text-sm text-muted-foreground">
              {lesson.completed ? "Completed" : `Last position ${formatDuration(Math.max(playhead, lesson.lastPosition ?? 0))}`}
            </p>
          </div>

          <div className="flex flex-wrap gap-2">
            <Button type="button" variant="outline" onClick={onPrevious} disabled={!onPrevious} className="gap-2">
              <ChevronLeft className="size-4" />
              Previous
            </Button>
            <Button type="button" variant={lesson.completed ? "secondary" : "outline"} onClick={toggleComplete} className="gap-2">
              {lesson.completed ? <CheckCircle2 className="size-4" /> : <Circle className="size-4" />}
              {lesson.completed ? "Completed" : "Mark complete"}
            </Button>
            <Button type="button" onClick={onNext} disabled={!onNext} className="gap-2">
              Next item
              <ChevronRight className="size-4" />
            </Button>
          </div>
        </div>
      </section>
    </div>
  )
}
