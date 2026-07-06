"use client"

import { memo, useCallback, useEffect, useMemo, useRef, useState, type ReactNode } from "react"
import {
  Camera,
  Captions,
  Maximize,
  Pause,
  Play,
  SkipForward,
  SlidersHorizontal,
  Volume2,
  VolumeX,
} from "lucide-react"
import { Button } from "@/components/ui/button"
import {
  DropdownMenu,
  DropdownMenuContent,
  DropdownMenuItem,
  DropdownMenuLabel,
  DropdownMenuRadioGroup,
  DropdownMenuRadioItem,
  DropdownMenuSeparator,
  DropdownMenuTrigger,
} from "@/components/ui/dropdown-menu"
import { Slider } from "@/components/ui/slider"
import {
  Tooltip,
  TooltipContent,
  TooltipProvider,
  TooltipTrigger,
} from "@/components/ui/tooltip"
import {
  destroyNativePlayer,
  getNativePlayerState,
  loadNativePlayerFile,
  pauseNativePlayer,
  playNativePlayer,
  seekNativePlayer,
  setNativePlayerBounds,
  setNativePlayerMuted,
  setNativePlayerRate,
  setNativePlayerVolume,
  stepNativePlayerFrame,
  takeNativePlayerScreenshot,
  type NativePlayerState,
} from "@/lib/native-player"
import { isTauri } from "@/lib/tauri"
import { formatDuration } from "@/lib/utils"
import type { Lesson } from "@/types"

interface VideoPlayerProps {
  lesson: Lesson
  libraryRoot: string
  onProgress: (currentTime: number, duration: number) => void
  onComplete: () => void
  onNext?: () => void
  autoplay?: boolean
}

const PLAYBACK_RATES = [0.5, 0.75, 1, 1.25, 1.5, 1.75, 2]
const POSITION_SAVE_MS = 5000
const STATE_POLL_MS = 250

const initialState: NativePlayerState = {
  path: null,
  paused: true,
  buffering: false,
  currentTime: 0,
  duration: 0,
  volume: 1,
  muted: false,
  rate: 1,
  width: null,
  height: null,
  audioTracks: [],
  subtitleTracks: [],
  selectedAudioTrackId: null,
  selectedSubtitleTrackId: null,
}

