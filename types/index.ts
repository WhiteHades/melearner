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
