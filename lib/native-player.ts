"use client"

import { invoke } from "@tauri-apps/api/core"
import { listen } from "@tauri-apps/api/event"

export type NativeTrack = {
  id: string
  title: string | null
  language: string | null
}

export type NativeChapter = {
  id: string
  title: string | null
  startTime: number
}

export type NativePlayerState = {
  path: string | null
  paused: boolean
  buffering: boolean
  currentTime: number
  duration: number
  volume: number
  muted: boolean
  rate: number
  width: number | null
  height: number | null
  surfaceAttached: boolean
  surfaceBackend: string | null
  surfaceRenderApi: boolean
  surfaceRenderThreadAlive: boolean
  surfaceRenderedFrames: number
  surfaceRenderError: string | null
  audioTracks: NativeTrack[]
  subtitleTracks: NativeTrack[]
  selectedAudioTrackId: string | null
  selectedSubtitleTrackId: string | null
  chapters: NativeChapter[]
  currentChapterId: string | null
}

export type NativePlayerTracksEvent = {
  audioTracks: NativeTrack[]
  subtitleTracks: NativeTrack[]
  selectedAudioTrackId: string | null
  selectedSubtitleTrackId: string | null
}

export type NativePlayerChaptersEvent = {
  chapters: NativeChapter[]
  currentChapterId: string | null
}

export type NativePlayerFileLoadedEvent = {
  path: string
}

export type NativePlayerPositionEvent = {
  path: string | null
  paused: boolean
  buffering: boolean
  currentTime: number
  duration: number
  volume: number
  muted: boolean
  rate: number
  width: number | null
  height: number | null
  surfaceRenderThreadAlive: boolean
  surfaceRenderedFrames: number
  surfaceRenderError: string | null
  currentChapterId: string | null
}

export type NativePlayerBounds = {
  x: number
  y: number
  width: number
  height: number
  scaleFactor: number
}

export type NativeSubtitleLoadOptions = {
  path: string
  label?: string
  language?: string
}

export type NativePlayerLoadOptions = {
  path: string
  allowedRoots: string[]
  subtitles?: NativeSubtitleLoadOptions[]
  startTime?: number
  autoplay?: boolean
}

export type NativePlayerSeekOptions = {
  seconds: number
  mode: "absolute" | "relative"
}

export function loadNativePlayerFile(options: NativePlayerLoadOptions): Promise<NativePlayerState> {
  return invoke<NativePlayerState>("native_player_load", { options })
}

export function getNativePlayerState(): Promise<NativePlayerState> {
  return invoke<NativePlayerState>("native_player_state")
}

export function playNativePlayer(): Promise<NativePlayerState> {
  return invoke<NativePlayerState>("native_player_play")
}

export function pauseNativePlayer(): Promise<NativePlayerState> {
  return invoke<NativePlayerState>("native_player_pause")
}

export function seekNativePlayer(options: NativePlayerSeekOptions): Promise<NativePlayerState> {
  return invoke<NativePlayerState>("native_player_seek", { options })
}

export function setNativePlayerVolume(volume: number): Promise<NativePlayerState> {
  return invoke<NativePlayerState>("native_player_set_volume", { volume })
}

export function setNativePlayerMuted(muted: boolean): Promise<NativePlayerState> {
  return invoke<NativePlayerState>("native_player_set_muted", { muted })
}

export function setNativePlayerRate(rate: number): Promise<NativePlayerState> {
  return invoke<NativePlayerState>("native_player_set_rate", { rate })
}

export function selectNativePlayerAudioTrack(id: string | null): Promise<NativePlayerState> {
  return invoke<NativePlayerState>("native_player_select_audio_track", { id })
}

export function selectNativePlayerSubtitleTrack(id: string | null): Promise<NativePlayerState> {
  return invoke<NativePlayerState>("native_player_select_subtitle_track", { id })
}

export function selectNativePlayerChapter(id: string): Promise<NativePlayerState> {
  return invoke<NativePlayerState>("native_player_select_chapter", { id })
}

export function setNativePlayerBounds(bounds: NativePlayerBounds): Promise<void> {
  return invoke<void>("native_player_set_bounds", { bounds })
}

export function setNativePlayerSurfaceVisible(visible: boolean): Promise<void> {
  return invoke<void>("native_player_set_surface_visible", { visible })
}

export function stepNativePlayerFrame(): Promise<NativePlayerState> {
  return invoke<NativePlayerState>("native_player_step_frame")
}

export function takeNativePlayerScreenshot(): Promise<string> {
  return invoke<string>("native_player_screenshot")
}

export function destroyNativePlayer(): Promise<void> {
  return invoke<void>("native_player_destroy")
}

export async function subscribeNativePlayerEvents({
  onState,
  onTracks,
  onChapters,
  onPosition,
  onFileLoaded,
  onEnd,
  onError,
}: {
  onState: (state: NativePlayerState) => void
  onTracks?: (tracks: NativePlayerTracksEvent) => void
  onChapters?: (chapters: NativePlayerChaptersEvent) => void
  onPosition?: (position: NativePlayerPositionEvent) => void
  onFileLoaded?: (file: NativePlayerFileLoadedEvent) => void
  onEnd: (state: NativePlayerState) => void
  onError: (message: string) => void
}): Promise<() => void> {
  const listeners = [
    listen<NativePlayerState>("native-player://state", (event) => onState(event.payload)),
    listen<NativePlayerTracksEvent>("native-player://tracks", (event) => onTracks?.(event.payload)),
    listen<NativePlayerChaptersEvent>("native-player://chapters", (event) => onChapters?.(event.payload)),
    listen<NativePlayerPositionEvent>("native-player://position", (event) => onPosition?.(event.payload)),
    listen<NativePlayerFileLoadedEvent>("native-player://file-loaded", (event) => onFileLoaded?.(event.payload)),
    listen<NativePlayerState>("native-player://end-file", (event) => onEnd(event.payload)),
    listen<{ message: string }>("native-player://error", (event) => onError(event.payload.message)),
  ]
  const unlisteners = await Promise.all(listeners)
  return () => {
    for (const unlisten of unlisteners) unlisten()
  }
}
