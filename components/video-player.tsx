"use client"

import { useRef, useState, useEffect, useCallback, memo } from "react"
import { convertFileSrc } from "@tauri-apps/api/core"
import { readTextFile } from "@tauri-apps/plugin-fs"
import { Button } from "@/components/ui/button"
import { Slider } from "@/components/ui/slider"
import {
  DropdownMenu,
  DropdownMenuContent,
  DropdownMenuItem,
  DropdownMenuTrigger,
} from "@/components/ui/dropdown-menu"
import { createSubtitleTrackId, toVttContent, type VideoSubtitleTrack } from "@/lib/subtitles"
import { cn, formatDuration } from "@/lib/utils"
import { isTauri, preparePlaybackMedia } from "@/lib/tauri"
import { frontendLog } from "@/lib/frontend-log"
import type { Lesson } from "@/types"
import {
  Play,
  Pause,
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
  onNext?: () => void
  autoplay?: boolean
}

const SPEEDS = [0.5, 0.75, 1, 1.25, 1.5, 1.75, 2]
const PROGRESS_SAVE_THROTTLE = 1000
const SLIDER_SYNC_THROTTLE = 250

function createMediaUrl(filePath: string): string {
  return isTauri() ? convertFileSrc(filePath) : filePath
}

function describeMediaError(media: HTMLMediaElement): string {
  const error = media.error
  if (!error) return "unknown media error"

  if (error.code === error.MEDIA_ERR_ABORTED) return "playback was aborted"
  if (error.code === error.MEDIA_ERR_NETWORK) return "media could not be loaded from disk"
  if (error.code === error.MEDIA_ERR_DECODE) return "media codec or container could not be decoded"
  if (error.code === error.MEDIA_ERR_SRC_NOT_SUPPORTED) return "media source or codec is not supported"
  return `media error code ${error.code}`
}

