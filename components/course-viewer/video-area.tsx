"use client"

import { useCallback, useEffect, useMemo, useRef, useState } from "react"
import { readTextFile } from "@tauri-apps/plugin-fs"
import { CheckCircle2, Circle } from "lucide-react"
import { Badge } from "@/components/ui/badge"
import { Button } from "@/components/ui/button"
import { Card, CardContent, CardHeader, CardTitle } from "@/components/ui/card"
import { Separator } from "@/components/ui/separator"
import { ContentViewer } from "@/components/content-viewer"
import { VideoPlayer } from "@/components/video-player"
import { trpc } from "@/lib/trpc/client"
import { isTauri } from "@/lib/tauri"
import { cleanSectionName, cn, formatDuration } from "@/lib/utils"
import type { Lesson } from "@/types"

interface VideoAreaProps {
  className?: string
  lesson?: Lesson | null
  onNext?: () => void
  onPrevious?: () => void
}

type TranscriptCue = {
  id: string
  start: number
  end: number
  text: string
}

const parseTimecode = (value: string) => {
  const clean = value.replace(",", ".")
  const parts = clean.split(":")
  if (parts.length < 2) return 0
  const [hours, minutes, rest] =
    parts.length === 3 ? parts : ["0", parts[0], parts[1]]
  if (!minutes || !rest) return 0
  const [seconds, millis = "0"] = rest.split(".")
  const total =
    Number(hours) * 3600 + Number(minutes) * 60 + Number(seconds) + Number(millis) / 1000
  return Number.isFinite(total) ? total : 0
}

const parseTranscript = (content: string): TranscriptCue[] => {
  const normalized = content.replace(/\r\n/g, "\n").replace(/\r/g, "\n")
  const blocks = normalized
    .split(/\n{2,}/)
    .map((block) => block.trim())
    .filter(Boolean)

  const cues: TranscriptCue[] = []

  blocks.forEach((block, index) => {
    const lines = block.split("\n").map((line) => line.trim())
    const timeLineIndex = lines.findIndex((line) => line.includes("-->"))
    if (timeLineIndex === -1) return

    const timeLine = lines[timeLineIndex]
    const [startRaw, endRaw] = timeLine.split("-->").map((part) => part.trim())
    if (!startRaw || !endRaw) return

    const start = parseTimecode(startRaw)
    const end = parseTimecode(endRaw)
    const text = lines.slice(timeLineIndex + 1).join(" ").trim()

    if (!text) return

    cues.push({
      id: `cue-${index}`,
      start,
      end,
      text,
    })
  })

  return cues
}

