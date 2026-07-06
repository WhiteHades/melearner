"use client"

import { invoke } from "@tauri-apps/api/core"

export type NativeTrack = {
  id: string
  title: string | null
  language: string | null
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
  audioTracks: NativeTrack[]
  subtitleTracks: NativeTrack[]
  selectedAudioTrackId: string | null
  selectedSubtitleTrackId: string | null
}

export type NativePlayerBounds = {
  x: number
  y: number
  width: number
  height: number
  scaleFactor: number
}

export type NativePlayerLoadOptions = {
  path: string
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

export function setNativePlayerBounds(bounds: NativePlayerBounds): Promise<void> {
  return invoke<void>("native_player_set_bounds", { bounds })
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