function VideoPlayerComponent({
  lesson,
  libraryRoot,
  onProgress,
  onComplete,
  onNext,
  autoplay = false,
}: VideoPlayerProps) {
  const surfaceRef = useRef<HTMLDivElement | null>(null)
  const lastSaveRef = useRef(0)
  const [nativeState, setNativeState] = useState<NativePlayerState>(initialState)
  const [error, setError] = useState<{ path: string; message: string } | null>(null)
  const [isFullscreen, setIsFullscreen] = useState(false)
  const isPlayable = lesson.type === "video" || lesson.type === "audio"
  const fallbackState = useMemo<NativePlayerState>(() => ({
    ...initialState,
    path: lesson.path,
    paused: !autoplay,
    currentTime: lesson.lastPosition,
    duration: lesson.duration,
  }), [autoplay, lesson.duration, lesson.lastPosition, lesson.path])
  const state = nativeState.path === lesson.path ? nativeState : fallbackState

  const updateBounds = useCallback(() => {
    const surface = surfaceRef.current
    if (!surface || !isTauri()) return
    const rect = surface.getBoundingClientRect()
    void setNativePlayerBounds({
      x: Math.round(rect.left),
      y: Math.round(rect.top),
      width: Math.round(rect.width),
      height: Math.round(rect.height),
      scaleFactor: window.devicePixelRatio || 1,
    }).catch((reason) => setError({ path: lesson.path, message: String(reason) }))
  }, [lesson.path])

  useEffect(() => {
    if (!isPlayable || !isTauri()) return
    let isActive = true

    void loadNativePlayerFile({ path: lesson.path, allowedRoots: [libraryRoot], startTime: lesson.lastPosition || undefined, autoplay })
      .then((next) => {
        if (!isActive) return
        setError(null)
        setNativeState(next)
      })
      .catch((reason) => {
        if (isActive) setError({ path: lesson.path, message: String(reason) })
      })

    return () => {
      isActive = false
      if (isTauri()) void destroyNativePlayer().catch(() => undefined)
    }
  }, [autoplay, isPlayable, lesson.duration, lesson.id, lesson.lastPosition, lesson.path, libraryRoot])

  useEffect(() => {
    const surface = surfaceRef.current
    if (!surface) return

    updateBounds()
    const observer = new ResizeObserver(updateBounds)
    observer.observe(surface)
    window.addEventListener("resize", updateBounds)
    return () => {
      observer.disconnect()
      window.removeEventListener("resize", updateBounds)
    }
  }, [updateBounds])

  useEffect(() => {
    if (!isPlayable || !isTauri()) return
    let cancelled = false
    let inFlight = false

    const poll = () => {
      if (inFlight) return
      inFlight = true
      void getNativePlayerState()
        .then((next) => {
          if (!cancelled && next.path === lesson.path) setNativeState(next)
        })
        .catch((reason) => {
          if (!cancelled) setError({ path: lesson.path, message: String(reason) })
        })
        .finally(() => {
          inFlight = false
        })
    }

    poll()
    const interval = window.setInterval(poll, STATE_POLL_MS)
    return () => {
      cancelled = true
      window.clearInterval(interval)
    }
  }, [isPlayable, lesson.path])

  useEffect(() => {
    const now = Date.now()
    const shouldSave = now - lastSaveRef.current >= POSITION_SAVE_MS || state.currentTime >= state.duration - 1
    if (!isPlayable || !shouldSave) return
    lastSaveRef.current = now
    onProgress(state.currentTime, state.duration)
    if (state.duration > 0 && state.currentTime >= state.duration - 1) onComplete()
  }, [isPlayable, onComplete, onProgress, state.currentTime, state.duration])

  const formattedPosition = useMemo(() => {
    return `${formatDuration(state.currentTime)} / ${formatDuration(state.duration)}`
  }, [state.currentTime, state.duration])

  const togglePlayback = useCallback(() => {
    const nextPaused = !state.paused
    setNativeState((current) => ({ ...(current.path === lesson.path ? current : fallbackState), paused: nextPaused }))
    const action = nextPaused ? pauseNativePlayer : playNativePlayer
    void action().catch((reason) => setError({ path: lesson.path, message: String(reason) }))
  }, [fallbackState, lesson.path, state.paused])

  const changeSeek = useCallback((value: number[]) => {
    const currentTime = value[0] ?? 0
    setNativeState((current) => ({ ...(current.path === lesson.path ? current : fallbackState), currentTime }))
  }, [fallbackState, lesson.path])

  const commitSeek = useCallback((value: number[]) => {
    const currentTime = value[0] ?? 0
    setNativeState((current) => ({ ...(current.path === lesson.path ? current : fallbackState), currentTime }))
    void seekNativePlayer({ seconds: currentTime, mode: "absolute" }).catch((reason) => setError({ path: lesson.path, message: String(reason) }))
    onProgress(currentTime, state.duration)
  }, [fallbackState, lesson.path, onProgress, state.duration])

  const changeVolume = useCallback((value: number[]) => {
    const volume = value[0] ?? 0
    setNativeState((current) => ({ ...(current.path === lesson.path ? current : fallbackState), volume, muted: volume === 0 }))
    void setNativePlayerVolume(volume).catch((reason) => setError({ path: lesson.path, message: String(reason) }))
  }, [fallbackState, lesson.path])

  const toggleMute = useCallback(() => {
    const muted = !state.muted
    setNativeState((current) => ({ ...(current.path === lesson.path ? current : fallbackState), muted }))
    void setNativePlayerMuted(muted).catch((reason) => setError({ path: lesson.path, message: String(reason) }))
  }, [fallbackState, lesson.path, state.muted])

  const changeRate = useCallback((rate: number) => {
    setNativeState((current) => ({ ...(current.path === lesson.path ? current : fallbackState), rate }))
    void setNativePlayerRate(rate).catch((reason) => setError({ path: lesson.path, message: String(reason) }))
  }, [fallbackState, lesson.path])

  const toggleFullscreen = useCallback(() => {
    const surface = surfaceRef.current
    if (!surface) return
    if (document.fullscreenElement) {
      void document.exitFullscreen()
      setIsFullscreen(false)
    } else {
      void surface.requestFullscreen()
      setIsFullscreen(true)
    }
  }, [])

  if (!isPlayable) {
    return (
      <div className="flex min-h-[22rem] items-center justify-center bg-black text-sm text-white/70">
        This learning item is not playable media.
      </div>
    )
  }

  return (
    <TooltipProvider delayDuration={150}>
      <div className="w-full min-w-0 overflow-hidden rounded-lg border border-border bg-black text-white shadow-[var(--shadow-soft)]">
      <div
        ref={surfaceRef}
        className="relative flex aspect-video min-h-[12rem] w-full items-center justify-center bg-black sm:min-h-[18rem]"
        data-native-video-surface=""
      >
        <div className="pointer-events-none absolute inset-0 flex items-center justify-center">
          <div className="flex flex-col items-center gap-3 text-center text-white/80">
            <div className="flex size-14 items-center justify-center rounded-full border border-white/15 bg-white/5">
              {state.paused ? <Play className="size-6" /> : <Pause className="size-6" />}
            </div>
            <div className="max-w-xl px-6">
              <p className="line-clamp-1 text-sm font-medium text-white">{lesson.name}</p>
            </div>
          </div>
        </div>
        {error?.path === lesson.path && (
          <div className="absolute inset-x-6 bottom-6 rounded-lg border border-destructive/40 bg-destructive/15 px-3 py-2 text-xs text-destructive-foreground">
            {error.message}
          </div>
        )}
      </div>

      <div className="border-t border-white/10 bg-black/95 px-4 py-3">
        <div className="flex flex-col gap-3 sm:flex-row sm:items-center">
          <div className="flex items-center gap-3">
            <PlayerIconButton label={state.paused ? "Play" : "Pause"} onClick={togglePlayback}>
              {state.paused ? <Play /> : <Pause />}
            </PlayerIconButton>
            <div className="min-w-[6.5rem] text-xs tabular-nums text-white/70">{formattedPosition}</div>
          </div>
          <Slider
            aria-label="Seek"
            className="w-full min-w-0 sm:min-w-[9rem] sm:flex-1"
            max={Math.max(state.duration, 1)}
            min={0}
            onValueChange={changeSeek}
            onValueCommit={commitSeek}
            step={0.1}
            value={[Math.min(state.currentTime, Math.max(state.duration, 1))]}
          />
          <div className="flex flex-wrap items-center gap-3">
            <PlayerIconButton label={state.muted ? "Unmute" : "Mute"} onClick={toggleMute}>
              {state.muted ? <VolumeX /> : <Volume2 />}
            </PlayerIconButton>
            <Slider
              aria-label="Volume"
              className="hidden w-24 md:flex"
              max={1}
              min={0}
              onValueChange={changeVolume}
              step={0.01}
              value={[state.muted ? 0 : state.volume]}
            />
            <PlayerMenu
              state={state}
              onRateChange={changeRate}
            />
            <PlayerIconButton label="Step frame" onClick={() => void stepNativePlayerFrame().catch((reason) => setError({ path: lesson.path, message: String(reason) }))}>
              <SlidersHorizontal />
            </PlayerIconButton>
            <PlayerIconButton label="Screenshot" onClick={() => void takeNativePlayerScreenshot().catch((reason) => setError({ path: lesson.path, message: String(reason) }))}>
              <Camera />
            </PlayerIconButton>
            <PlayerIconButton label={isFullscreen ? "Exit fullscreen" : "Fullscreen"} onClick={toggleFullscreen}>
              <Maximize />
            </PlayerIconButton>
            {onNext && (
              <PlayerIconButton label="Next item" onClick={onNext}>
                <SkipForward />
              </PlayerIconButton>
            )}
          </div>
        </div>
      </div>
    </div>
    </TooltipProvider>
  )
}

