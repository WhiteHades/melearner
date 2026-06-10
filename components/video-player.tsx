"use client"

import { useRef, useState, useEffect, useCallback, memo } from "react"
import { invoke } from "@tauri-apps/api/core"
import { Button } from "@/components/ui/button"
import { Slider } from "@/components/ui/slider"
import {
  DropdownMenu,
  DropdownMenuContent,
  DropdownMenuItem,
  DropdownMenuTrigger,
} from "@/components/ui/dropdown-menu"
import { readTextFile } from "@tauri-apps/plugin-fs"
import { createSubtitleTrackId, toVttContent, type VideoSubtitleTrack } from "@/lib/subtitles"
import { cn, formatDuration } from "@/lib/utils"
import { isTauri } from "@/lib/tauri"
import type { Lesson } from "@/types"
import {
  Play,
  Pause,
  SkipBack,
  SkipForward,
  Volume2,
  VolumeX,
  RotateCcw,
  Check,
  Maximize,
  Minimize,
} from "lucide-react"

interface VideoPlayerProps {
  lesson: Lesson
  onProgress: (currentTime: number, duration: number) => void
  onComplete: () => void
  onPrevious?: () => void
  onNext?: () => void
  autoplay?: boolean
  seekTo?: number | null
}

const SPEEDS = [0.5, 0.75, 1, 1.25, 1.5, 1.75, 2]
const TIME_UPDATE_THROTTLE = 250

function createMediaUrl(port: number, filePath: string): string {
  return `http://127.0.0.1:${port}/video/${encodeURIComponent(filePath)}`
}

