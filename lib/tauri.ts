import { invoke, isTauri as tauriIsTauri } from "@tauri-apps/api/core"
import { open } from "@tauri-apps/plugin-dialog"

export interface FileEntry {
  id: string
  path: string
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
  name: string
  path: string
  sections: SectionData[]
}

export interface ScanResult {
  scan_type: "library" | "singlecourse" | "bundle"
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
    title: "select course folder",
  })
  return result as string | null
}

export function isTauri(): boolean {
  return tauriIsTauri()
}