function PlayerIconButton({
  label,
  onClick,
  children,
}: {
  label: string
  onClick: () => void
  children: ReactNode
}) {
  return (
    <Tooltip>
      <TooltipTrigger asChild>
        <Button
          variant="ghost"
          size="icon-sm"
          className="text-white hover:bg-white/10 hover:text-white"
          onClick={onClick}
          aria-label={label}
        >
          {children}
        </Button>
      </TooltipTrigger>
      <TooltipContent>{label}</TooltipContent>
    </Tooltip>
  )
}

function PlayerMenu({
  state,
  onRateChange,
}: {
  state: NativePlayerState
  onRateChange: (rate: number) => void
}) {
  return (
    <DropdownMenu>
      <DropdownMenuTrigger asChild>
        <Button variant="ghost" size="sm" className="min-w-16 text-white hover:bg-white/10 hover:text-white">
          {state.rate}x
        </Button>
      </DropdownMenuTrigger>
      <DropdownMenuContent align="end" className="w-48">
        <DropdownMenuLabel>Speed</DropdownMenuLabel>
        <DropdownMenuRadioGroup value={String(state.rate)} onValueChange={(value) => onRateChange(Number(value))}>
          {PLAYBACK_RATES.map((rate) => (
            <DropdownMenuRadioItem key={rate} value={String(rate)}>
              {rate}x
            </DropdownMenuRadioItem>
          ))}
        </DropdownMenuRadioGroup>
        <DropdownMenuSeparator />
        <DropdownMenuLabel className="flex items-center gap-2">
          <Captions className="size-4" />
          Captions
        </DropdownMenuLabel>
        <DropdownMenuItem disabled>
          {state.subtitleTracks.length === 0 ? "No subtitle tracks" : "Subtitle track selection"}
        </DropdownMenuItem>
        <DropdownMenuSeparator />
        <DropdownMenuLabel>Audio</DropdownMenuLabel>
        <DropdownMenuItem disabled>
          {state.audioTracks.length === 0 ? "Default audio" : "Audio track selection"}
        </DropdownMenuItem>
      </DropdownMenuContent>
    </DropdownMenu>
  )
}

export const VideoPlayer = memo(VideoPlayerComponent)