function VideoPlayerComponent({
  lesson,
  onProgress,
  onComplete,
  onPrevious,
  onNext,
  autoplay = false,
  seekTo,
}: VideoPlayerProps) {
  const videoRef = useRef<HTMLVideoElement>(null)
  const containerRef = useRef<HTMLDivElement>(null)
  const [videoSrc, setVideoSrc] = useState<string | null>(null)
  const [subtitleTracks, setSubtitleTracks] = useState<VideoSubtitleTrack[]>([])
  const [activeSubtitleId, setActiveSubtitleId] = useState("off")
  const [isPlaying, setIsPlaying] = useState(false)
  const [currentTime, setCurrentTime] = useState(0)
  const [duration, setDuration] = useState(0)
  const [volume, setVolume] = useState(1)
  const [isMuted, setIsMuted] = useState(false)
  const [speed, setSpeed] = useState(1)
  const [isEnded, setIsEnded] = useState(false)
  const [isFullscreen, setIsFullscreen] = useState(false)
  const [showControls, setShowControls] = useState(true)
  const controlsTimeoutRef = useRef<NodeJS.Timeout | null>(null)
  const hasInitialSeekRef = useRef(false)
  const [error, setError] = useState<string | null>(null)
  const lastTimeUpdateRef = useRef(0)
  const isSeekingRef = useRef(false)
  const subtitleUrlRefs = useRef<string[]>([])

  const isVideoFile = lesson.type === "video"

  useEffect(() => {
    async function loadVideoSrc() {
      if (!isVideoFile) return

      try {
        setError(null)
        setVideoSrc(null)
        setSubtitleTracks([])
        setActiveSubtitleId("off")
        subtitleUrlRefs.current.forEach((url) => URL.revokeObjectURL(url))
        subtitleUrlRefs.current = []

        if (isTauri()) {
          const port = await invoke<number>("get_video_server_port")
          const src = createMediaUrl(port, lesson.path)
          setVideoSrc(src)

          const builtTracks = await Promise.allSettled(
            lesson.subtitles.map(async (subtitle, index) => {
              const content = await readTextFile(subtitle.path)
              const blob = new Blob([toVttContent(content, subtitle.path)], { type: "text/vtt" })
              const url = URL.createObjectURL(blob)
              subtitleUrlRefs.current.push(url)

              return {
                id: createSubtitleTrackId(subtitle, index),
                label: subtitle.label || subtitle.language || `Track ${index + 1}`,
                language: subtitle.language || "und",
                src: url,
              }
            })
          )

          const resolvedTracks = builtTracks
            .filter((result): result is PromiseFulfilledResult<VideoSubtitleTrack> => result.status === "fulfilled")
            .map((result) => result.value)

          setSubtitleTracks(resolvedTracks)
          setActiveSubtitleId(resolvedTracks[0]?.id ?? "off")
        } else {
          setVideoSrc(lesson.path)
          const builtTracks = lesson.subtitles.map((subtitle, index) => ({
            id: createSubtitleTrackId(subtitle, index),
            label: subtitle.label || subtitle.language || `Track ${index + 1}`,
            language: subtitle.language || "und",
            src: subtitle.path,
          }))
          setSubtitleTracks(builtTracks)
          setActiveSubtitleId(builtTracks[0]?.id ?? "off")
        }
      } catch {
        setError("failed to load video source")
      }
    }
    loadVideoSrc()

    return () => {
      subtitleUrlRefs.current.forEach((url) => URL.revokeObjectURL(url))
      subtitleUrlRefs.current = []
    }
  }, [lesson.path, lesson.subtitles, isVideoFile])

  useEffect(() => {
    const video = videoRef.current
    if (!video) return

    subtitleTracks.forEach((track, index) => {
      const textTrack = video.textTracks[index]
      if (!textTrack) return
      textTrack.mode = activeSubtitleId !== "off" && track.id === activeSubtitleId ? "showing" : "disabled"
    })
  }, [activeSubtitleId, subtitleTracks])

  useEffect(() => {
    const video = videoRef.current
    if (!video || !videoSrc) return

    const handleTimeUpdate = () => {
      if (isSeekingRef.current) return
      const now = Date.now()
      if (now - lastTimeUpdateRef.current < TIME_UPDATE_THROTTLE) return
      lastTimeUpdateRef.current = now
      const t = video.currentTime
      setCurrentTime(t)
      onProgress(t, video.duration)
    }

    const handleDurationChange = () => setDuration(video.duration)
    const handleEnded = () => {
      setIsEnded(true)
      setIsPlaying(false)
      onComplete()
    }
    const handlePlay = () => setIsPlaying(true)
    const handlePause = () => setIsPlaying(false)
    const handleError = () => setError("video playback failed")

    video.addEventListener("timeupdate", handleTimeUpdate)
    video.addEventListener("durationchange", handleDurationChange)
    video.addEventListener("ended", handleEnded)
    video.addEventListener("play", handlePlay)
    video.addEventListener("pause", handlePause)
    video.addEventListener("error", handleError)

    if (lesson.lastPosition > 0 && !hasInitialSeekRef.current) {
      video.currentTime = lesson.lastPosition
      hasInitialSeekRef.current = true
    }

    if (autoplay) {
      video.play().catch(() => setIsPlaying(false))
    }

    return () => {
      video.removeEventListener("timeupdate", handleTimeUpdate)
      video.removeEventListener("durationchange", handleDurationChange)
      video.removeEventListener("ended", handleEnded)
      video.removeEventListener("play", handlePlay)
      video.removeEventListener("pause", handlePause)
      video.removeEventListener("error", handleError)
    }
  }, [videoSrc, autoplay, lesson.lastPosition, onProgress, onComplete])

  useEffect(() => {
    if (seekTo === null || seekTo === undefined) return
    const video = videoRef.current
    if (!video) return
    video.currentTime = seekTo
    const frame = window.requestAnimationFrame(() => setCurrentTime(seekTo))
    onProgress(seekTo, video.duration)
    return () => window.cancelAnimationFrame(frame)
  }, [seekTo, onProgress])

  const togglePlay = useCallback(() => {
    const video = videoRef.current
    if (!video) return
    if (isPlaying) {
      video.pause()
    } else {
      video.play()
    }
  }, [isPlaying])

  const handleSeekCommit = useCallback(
    (value: number[]) => {
      const video = videoRef.current
      if (!video) return
      const t = value[0]
      video.currentTime = t
      setCurrentTime(t)
      onProgress(t, video.duration)
      isSeekingRef.current = false
    },
    [onProgress]
  )

  const handleVolumeChange = useCallback((value: number[]) => {
    const newVolume = value[0]
    const video = videoRef.current
    if (!video) return
    video.volume = newVolume
    setVolume(newVolume)
    setIsMuted(newVolume === 0)
  }, [])

  const toggleMute = useCallback(() => {
    const video = videoRef.current
    if (!video) return
    const newMuted = !isMuted
    video.muted = newMuted
    setIsMuted(newMuted)
  }, [isMuted])

  const changeSpeed = useCallback((newSpeed: number) => {
    const video = videoRef.current
    if (!video) return
    video.playbackRate = newSpeed
    setSpeed(newSpeed)
  }, [])

  const handleReplay = useCallback(() => {
    const video = videoRef.current
    if (!video) return
    video.currentTime = 0
    video.play()
    setIsEnded(false)
  }, [])

  const toggleFullscreen = useCallback(() => {
    if (!containerRef.current) return
    if (!document.fullscreenElement) {
      containerRef.current.requestFullscreen()
      setIsFullscreen(true)
    } else {
      document.exitFullscreen()
      setIsFullscreen(false)
    }
  }, [])

  const handleMouseMove = useCallback(() => {
    setShowControls(true)
    if (controlsTimeoutRef.current) {
      clearTimeout(controlsTimeoutRef.current)
    }
    controlsTimeoutRef.current = setTimeout(() => {
      if (isPlaying) setShowControls(false)
    }, 2500)
  }, [isPlaying])

  const subtitleButtonLabel =
    activeSubtitleId === "off"
      ? "CC Off"
      : subtitleTracks.find((track) => track.id === activeSubtitleId)?.label ?? "CC"

  if (!isVideoFile) {
    return (
      <div className="flex aspect-video w-full items-center justify-center bg-muted">
        <div className="text-center">
          <p className="text-lg font-medium">{lesson.name}</p>
          <div className="mt-4 flex justify-center gap-2">
            {onPrevious && (
              <Button variant="outline" size="sm" onClick={onPrevious}>
                <SkipBack className="mr-1 size-4" /> previous
              </Button>
            )}
            {onNext && (
              <Button size="sm" onClick={onNext}>
                next <SkipForward className="ml-1 size-4" />
              </Button>
            )}
          </div>
        </div>
      </div>
    )
  }

  if (!videoSrc) {
    return (
      <div className="flex aspect-video w-full items-center justify-center bg-black">
        <p className="text-white">loading...</p>
      </div>
    )
  }

  return (
    <div
      ref={containerRef}
      className="group relative flex aspect-video w-full flex-col overflow-hidden rounded-xl bg-black shadow-lg ring-1 ring-white/10"
      onMouseMove={handleMouseMove}
      onMouseLeave={() => isPlaying && setShowControls(false)}
    >
      <video
        ref={videoRef}
        src={videoSrc}
        className="size-full object-cover"
        onClick={togglePlay}
        playsInline
        preload="auto"
        style={{
          willChange: "transform",
          transform: "translateZ(0)",
        }}
      >
        {subtitleTracks.map((track) => (
          <track
            key={track.id}
            id={track.id}
            kind="subtitles"
            src={track.src}
            srcLang={track.language}
            label={track.label}
            default={track.id === activeSubtitleId}
          />
        ))}
      </video>

      {error && (
        <div className="absolute inset-0 flex items-center justify-center bg-black/80">
          <p className="text-white">{error}</p>
        </div>
      )}

      {isEnded && (
        <div className="absolute inset-0 z-20 flex items-center justify-center bg-black/60 backdrop-blur-sm">
          <div className="flex gap-4">
            <Button variant="outline" onClick={handleReplay}>
              <RotateCcw className="mr-2 size-4" /> replay
            </Button>
            {onNext && (
              <Button onClick={onNext}>
                next lesson <SkipForward className="ml-2 size-4" />
              </Button>
            )}
          </div>
        </div>
      )}

      <div
        className={cn(
          "absolute inset-x-0 bottom-0 z-10 bg-linear-to-t from-black/90 to-transparent p-4 transition-opacity duration-300",
          showControls || !isPlaying ? "opacity-100" : "pointer-events-none opacity-0"
        )}
      >
        <Slider
          value={[currentTime]}
          max={duration || 100}
          step={0.1}
          onValueChange={() => { isSeekingRef.current = true }}
          onValueCommit={handleSeekCommit}
          className="mb-4 cursor-pointer"
          aria-label="video progress"
        />

        <div className="flex items-center justify-between text-white">
          <div className="flex items-center gap-2">
            {onPrevious && (
              <Button variant="ghost" size="icon" onClick={onPrevious} className="text-white hover:bg-white/20" aria-label="previous lesson">
                <SkipBack className="size-5" aria-hidden="true" />
              </Button>
            )}

            <Button variant="ghost" size="icon" onClick={togglePlay} className="text-white hover:bg-white/20" aria-label={isPlaying ? "pause" : "play"}>
              {isPlaying ? <Pause className="size-6" aria-hidden="true" /> : <Play className="size-6" aria-hidden="true" />}
            </Button>

            {onNext && (
              <Button variant="ghost" size="icon" onClick={onNext} className="text-white hover:bg-white/20" aria-label="next lesson">
                <SkipForward className="size-5" aria-hidden="true" />
              </Button>
            )}

            <div className="group/vol ml-2 flex items-center gap-2">
              <Button variant="ghost" size="icon" onClick={toggleMute} className="text-white hover:bg-white/20" aria-label={isMuted ? "unmute" : "mute"}>
                {isMuted ? <VolumeX className="size-5" aria-hidden="true" /> : <Volume2 className="size-5" aria-hidden="true" />}
              </Button>
              <Slider
                value={[isMuted ? 0 : volume]}
                max={1}
                step={0.01}
                onValueChange={handleVolumeChange}
                className="w-0 scale-x-0 transition-all duration-200 group-hover/vol:w-20 group-hover/vol:scale-x-100"
                aria-label="volume"
              />
            </div>

            <span className="ml-2 text-xs font-medium tabular-nums opacity-90">
              {formatDuration(currentTime)} / {formatDuration(duration)}
            </span>
          </div>

          <div className="flex items-center gap-2">
            <DropdownMenu>
              <DropdownMenuTrigger asChild>
                <Button variant="ghost" size="sm" className="h-8 gap-1 px-2 text-white hover:bg-white/20">
                  <span className="text-xs font-bold">{speed}x</span>
                </Button>
              </DropdownMenuTrigger>
              <DropdownMenuContent align="end" className="w-20 min-w-20 bg-black text-white border-white/10 [&_[data-highlighted]]:bg-white/10 [&_[data-highlighted]]:text-white">
                {SPEEDS.map((s) => (
                  <DropdownMenuItem key={s} onClick={() => changeSpeed(s)}>
                    {speed === s && <Check className="mr-2 size-3" />}
                    {s}x
                  </DropdownMenuItem>
                ))}
              </DropdownMenuContent>
            </DropdownMenu>

            {subtitleTracks.length > 0 && (
              <DropdownMenu>
                <DropdownMenuTrigger asChild>
                  <Button variant="ghost" size="sm" className="h-8 gap-1 px-2 text-white hover:bg-white/20">
                    <span className="text-xs font-bold">{subtitleButtonLabel}</span>
                  </Button>
                </DropdownMenuTrigger>
                <DropdownMenuContent align="end" className="min-w-32 border-white/10 bg-black text-white [&_[data-highlighted]]:bg-white/10 [&_[data-highlighted]]:text-white">
                  <DropdownMenuItem onClick={() => setActiveSubtitleId("off")}>
                    {activeSubtitleId === "off" && <Check className="mr-2 size-3" />}
                    Off
                  </DropdownMenuItem>
                  {subtitleTracks.map((track) => (
                    <DropdownMenuItem key={track.id} onClick={() => setActiveSubtitleId(track.id)}>
                      {activeSubtitleId === track.id && <Check className="mr-2 size-3" />}
                      {track.label}
                    </DropdownMenuItem>
                  ))}
                </DropdownMenuContent>
              </DropdownMenu>
            )}

            <Button variant="ghost" size="icon" onClick={toggleFullscreen} className="text-white hover:bg-white/20" aria-label={isFullscreen ? "exit fullscreen" : "fullscreen"}>
              {isFullscreen ? <Minimize className="size-5" aria-hidden="true" /> : <Maximize className="size-5" aria-hidden="true" />}
            </Button>
          </div>
        </div>
      </div>
    </div>
  )
}

export const VideoPlayer = memo(VideoPlayerComponent)