export function VideoArea({ className, lesson, onNext, onPrevious }: VideoAreaProps) {
  const utils = trpc.useUtils()
  const lastUpdateRef = useRef(0)
  const transcriptRef = useRef<HTMLDivElement>(null)
  const lessonId = lesson?.id ?? ""
  const lessonDuration = lesson?.duration ?? 0
  const lessonLastPosition = lesson?.lastPosition ?? 0
  const [playhead, setPlayhead] = useState(0)
  const [seekTo, setSeekTo] = useState<number | null>(null)
  const [transcript, setTranscript] = useState<TranscriptCue[]>([])
  const [transcriptLabel, setTranscriptLabel] = useState<string | null>(null)
  const [transcriptError, setTranscriptError] = useState<string | null>(null)
  const updateProgress = trpc.lessons.updateProgress.useMutation({
    onSuccess: async () => {
      await utils.courses.list.invalidate()
    },
  })

  useEffect(() => {
    let isActive = true

    const loadTranscript = async () => {
      setTranscript([])
      setTranscriptLabel(null)
      setTranscriptError(null)

      if (!lesson || lesson.subtitles.length === 0 || !isTauri()) return

      const preferredSubtitle =
        lesson.subtitles.find((item) => item.path.toLowerCase().endsWith(".srt")) ??
        lesson.subtitles[0]

      if (!preferredSubtitle) return

      try {
        const content = await readTextFile(preferredSubtitle.path)
        const cues = parseTranscript(content)
        if (!isActive) return
        setTranscript(cues)
        setTranscriptLabel(preferredSubtitle.label || preferredSubtitle.language)
      } catch {
        if (!isActive) return
        setTranscriptError("Failed to load transcript.")
      }
    }

    loadTranscript()

    return () => {
      isActive = false
    }
  }, [lesson])

  useEffect(() => {
    if (seekTo === null) return
    const timer = window.setTimeout(() => setSeekTo(null), 0)
    return () => window.clearTimeout(timer)
  }, [seekTo])

  const handleProgress = useCallback(
    (currentTime: number, duration: number) => {
      if (!lessonId) return
      setPlayhead(currentTime)
      const now = Date.now()
      const shouldUpdate = now - lastUpdateRef.current > 5000 || currentTime >= duration - 1

      if (!shouldUpdate) return
      lastUpdateRef.current = now

      updateProgress.mutate({
        lessonId,
        watchedTime: currentTime,
        lastPosition: currentTime,
        completed: duration > 0 ? currentTime >= duration - 1 : false,
      })
    },
    [lessonId, updateProgress]
  )

  const handleComplete = useCallback(() => {
    if (!lessonId) return
    const completionTime = lessonDuration > 0 ? lessonDuration : lessonLastPosition
    updateProgress.mutate({
      lessonId,
      watchedTime: completionTime,
      lastPosition: completionTime,
      completed: true,
    })
  }, [lessonDuration, lessonId, lessonLastPosition, updateProgress])

  const toggleComplete = useCallback(() => {
    if (!lesson || !lessonId) return
    const completionTime = Math.max(playhead, lesson.lastPosition)
    updateProgress.mutate({
      lessonId,
      watchedTime: completionTime,
      lastPosition: completionTime,
      completed: !lesson.completed,
    })
  }, [lesson, lessonId, playhead, updateProgress])

  const activeCueIndex = useMemo(() => {
    if (transcript.length === 0) return -1
    return transcript.findIndex((cue) => playhead >= cue.start && playhead <= cue.end)
  }, [playhead, transcript])

  useEffect(() => {
    if (activeCueIndex < 0) return
    const container = transcriptRef.current
    const activeNode = container?.querySelector(
      `[data-cue-index="${activeCueIndex}"]`
    ) as HTMLElement | null

    if (!activeNode) return
    activeNode.scrollIntoView({ block: "nearest", behavior: "smooth" })
  }, [activeCueIndex])

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
  const showTranscript =
    isVideo && (transcript.length > 0 || transcriptError || lesson.subtitles.length > 0)

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
            <div className="aspect-video w-full bg-black">
              <VideoPlayer
                lesson={lesson}
                onProgress={handleProgress}
                onComplete={handleComplete}
                onNext={onNext}
                onPrevious={onPrevious}
                seekTo={seekTo}
              />
            </div>
          ) : (
            <ContentViewer lesson={lesson} onNext={onNext} onPrevious={onPrevious} />
          )}
        </Card>
      </div>

      {showTranscript && (
        <Card>
          <CardHeader className="gap-3">
            <div className="flex flex-wrap items-center justify-between gap-3">
              <div className="space-y-1">
                <CardTitle className="text-xl font-semibold tracking-tight">Transcript</CardTitle>
                <p className="text-sm text-muted-foreground">
                  Click any line to jump to that moment in the lesson.
                </p>
              </div>
              {transcriptLabel ? (
                <Badge variant="secondary" className="rounded-full uppercase">
                  {transcriptLabel}
                </Badge>
              ) : (
                <Badge variant="outline" className="rounded-full text-xs uppercase">
                  Auto
                </Badge>
              )}
            </div>
          </CardHeader>

          <CardContent className="space-y-4">
            <Separator />
            <div ref={transcriptRef} className="max-h-[360px] space-y-3 overflow-y-auto pr-1">
              {transcriptError && <p className="text-sm text-destructive">{transcriptError}</p>}
              {!transcriptError && transcript.length === 0 && (
                <p className="text-sm text-muted-foreground">No transcript found for this lesson.</p>
              )}

              {transcript.map((cue, index) => {
                const isActiveCue = index === activeCueIndex

                return (
                  <button
                    key={cue.id}
                    type="button"
                    data-cue-index={index}
                    onClick={() => setSeekTo(cue.start)}
                    className={cn(
                      "w-full rounded-2xl border px-4 py-3 text-left transition-[background-color,border-color,transform] duration-200 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-ring focus-visible:ring-offset-2",
                      isActiveCue
                        ? "border-primary/25 bg-primary/10"
                        : "border-border/70 bg-background/60 hover:-translate-y-0.5 hover:bg-muted/50"
                    )}
                  >
                    <div className="flex items-center gap-3 text-xs text-muted-foreground tabular-nums">
                      <span className="font-mono">{formatDuration(cue.start)}</span>
                      <span>{formatDuration(cue.end)}</span>
                    </div>
                    <p className="mt-2 text-sm leading-6 text-foreground">{cue.text}</p>
                  </button>
                )
              })}
            </div>
          </CardContent>
        </Card>
      )}
    </div>
  )
}
