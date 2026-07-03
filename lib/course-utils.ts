import type { Course, Section, Lesson } from "@/types"
import type { ScanResult, CourseData, SectionData, FileEntry } from "@/lib/tauri"

function getFileStem(value: string): string {
  const basename = value.replace(/\\/g, "/").split("/").pop() ?? value
  return basename.replace(/\.[^.]+$/, "")
}

const LANGUAGE_SUFFIXES: Record<string, string> = {
  english: "en",
  en: "en",
  spanish: "es",
  es: "es",
  french: "fr",
  fr: "fr",
  german: "de",
  de: "de",
  portuguese: "pt",
  pt: "pt",
  italian: "it",
  it: "it",
}

function mapFileType(type: FileEntry["file_type"]): Lesson["type"] {
  switch (type) {
    case "video":
      return "video"
    case "audio":
      return "audio"
    case "document":
      return "document"
    case "quiz":
      return "quiz"
    default:
      return "document"
  }
}

function getSubtitleLanguage(subtitleName: string, mediaStem: string): string | null {
  const subtitleStem = getFileStem(subtitleName)
  if (subtitleStem === mediaStem) return "default"

  const suffix = subtitleStem.slice(mediaStem.length)
  if (suffix.startsWith(".") || suffix.startsWith("_")) {
    return suffix.slice(1).trim() || null
  }

  if (suffix.startsWith(" ")) {
    const languageName = suffix.trim().toLowerCase()
    return LANGUAGE_SUFFIXES[languageName] ?? null
  }

  return null
}

function isSubtitleForMedia(subtitleName: string, mediaStem: string): boolean {
  return getSubtitleLanguage(subtitleName, mediaStem) !== null
}

function extractSubtitles(files: FileEntry[], videoPath: string): Lesson["subtitles"] {
  const videoBasename = getFileStem(videoPath)

  return files
    .filter((f) => f.file_type === "subtitle")
    .filter((f) => isSubtitleForMedia(f.name, videoBasename))
    .map((f) => {
      const lang = getSubtitleLanguage(f.name, videoBasename) ?? "default"
      return {
        path: f.path,
        language: lang,
        label: lang,
      }
    })
}

function sectionDataToSection(
  data: SectionData,
  courseId: string
): Section {
  const lessons: Lesson[] = data.files
    .filter((f) => f.file_type !== "subtitle")
    .sort((a, b) => a.name.localeCompare(b.name, undefined, { numeric: true }))
    .map((file, index) => ({
      id: file.id,
      courseId,
      sectionName: data.name,
      name: file.name.replace(/\.[^.]+$/, ""),
      path: file.path,
      type: mapFileType(file.file_type),
      duration: 0,
      fileSize: file.size,
      completed: false,
      watchedTime: 0,
      lastPosition: 0,
      order: index,
      subtitles: extractSubtitles(data.files, file.path),
    }))

  return {
    id: data.id,
    name: data.name,
    lessons,
    order: data.order,
  }
}

function courseDataToCourse(data: CourseData): Course {
  const courseId = data.id

  const sections = [...data.sections]
    .sort((a, b) => a.order - b.order || a.name.localeCompare(b.name, undefined, { numeric: true }))
    .map((section) => sectionDataToSection(section, courseId))

  return {
    id: courseId,
    name: data.name,
    path: data.path,
    sections,
    progress: 0,
    totalDuration: 0,
    watchedDuration: 0,
    lastAccessed: null,
    thumbnail: null,
    thumbnailSourcePath: sections.flatMap((section) => section.lessons).find((lesson) => lesson.type === "video")?.path ?? null,
  }
}

export function processScanResult(result: ScanResult): Course[] {
  return [...result.courses]
    .sort((a, b) => a.name.localeCompare(b.name, undefined, { numeric: true }))
    .map(courseDataToCourse)
}
