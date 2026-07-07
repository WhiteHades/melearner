"use client"

import { memo, useCallback, useEffect, useMemo, useRef, useState, type ReactNode } from "react"
import { getCurrentWindow } from "@tauri-apps/api/window"
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
  selectNativePlayerAudioTrack,
  selectNativePlayerChapter,
  selectNativePlayerSubtitleTrack,
  setNativePlayerBounds,
  setNativePlayerMuted,
  setNativePlayerRate,
  setNativePlayerSurfaceVisible,
  setNativePlayerVolume,
  stepNativePlayerFrame,
  subscribeNativePlayerEvents,
  takeNativePlayerScreenshot,
  type NativePlayerBounds,
  type NativePlayerState,
} from "@/lib/native-player"
import { frontendLog } from "@/lib/frontend-log"
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

function isEditableShortcutTarget(target: EventTarget | null) {
  if (!(target instanceof HTMLElement)) return false
  const tagName = target.tagName.toLowerCase()
  return target.isContentEditable || tagName === "input" || tagName === "textarea" || tagName === "select"
}

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
  surfaceAttached: false,
  surfaceBackend: null,
  surfaceRenderApi: false,
  surfaceRenderThreadAlive: false,
  surfaceRenderedFrames: 0,
  surfaceRenderWidth: null,
  surfaceRenderHeight: null,
  surfaceRenderUpdateFlags: 0,
  surfaceRenderError: null,
  audioTracks: [],
  subtitleTracks: [],
  selectedAudioTrackId: null,
  selectedSubtitleTrackId: null,
  chapters: [],
  currentChapterId: null,
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
  const boundsTimerRef = useRef<number | null>(null)
  const positionRafRef = useRef<number | null>(null)
  const isSeekingRef = useRef(false)
  const [nativeState, setNativeState] = useState<NativePlayerState>(initialState)
  const [visibleCurrentTime, setVisibleCurrentTime] = useState(lesson.lastPosition)
  const [error, setError] = useState<{ path: string; message: string } | null>(null)
  const [isFullscreen, setIsFullscreen] = useState(false)
  const [loadRequested, setLoadRequested] = useState(true)
  const autoplayNextLoadRef = useRef(autoplay)
  const [loadSnapshot] = useState(() => ({
    id: lesson.id,
    path: lesson.path,
    lastPosition: lesson.lastPosition,
    subtitles: lesson.subtitles,
  }))
  const isPlayable = lesson.type === "video" || lesson.type === "audio"
  const fallbackState = useMemo<NativePlayerState>(() => ({
    ...initialState,
    path: lesson.path,
    paused: !autoplay,
    currentTime: lesson.lastPosition,
    duration: lesson.duration,
  }), [autoplay, lesson.duration, lesson.lastPosition, lesson.path])
  const state = nativeState.path === lesson.path ? nativeState : fallbackState
  const isLoaded = nativeState.path === lesson.path
  const displayCurrentTime = state.duration > 0
    ? Math.min(Math.max(visibleCurrentTime, 0), state.duration)
    : Math.max(visibleCurrentTime, 0)

  const measureBounds = useCallback((): NativePlayerBounds | null => {
    const surface = surfaceRef.current
    if (!surface || !isTauri()) return null
    const rect = surface.getBoundingClientRect()
    return {
      x: Math.round(rect.left),
      y: Math.round(rect.top),
      width: Math.round(rect.width),
      height: Math.round(rect.height),
      scaleFactor: window.devicePixelRatio || 1,
    }
  }, [])

  const updateBounds = useCallback(async () => {
    const bounds = measureBounds()
    if (!bounds) return
    await setNativePlayerBounds(bounds)
  }, [measureBounds])

  const requestNativeSurfaceSync = useCallback(() => {
    if (!isTauri() || boundsTimerRef.current !== null) return

    boundsTimerRef.current = window.setTimeout(() => {
      boundsTimerRef.current = null
      void updateBounds().catch((reason) => setError({ path: lesson.path, message: String(reason) }))
    }, 0)
  }, [lesson.path, updateBounds])

  const syncWindowFullscreenState = useCallback(() => {
    if (!isTauri()) return
    void getCurrentWindow()
      .isFullscreen()
      .then(setIsFullscreen)
      .catch(() => undefined)
  }, [])

  useEffect(() => {
    if (!loadRequested || !isPlayable || !isTauri()) return
    let isActive = true

    void (async () => {
      const shouldAutoplay = autoplayNextLoadRef.current || autoplay
      autoplayNextLoadRef.current = false
      frontendLog("info", "native.player.load.start", {
        path: loadSnapshot.path,
        lessonId: loadSnapshot.id,
        autoplay: shouldAutoplay,
      })
      await updateBounds()
      const next = await loadNativePlayerFile({
        path: loadSnapshot.path,
        allowedRoots: [libraryRoot],
        subtitles: loadSnapshot.subtitles.map((subtitle) => ({
          path: subtitle.path,
          label: subtitle.label,
          language: subtitle.language,
        })),
        startTime: loadSnapshot.lastPosition || undefined,
        autoplay: shouldAutoplay,
      })

      if (!isActive) return
      frontendLog("info", "native.player.load.ready", {
        path: loadSnapshot.path,
        surfaceAttached: next.surfaceAttached,
        surfaceBackend: next.surfaceBackend,
        surfaceRenderApi: next.surfaceRenderApi,
        surfaceRenderThreadAlive: next.surfaceRenderThreadAlive,
        surfaceRenderedFrames: next.surfaceRenderedFrames,
        surfaceRenderWidth: next.surfaceRenderWidth,
        surfaceRenderHeight: next.surfaceRenderHeight,
        surfaceRenderUpdateFlags: next.surfaceRenderUpdateFlags,
        surfaceRenderError: next.surfaceRenderError,
        audioTracks: next.audioTracks.length,
        subtitleTracks: next.subtitleTracks.length,
        chapters: next.chapters.length,
      })
      setError(null)
      setNativeState(next)
    })()
      .catch((reason) => {
        if (isActive) {
          frontendLog("error", "native.player.load.failed", {
            path: loadSnapshot.path,
            error: String(reason),
          })
          setError({ path: loadSnapshot.path, message: String(reason) })
        }
      })

    return () => {
      isActive = false
      if (isTauri()) void destroyNativePlayer().catch(() => undefined)
    }
  }, [autoplay, isPlayable, libraryRoot, loadRequested, loadSnapshot, updateBounds])

  useEffect(() => {
    isSeekingRef.current = false
  }, [lesson.id])

  useEffect(() => {
    const frame = window.requestAnimationFrame(() => {
      if (!isSeekingRef.current) setVisibleCurrentTime(state.currentTime)
    })
    return () => window.cancelAnimationFrame(frame)
  }, [state.currentTime, state.path])

  useEffect(() => {
    if (positionRafRef.current !== null) {
      window.cancelAnimationFrame(positionRafRef.current)
      positionRafRef.current = null
    }

    if (!isPlayable || state.path !== lesson.path || state.paused || state.duration <= 0 || isSeekingRef.current) return

    const startedAt = performance.now()
    const startedPosition = state.currentTime
    const tick = (now: number) => {
      if (isSeekingRef.current) {
        positionRafRef.current = null
        return
      }
      const elapsedSeconds = ((now - startedAt) / 1000) * state.rate
      setVisibleCurrentTime(Math.min(state.duration, startedPosition + elapsedSeconds))
      positionRafRef.current = window.requestAnimationFrame(tick)
    }

    positionRafRef.current = window.requestAnimationFrame(tick)
    return () => {
      if (positionRafRef.current !== null) {
        window.cancelAnimationFrame(positionRafRef.current)
        positionRafRef.current = null
      }
    }
  }, [isPlayable, lesson.path, state.currentTime, state.duration, state.path, state.paused, state.rate])

  useEffect(() => {
    const surface = surfaceRef.current
    if (!surface || !isTauri()) return
    let disposed = false
    let unlistenMoved: (() => void) | null = null
    let unlistenResized: (() => void) | null = null
    let visibilityObserver: IntersectionObserver | null = null

    const requestBoundsUpdate = () => requestNativeSurfaceSync()
    const requestBoundsAndFullscreenUpdate = () => {
      requestBoundsUpdate()
      syncWindowFullscreenState()
    }
    requestBoundsUpdate()
    const observer = new ResizeObserver(requestBoundsUpdate)
    observer.observe(surface)
    window.addEventListener("resize", requestBoundsAndFullscreenUpdate)
    window.addEventListener("scroll", requestBoundsUpdate, true)

    if ("IntersectionObserver" in window) {
      visibilityObserver = new IntersectionObserver(([intersection]) => {
        const visible = intersection ? intersection.isIntersecting : false
        void setNativePlayerSurfaceVisible(visible).catch((reason) => {
          if (!disposed) setError({ path: lesson.path, message: String(reason) })
        })
        if (visible) requestBoundsUpdate()
      }, { threshold: 0.01 })
      visibilityObserver.observe(surface)
    }

    const appWindow = getCurrentWindow()
    syncWindowFullscreenState()
    void appWindow.onMoved(requestBoundsUpdate).then((unlisten) => {
      if (disposed) {
        unlisten()
      } else {
        unlistenMoved = unlisten
      }
    })
    void appWindow.onResized(requestBoundsAndFullscreenUpdate).then((unlisten) => {
      if (disposed) {
        unlisten()
      } else {
        unlistenResized = unlisten
      }
    })

    return () => {
      disposed = true
      if (boundsTimerRef.current !== null) {
        window.clearTimeout(boundsTimerRef.current)
        boundsTimerRef.current = null
      }
      observer.disconnect()
      visibilityObserver?.disconnect()
      window.removeEventListener("resize", requestBoundsAndFullscreenUpdate)
      window.removeEventListener("scroll", requestBoundsUpdate, true)
      unlistenMoved?.()
      unlistenResized?.()
    }
  }, [lesson.path, requestNativeSurfaceSync, syncWindowFullscreenState])

  useEffect(() => {
    if (!isPlayable || !isTauri()) return
    let cancelled = false
    let unsubscribe: (() => void) | null = null

    void subscribeNativePlayerEvents({
      onState: (next) => {
        if (!cancelled && (next.path === lesson.path || next.path === null)) setNativeState(next)
      },
      onTracks: (next) => {
        if (cancelled) return
        setNativeState((current) => {
          if (current.path !== lesson.path) return current
          return {
            ...current,
            audioTracks: next.audioTracks,
            subtitleTracks: next.subtitleTracks,
            selectedAudioTrackId: next.selectedAudioTrackId,
            selectedSubtitleTrackId: next.selectedSubtitleTrackId,
          }
        })
      },
      onChapters: (next) => {
        if (cancelled) return
        setNativeState((current) => {
          if (current.path !== lesson.path) return current
          return {
            ...current,
            chapters: next.chapters,
            currentChapterId: next.currentChapterId,
          }
        })
      },
      onPosition: (next) => {
        if (cancelled || next.path !== lesson.path) return
        setNativeState((current) => {
          if (current.path !== lesson.path) return current
          return {
            ...current,
            paused: next.paused,
            buffering: next.buffering,
            currentTime: next.currentTime,
            duration: next.duration,
            volume: next.volume,
            muted: next.muted,
            rate: next.rate,
            width: next.width,
            height: next.height,
            surfaceRenderThreadAlive: next.surfaceRenderThreadAlive,
            surfaceRenderedFrames: next.surfaceRenderedFrames,
            surfaceRenderWidth: next.surfaceRenderWidth,
            surfaceRenderHeight: next.surfaceRenderHeight,
            surfaceRenderUpdateFlags: next.surfaceRenderUpdateFlags,
            surfaceRenderError: next.surfaceRenderError,
            currentChapterId: next.currentChapterId,
          }
        })
      },
      onFileLoaded: (next) => {
        if (!cancelled && next.path === lesson.path) setError(null)
      },
      onEnd: (next) => {
        if (!cancelled && next.path === lesson.path) onComplete()
      },
      onError: (message) => {
        if (!cancelled) setError({ path: lesson.path, message })
      },
    })
      .then((nextUnsubscribe) => {
        if (cancelled) {
          nextUnsubscribe()
        } else {
          unsubscribe = nextUnsubscribe
        }
      })
      .catch((reason) => {
        if (!cancelled) setError({ path: lesson.path, message: String(reason) })
      })

    void getNativePlayerState()
      .then((next) => {
        if (!cancelled && next.path === lesson.path) setNativeState(next)
      })
      .catch((reason) => {
        if (!cancelled) setError({ path: lesson.path, message: String(reason) })
      })

    return () => {
      cancelled = true
      unsubscribe?.()
    }
  }, [isPlayable, lesson.path, onComplete])

  useEffect(() => {
    const now = Date.now()
    const shouldSave = now - lastSaveRef.current >= POSITION_SAVE_MS || state.currentTime >= state.duration - 1
    if (!isPlayable || !shouldSave) return
    lastSaveRef.current = now
    onProgress(state.currentTime, state.duration)
    if (state.duration > 0 && state.currentTime >= state.duration - 1) onComplete()
  }, [isPlayable, onComplete, onProgress, state.currentTime, state.duration])

  const formattedPosition = useMemo(() => {
    return `${formatDuration(displayCurrentTime)} / ${formatDuration(state.duration)}`
  }, [displayCurrentTime, state.duration])

  const togglePlayback = useCallback(() => {
    if (!isLoaded) {
      autoplayNextLoadRef.current = true
      setLoadRequested(true)
      return
    }
    const nextPaused = !state.paused
    setNativeState((current) => ({ ...(current.path === lesson.path ? current : fallbackState), paused: nextPaused }))
    const action = nextPaused ? pauseNativePlayer : playNativePlayer
    void action().catch((reason) => setError({ path: lesson.path, message: String(reason) }))
  }, [fallbackState, isLoaded, lesson.path, state.paused])

  const changeSeek = useCallback((value: number[]) => {
    const currentTime = value[0] ?? 0
    isSeekingRef.current = true
    setVisibleCurrentTime(currentTime)
    setNativeState((current) => ({ ...(current.path === lesson.path ? current : fallbackState), currentTime }))
  }, [fallbackState, lesson.path])

  const commitSeek = useCallback((value: number[]) => {
    if (!isLoaded) return
    const currentTime = value[0] ?? 0
    isSeekingRef.current = true
    setVisibleCurrentTime(currentTime)
    setNativeState((current) => ({ ...(current.path === lesson.path ? current : fallbackState), currentTime }))
    void seekNativePlayer({ seconds: currentTime, mode: "absolute" })
      .then((next) => {
        isSeekingRef.current = false
        setNativeState(next)
        setVisibleCurrentTime(next.currentTime)
      })
      .catch((reason) => {
        isSeekingRef.current = false
        setError({ path: lesson.path, message: String(reason) })
      })
    onProgress(currentTime, state.duration)
  }, [fallbackState, isLoaded, lesson.path, onProgress, state.duration])

  const changeVolume = useCallback((value: number[]) => {
    if (!isLoaded) return
    const volume = value[0] ?? 0
    setNativeState((current) => ({ ...(current.path === lesson.path ? current : fallbackState), volume, muted: volume === 0 }))
    void setNativePlayerVolume(volume).catch((reason) => setError({ path: lesson.path, message: String(reason) }))
  }, [fallbackState, isLoaded, lesson.path])

  const toggleMute = useCallback(() => {
    if (!isLoaded) return
    const muted = !state.muted
    setNativeState((current) => ({ ...(current.path === lesson.path ? current : fallbackState), muted }))
    void setNativePlayerMuted(muted).catch((reason) => setError({ path: lesson.path, message: String(reason) }))
  }, [fallbackState, isLoaded, lesson.path, state.muted])

  const changeRate = useCallback((rate: number) => {
    if (!isLoaded) return
    setNativeState((current) => ({ ...(current.path === lesson.path ? current : fallbackState), rate }))
    void setNativePlayerRate(rate).catch((reason) => setError({ path: lesson.path, message: String(reason) }))
  }, [fallbackState, isLoaded, lesson.path])

  const applyNativeState = useCallback((action: () => Promise<NativePlayerState>) => {
    if (!isLoaded) return
    void action()
      .then(setNativeState)
      .catch((reason) => setError({ path: lesson.path, message: String(reason) }))
  }, [isLoaded, lesson.path])

  const changeAudioTrack = useCallback((id: string) => {
    applyNativeState(() => selectNativePlayerAudioTrack(id))
  }, [applyNativeState])

  const changeSubtitleTrack = useCallback((id: string | null) => {
    applyNativeState(() => selectNativePlayerSubtitleTrack(id))
  }, [applyNativeState])

  const changeChapter = useCallback((id: string) => {
    applyNativeState(() => selectNativePlayerChapter(id))
  }, [applyNativeState])

  const stepFrame = useCallback(() => {
    applyNativeState(stepNativePlayerFrame)
  }, [applyNativeState])

  const toggleFullscreen = useCallback(() => {
    if (!surfaceRef.current || !isTauri()) return
    const appWindow = getCurrentWindow()
    void appWindow
      .isFullscreen()
      .then(async (fullscreen) => {
        const nextFullscreen = !fullscreen
        await appWindow.setFullscreen(nextFullscreen)
        setIsFullscreen(nextFullscreen)
        requestNativeSurfaceSync()
      })
      .catch((reason) => setError({ path: lesson.path, message: String(reason) }))
  }, [lesson.path, requestNativeSurfaceSync])

  useEffect(() => {
    if (!isPlayable) return

    function handlePlayerKeyDown(event: KeyboardEvent) {
      if (event.defaultPrevented || event.metaKey || event.ctrlKey || event.altKey || event.shiftKey) return
      if (isEditableShortcutTarget(event.target)) return

      switch (event.code) {
        case "Space":
        case "KeyK":
          event.preventDefault()
          togglePlayback()
          break
        case "KeyM":
          event.preventDefault()
          toggleMute()
          break
        case "KeyF":
          event.preventDefault()
          toggleFullscreen()
          break
        case "KeyJ":
        case "ArrowLeft":
          event.preventDefault()
          applyNativeState(() => seekNativePlayer({ seconds: -10, mode: "relative" }))
          break
        case "KeyL":
        case "ArrowRight":
          event.preventDefault()
          applyNativeState(() => seekNativePlayer({ seconds: 10, mode: "relative" }))
          break
      }
    }

    document.addEventListener("keydown", handlePlayerKeyDown)
    return () => document.removeEventListener("keydown", handlePlayerKeyDown)
  }, [applyNativeState, isPlayable, toggleFullscreen, toggleMute, togglePlayback])

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
            value={[Math.min(displayCurrentTime, Math.max(state.duration, 1))]}
            disabled={!isLoaded}
          />
          <div className="flex flex-wrap items-center gap-3">
            <PlayerIconButton label={state.muted ? "Unmute" : "Mute"} onClick={toggleMute} disabled={!isLoaded}>
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
              disabled={!isLoaded}
            />
            <PlayerMenu
              state={state}
              onRateChange={changeRate}
              onAudioTrackChange={changeAudioTrack}
              onSubtitleTrackChange={changeSubtitleTrack}
              onChapterChange={changeChapter}
              disabled={!isLoaded}
            />
            <PlayerIconButton label="Step frame" disabled={!isLoaded} onClick={stepFrame}>
              <SlidersHorizontal />
            </PlayerIconButton>
            <PlayerIconButton label="Screenshot" disabled={!isLoaded} onClick={() => {
              if (!isLoaded) return
              void takeNativePlayerScreenshot().catch((reason) => setError({ path: lesson.path, message: String(reason) }))
            }}>
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
  disabled = false,
  children,
}: {
  label: string
  onClick: () => void
  disabled?: boolean
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
          disabled={disabled}
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
  onAudioTrackChange,
  onSubtitleTrackChange,
  onChapterChange,
  disabled,
}: {
  state: NativePlayerState
  onRateChange: (rate: number) => void
  onAudioTrackChange: (id: string) => void
  onSubtitleTrackChange: (id: string | null) => void
  onChapterChange: (id: string) => void
  disabled: boolean
}) {
  return (
    <DropdownMenu>
      <DropdownMenuTrigger asChild>
        <Button variant="ghost" size="sm" className="min-w-16 text-white hover:bg-white/10 hover:text-white" disabled={disabled}>
          {state.rate}x
        </Button>
      </DropdownMenuTrigger>
      <DropdownMenuContent
        align="end"
        collisionPadding={8}
        className="max-h-[calc(var(--radix-dropdown-menu-content-available-height)-8px)] w-48"
      >
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
        <DropdownMenuRadioGroup
          value={state.selectedSubtitleTrackId ?? "off"}
          onValueChange={(value) => onSubtitleTrackChange(value === "off" ? null : value)}
        >
          <DropdownMenuRadioItem value="off">Off</DropdownMenuRadioItem>
          {state.subtitleTracks.map((track, index) => (
            <DropdownMenuRadioItem key={track.id} value={track.id}>
              {formatTrackLabel(track, index, "Subtitle")}
            </DropdownMenuRadioItem>
          ))}
        </DropdownMenuRadioGroup>
        <DropdownMenuSeparator />
        <DropdownMenuLabel>Audio</DropdownMenuLabel>
        {state.audioTracks.length === 0 ? (
          <DropdownMenuItem disabled>Default audio</DropdownMenuItem>
        ) : (
          <DropdownMenuRadioGroup
            value={state.selectedAudioTrackId ?? state.audioTracks[0]?.id}
            onValueChange={onAudioTrackChange}
          >
            {state.audioTracks.map((track, index) => (
              <DropdownMenuRadioItem key={track.id} value={track.id}>
                {formatTrackLabel(track, index, "Audio")}
              </DropdownMenuRadioItem>
            ))}
          </DropdownMenuRadioGroup>
        )}
        {state.chapters.length > 0 && (
          <>
            <DropdownMenuSeparator />
            <DropdownMenuLabel>Chapters</DropdownMenuLabel>
            {state.chapters.map((chapter, index) => (
              <DropdownMenuItem key={chapter.id} onSelect={() => onChapterChange(chapter.id)}>
                {formatChapterLabel(chapter, index)}
              </DropdownMenuItem>
            ))}
          </>
        )}
      </DropdownMenuContent>
    </DropdownMenu>
  )
}

function formatTrackLabel(
  track: NativePlayerState["audioTracks"][number],
  index: number,
  fallback: string,
) {
  if (track.title) return track.title
  if (track.language) return `${fallback} ${index + 1} (${track.language})`
  return `${fallback} ${index + 1}`
}

function formatChapterLabel(
  chapter: NativePlayerState["chapters"][number],
  index: number,
) {
  return `${formatDuration(chapter.startTime)} ${chapter.title ?? `Chapter ${index + 1}`}`
}

export const VideoPlayer = memo(VideoPlayerComponent)
