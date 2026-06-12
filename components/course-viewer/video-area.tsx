"use client"

import { useCallback, useRef, useState } from "react"
import { CheckCircle2, Circle } from "lucide-react"
import { Badge } from "@/components/ui/badge"
import { Button } from "@/components/ui/button"
import { Card, CardContent } from "@/components/ui/card"
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
  const [playhead, setPlayhead] = useState(0)

  const handleProgress = useCallback(
    (currentTime: number, duration: number) => {
      if (!lessonId) return
      setPlayhead(currentTime)
      const now = Date.now()
      const shouldUpdate = now - lastUpdateRef.current > 5000 || currentTime >= duration - 1

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
      <Card className={className}>
        <CardContent className="flex min-h-72 items-center justify-center px-6 py-12 text-center">
          <div className="space-y-2">
            <h3 className="text-xl font-semibold tracking-tight text-foreground">
              Select a lesson to start learning
            </h3>
            <p className="text-sm text-muted-foreground">
              Pick any lesson from the outline to open the player or document viewer.
            </p>
          </div>
        </CardContent>
      </Card>
    )
  }

  const isVideo = lesson.type === "video"

  return (
    <div className={cn("flex flex-col gap-6", className)}>
      <div className="space-y-4">
        <div className="flex flex-col gap-4 lg:flex-row lg:items-start lg:justify-between">
          <div className="space-y-3">
            <div className="flex flex-wrap items-center gap-2 text-xs text-muted-foreground tabular-nums">
              <Badge variant="secondary" className="rounded-full px-2.5 py-1 text-xs font-medium">
                {cleanSectionName(lesson.sectionName) || "Module"}
              </Badge>
              <Badge variant="outline" className="rounded-full px-2.5 py-1 uppercase tracking-wide">
                {lesson.type}
              </Badge>
            </div>
            <div className="space-y-2">
              <h1 className="text-3xl font-bold tracking-tight text-foreground text-balance">
                {lesson.name}
              </h1>
              <p className="text-sm leading-6 text-muted-foreground">
                {lesson.completed
                  ? "Completed"
                  : `Last position ${formatDuration(lesson.lastPosition ?? 0)}`}
              </p>
            </div>
          </div>

          <div className="flex flex-wrap gap-2">
            <Button variant="outline" onClick={onPrevious} disabled={!onPrevious}>
              Previous
            </Button>
            <Button
              variant={lesson.completed ? "secondary" : "outline"}
              onClick={toggleComplete}
              className="gap-2"
            >
              {lesson.completed ? <CheckCircle2 data-icon="inline-start" /> : <Circle data-icon="inline-start" />}
              {lesson.completed ? "Completed" : "Mark complete"}
            </Button>
            <Button onClick={onNext} disabled={!onNext}>
              Next lesson
            </Button>
          </div>
        </div>

        <Card className="overflow-hidden rounded-[28px] border-border/70 shadow-[0_28px_80px_-52px_rgba(15,23,42,0.55)]">
          {isVideo ? (
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
        </Card>
      </div>
    </div>
  )
}
