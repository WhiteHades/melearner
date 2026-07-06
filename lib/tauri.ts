import { invoke, isTauri as tauriIsTauri } from "@tauri-apps/api/core"
import { open } from "@tauri-apps/plugin-dialog"

export interface FileEntry {
  id: string
  path: string
  relative_path: string
  name: string
  file_type: "video" | "audio" | "document" | "subtitle" | "quiz" | "unknown"
  size: number
}

export interface SectionData {
  id: string
  name: string
  files: FileEntry[]
  order: number
}

export interface CourseData {
  id: string
  marker_identity_id: string | null
  name: string
  path: string
  fingerprint: string
  sections: SectionData[]
}

export interface ScanResult {
  scan_type: "library" | "singlecourse"
  courses: CourseData[]
  warnings: string[]
}

export async function scanFolder(path: string): Promise<ScanResult> {
  return invoke<ScanResult>("scan_folder", { path })
}

export async function selectFolderDialog(): Promise<string | null> {
  const result = await open({
    directory: true,
    multiple: false,
    title: "select root folder",
  })
  return result as string | null
}

export function isTauri(): boolean {
  return tauriIsTauri()
}

export interface BuildInfo {
  version: string
  git_sha: string
  git_sha_long: string
  build_timestamp: string
  rust_version: string
}

export async function getBuildInfo(): Promise<BuildInfo> {
  return invoke<BuildInfo>("get_build_info")
}

export async function getDatabasePath(): Promise<string> {
  return invoke<string>("get_database_path")
}

export async function openNativeFile(path: string): Promise<void> {
  return invoke<void>("open_native", { path })
}

export async function preparePlaybackMedia(path: string, mediaType: "video" | "audio"): Promise<{ path: string }> {
  return invoke<{ path: string }>("prepare_playback_media", { path, mediaType })
}

export async function cancelPlaybackMedia(path: string, mediaType: "video" | "audio"): Promise<void> {
  return invoke<void>("cancel_playback_media", { path, mediaType })
}

export async function writeCourseMarker(path: string, identityId: string): Promise<void> {
  return invoke<void>("write_course_marker", { path, identityId })
}