function VideoPlayerComponent({
  lesson,
  onProgress,
  onComplete,
  onNext,
  autoplay = false,
}: VideoPlayerProps) {
  const videoRef = useRef<HTMLVideoElement | HTMLAudioElement | null>(null)
  const containerRef = useRef<HTMLDivElement>(null)
  const [videoSrc, setVideoSrc] = useState<string | null>(null)
  const [subtitleTracks, setSubtitleTracks] = useState<VideoSubtitleTrack[]>([])
  const [activeSubtitleId, setActiveSubtitleId] = useState("off")
  const [isPlaying, setIsPlaying] = useState(false)
  const [duration, setDuration] = useState(0)
  const [volume, setVolume] = useState(1)
  const [isMuted, setIsMuted] = useState(false)
  const [speed, setSpeed] = useState(1)
  const [isEnded, setIsEnded] = useState(false)
  const [isFullscreen, setIsFullscreen] = useState(false)
  const [showControls, setShowControls] = useState(true)
  const [error, setError] = useState<string | null>(null)
  const [isPreparingFallback, setIsPreparingFallback] = useState(false)

  const progressRef = useRef(0)
  const lastProgressSaveRef = useRef(0)
  const lastSliderSyncRef = useRef(0)
  const hasInitialSeekRef = useRef(false)
  const isScrubbingRef = useRef(false)
  const [sliderValue, setSliderValue] = useState(0)
  const controlsTimeoutRef = useRef<NodeJS.Timeout | null>(null)
  const subtitleUrlRefs = useRef<string[]>([])
  const rafRef = useRef<number | null>(null)
  const timeDisplayRef = useRef<HTMLSpanElement>(null)
  const progressBarFillRef = useRef<HTMLDivElement>(null)
  const fallbackAttemptedRef = useRef(false)

  const isVideoFile = lesson.type === "video"
  const isAudioFile = lesson.type === "audio"
  const isPlayableFile = isVideoFile || isAudioFile

  const setMediaRef = useCallback((node: HTMLVideoElement | HTMLAudioElement | null) => {
    videoRef.current = node
  }, [])

  useEffect(() => {
    async function loadVideoSrc() {
      if (!isPlayableFile) return

      try {
        setError(null)
        setIsPreparingFallback(false)
        setVideoSrc(null)
        setSubtitleTracks([])
        setActiveSubtitleId("off")
        fallbackAttemptedRef.current = false
        hasInitialSeekRef.current = false
        progressRef.current = 0
        setSliderValue(0)
        subtitleUrlRefs.current.forEach((url) => URL.revokeObjectURL(url))
        subtitleUrlRefs.current = []

        if (isTauri()) {
          setVideoSrc(createMediaUrl(lesson.path))

          const builtTracks = isVideoFile
            ? await Promise.allSettled(
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
            : []

          const resolvedTracks = builtTracks
            .filter((result): result is PromiseFulfilledResult<VideoSubtitleTrack> => result.status === "fulfilled")
            .map((result) => result.value)

          setSubtitleTracks(resolvedTracks)
          setActiveSubtitleId(resolvedTracks[0]?.id ?? "off")
        } else {
          setVideoSrc(lesson.path)
          const builtTracks = isVideoFile ? lesson.subtitles.map((subtitle, index) => ({
            id: createSubtitleTrackId(subtitle, index),
            label: subtitle.label || subtitle.language || `Track ${index + 1}`,
            language: subtitle.language || "und",
            src: subtitle.path,
          })) : []
          setSubtitleTracks(builtTracks)
          setActiveSubtitleId(builtTracks[0]?.id ?? "off")
        }
      } catch (err) {
        frontendLog("error", "media.source.failed", { path: lesson.path, error: err })
        setError("failed to load media source")
      }
    }
    loadVideoSrc()

    return () => {
      subtitleUrlRefs.current.forEach((url) => URL.revokeObjectURL(url))
      subtitleUrlRefs.current = []
    }
  }, [lesson.path, lesson.subtitles, isPlayableFile, isVideoFile])

  useEffect(() => {
    const video = videoRef.current
    if (!video) return

    const tick = () => {
      const t = video.currentTime
      progressRef.current = t
      const dur = Number.isFinite(video.duration) ? video.duration : 0
      if (timeDisplayRef.current) {
        timeDisplayRef.current.textContent = `${formatDuration(t)} / ${formatDuration(dur)}`
      }
      if (progressBarFillRef.current) {
        const pct = dur > 0 ? (t / dur) * 100 : 0
        progressBarFillRef.current.style.transform = `scaleX(${pct / 100})`
      }
      const now = Date.now()
      if (!isScrubbingRef.current) {
        if (now - lastProgressSaveRef.current > PROGRESS_SAVE_THROTTLE) {
          lastProgressSaveRef.current = now
          if (!video.paused) onProgress(t, dur)
        }
        if (now - lastSliderSyncRef.current > SLIDER_SYNC_THROTTLE) {
          lastSliderSyncRef.current = now
          setSliderValue(t)
        }
      }
      rafRef.current = requestAnimationFrame(tick)
    }

    if (videoSrc) {
      rafRef.current = requestAnimationFrame(tick)
    }

    return () => {
      if (rafRef.current !== null) {
        cancelAnimationFrame(rafRef.current)
        rafRef.current = null
      }
    }
  }, [videoSrc, onProgress])

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

    const handleDurationChange = () => setDuration(Number.isFinite(video.duration) ? video.duration : 0)
    const handleEnded = () => {
      setIsEnded(true)
      setIsPlaying(false)
      onComplete()
    }
    const handlePlay = () => setIsPlaying(true)
    const handlePause = () => {
      setIsPlaying(false)
      if (Number.isFinite(video.duration)) onProgress(video.currentTime, video.duration)
    }
    const handleError = () => {
      const detail = describeMediaError(video)
      frontendLog("error", "media.playback.failed", { path: lesson.path, type: lesson.type, detail })

      if (!isTauri() || fallbackAttemptedRef.current) {
        setError(`playback failed: ${detail}`)
        return
      }

      fallbackAttemptedRef.current = true
      setIsPreparingFallback(true)
      setError("preparing a compatible playback copy...")

      preparePlaybackMedia(lesson.path, isAudioFile ? "audio" : "video")
        .then((prepared) => {
          frontendLog("info", "media.playback.fallback.ready", { originalPath: lesson.path, preparedPath: prepared.path })
          hasInitialSeekRef.current = false
          setError(null)
          setVideoSrc(createMediaUrl(prepared.path))
        })
        .catch((err) => {
          frontendLog("error", "media.playback.fallback.failed", { path: lesson.path, error: err })
          setError(`playback failed: ${detail}`)
        })
        .finally(() => setIsPreparingFallback(false))
    }
    const handleSeeked = () => { isScrubbingRef.current = false }
    const handleSeeking = () => { isScrubbingRef.current = true }

    video.addEventListener("durationchange", handleDurationChange)
    video.addEventListener("ended", handleEnded)
    video.addEventListener("play", handlePlay)
    video.addEventListener("pause", handlePause)
    video.addEventListener("error", handleError)
    video.addEventListener("seeking", handleSeeking)
    video.addEventListener("seeked", handleSeeked)

    if (lesson.lastPosition > 0 && !hasInitialSeekRef.current) {
      video.currentTime = lesson.lastPosition
      hasInitialSeekRef.current = true
    }

    if (autoplay) {
      video.play().catch(() => setIsPlaying(false))
    }

    return () => {
      video.removeEventListener("durationchange", handleDurationChange)
      video.removeEventListener("ended", handleEnded)
      video.removeEventListener("play", handlePlay)
      video.removeEventListener("pause", handlePause)
      video.removeEventListener("error", handleError)
      video.removeEventListener("seeking", handleSeeking)
      video.removeEventListener("seeked", handleSeeked)
    }
  }, [videoSrc, autoplay, isAudioFile, lesson.lastPosition, lesson.path, lesson.type, onComplete, onProgress])

  useEffect(() => {
    const handleFullscreenChange = () => setIsFullscreen(document.fullscreenElement === containerRef.current)
    document.addEventListener("fullscreenchange", handleFullscreenChange)
    return () => document.removeEventListener("fullscreenchange", handleFullscreenChange)
  }, [])

  const togglePlay = useCallback(() => {
    const video = videoRef.current
    if (!video) return
    if (isPlaying) {
      video.pause()
    } else {
      video.play().catch(() => setIsPlaying(false))
    }
  }, [isPlaying])

  const handleScrubChange = useCallback((value: number[]) => {
    const t = value[0]
    setSliderValue(t)
    isScrubbingRef.current = true
    if (progressBarFillRef.current) {
      const dur = duration
      const pct = dur > 0 ? (t / dur) * 100 : 0
      progressBarFillRef.current.style.transform = `scaleX(${pct / 100})`
    }
    if (timeDisplayRef.current) {
      timeDisplayRef.current.textContent = `${formatDuration(t)} / ${formatDuration(duration)}`
    }
  }, [duration])

  const handleScrubCommit = useCallback(
    (value: number[]) => {
      const video = videoRef.current
      if (!video) return
    const t = value[0]
    video.currentTime = t
    progressRef.current = t
    onProgress(t, Number.isFinite(video.duration) ? video.duration : 0)
    lastProgressSaveRef.current = Date.now()
    },
    [onProgress]
  )

  const handleVolumeChange = useCallback((value: number[]) => {
    const newVolume = value[0]
    const video = videoRef.current
    if (!video) return
    video.volume = newVolume
    video.muted = newVolume === 0
    setVolume(newVolume)
    setIsMuted(newVolume === 0)
  }, [])

  const toggleMute = useCallback(() => {
    const video = videoRef.current
    if (!video) return
    const newMuted = !isMuted
    video.muted = newMuted
    if (!newMuted && video.volume === 0) {
      video.volume = 0.75
      setVolume(0.75)
    }
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
      containerRef.current.requestFullscreen().catch(() => setIsFullscreen(false))
    } else {
      document.exitFullscreen().catch(() => setIsFullscreen(false))
    }
  }, [])

  useEffect(() => {
    function handleKeyDown(event: KeyboardEvent) {
      const target = event.target as HTMLElement | null
      if (target?.closest("input, textarea, select, [contenteditable='true']")) return

      const video = videoRef.current
      if (!video) return

      if (event.key === " " || event.key.toLowerCase() === "k") {
        event.preventDefault()
        if (video.paused) video.play().catch(() => setIsPlaying(false))
        else video.pause()
      }
      if (event.key.toLowerCase() === "m") {
        event.preventDefault()
        toggleMute()
      }
      if (event.key.toLowerCase() === "f") {
        event.preventDefault()
        toggleFullscreen()
      }
      if (event.key.toLowerCase() === "j" || event.key === "ArrowLeft") {
        event.preventDefault()
        video.currentTime = Math.max(0, video.currentTime - 10)
      }
      if (event.key.toLowerCase() === "l" || event.key === "ArrowRight") {
        event.preventDefault()
        video.currentTime = Math.min(Number.isFinite(video.duration) ? video.duration : video.currentTime + 10, video.currentTime + 10)
      }
    }

    document.addEventListener("keydown", handleKeyDown)
    return () => document.removeEventListener("keydown", handleKeyDown)
  }, [toggleFullscreen, toggleMute])

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

  if (!isPlayableFile) {
    return (
      <div className="flex aspect-video w-full items-center justify-center bg-muted">
        <div className="text-center">
          <p className="text-lg font-medium">{lesson.name}</p>
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
      className={cn(
        "group relative flex w-full flex-col overflow-hidden rounded-xl bg-black shadow-lg ring-1 ring-white/10",
        isVideoFile ? "aspect-video" : "min-h-[360px]"
      )}
      onMouseMove={handleMouseMove}
      onMouseLeave={() => isPlaying && setShowControls(false)}
    >
      {isVideoFile ? (
        <video
          ref={setMediaRef}
          src={videoSrc}
          className="size-full object-contain"
          onClick={togglePlay}
          playsInline
          preload="auto"
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
      ) : (
        <div className="flex min-h-[360px] flex-1 items-center justify-center p-8">
          <audio ref={setMediaRef} src={videoSrc} preload="auto" />
          <div className="flex max-w-lg flex-col items-center gap-5 text-center text-white">
            <div className="flex size-20 items-center justify-center rounded-2xl bg-white/10 ring-1 ring-white/10">
              <Play className="size-9" />
            </div>
            <div className="flex flex-col gap-2">
              <h2 className="text-2xl font-semibold tracking-tight">{lesson.name}</h2>
              <p className="text-sm text-white/60">Audio lesson</p>
            </div>
          </div>
        </div>
      )}

      {error && (
        <div className="absolute inset-0 flex items-center justify-center bg-black/80">
          <div className="max-w-md px-6 text-center text-white">
            <p className="font-medium">{error}</p>
            {isPreparingFallback && <p className="mt-2 text-sm text-white/70">This can take a moment for uncommon codecs.</p>}
          </div>
        </div>
      )}

      {isEnded && (
        <div className="absolute inset-0 z-20 flex items-center justify-center bg-black/70">
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
        <div className="relative mb-4 h-1.5 cursor-pointer" aria-label="video progress">
          <div className="absolute inset-0 rounded-full bg-white/20" />
          <div
            ref={progressBarFillRef}
            className="absolute inset-y-0 left-0 origin-left rounded-full bg-white"
            style={{ width: "100%", transform: "scaleX(0)" }}
          />
          <Slider
            value={[sliderValue]}
            max={duration || 100}
            step={0.1}
            onValueChange={handleScrubChange}
            onValueCommit={handleScrubCommit}
            className="absolute inset-0 cursor-pointer opacity-0"
            aria-label="video progress"
          />
        </div>

        <div className="flex items-center justify-between text-white">
          <div className="flex items-center gap-2">
            <Button variant="ghost" size="icon" onClick={togglePlay} className="text-white hover:bg-white/20" aria-label={isPlaying ? "pause" : "play"}>
              {isPlaying ? <Pause className="size-6" aria-hidden="true" /> : <Play className="size-6" aria-hidden="true" />}
            </Button>

            <div className="group/vol ml-2 flex items-center gap-2">
              <Button variant="ghost" size="icon" onClick={toggleMute} className="text-white hover:bg-white/20" aria-label={isMuted ? "unmute" : "mute"}>
                {isMuted ? <VolumeX className="size-5" aria-hidden="true" /> : <Volume2 className="size-5" aria-hidden="true" />}
              </Button>
              <Slider
                value={[isMuted ? 0 : volume]}
                max={1}
                step={0.01}
                onValueChange={handleVolumeChange}
                className="pointer-events-none w-20 origin-left scale-x-95 opacity-0 transition-[opacity,transform] duration-200 group-hover/vol:pointer-events-auto group-hover/vol:scale-x-100 group-hover/vol:opacity-100"
                aria-label="volume"
              />
            </div>

            <span ref={timeDisplayRef} className="ml-2 text-xs font-medium tabular-nums opacity-90">
              0:00 / 0:00
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
