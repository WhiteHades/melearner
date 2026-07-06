export interface Course {
  id: string
  identityId: string
  markerIdentityId: string | null
  name: string
  path: string
  fingerprint: string | null
  missingSince: string | null
  sections: Section[]
  progress: number
  totalDuration: number
  watchedDuration: number
  lastAccessed: string | null
  thumbnail: string | null
  thumbnailSourcePath: string | null
}

export interface Section {
  id: string
  name: string
  lessons: Lesson[]
  order: number
}

export interface Lesson {
  id: string
  courseId: string
  sectionName: string
  name: string
  path: string
  relativePath: string | null
  type: "video" | "audio" | "document" | "quiz"
  duration: number
  fileSize: number
  completed: boolean
  watchedTime: number
  lastPosition: number
  order: number
  subtitles: SubtitleFile[]
}

export interface SubtitleFile {
  path: string
  language: string
  label: string
}

export interface Note {
  id: string
  lessonId: string
  timestamp: number
  text: string
  createdAt: string
}

export interface ActivityDay {
  date: string
  watchedSeconds: number
  lessonsTouched: number
  completions: number
}

export type FileType = "video" | "audio" | "document" | "subtitle" | "quiz" | "unknown"

export const VIDEO_EXTENSIONS = [".mp4", ".mkv", ".webm", ".mov", ".avi", ".m4v"]
export const AUDIO_EXTENSIONS = [".mp3", ".wav", ".aac", ".m4a", ".flac", ".ogg"]
export const DOCUMENT_EXTENSIONS = [".pdf", ".txt", ".md", ".markdown", ".html", ".htm", ".docx", ".doc"]
export const SUBTITLE_EXTENSIONS = [".srt", ".vtt"]
export const PARTIAL_EXTENSIONS = [".part", ".crdownload", ".download"]
export const IGNORED_FOLDERS = [".git", "node_modules", "__MACOSX", ".DS_Store", "Thumbs.db"]
