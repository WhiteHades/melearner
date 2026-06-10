import type { Course, Section, Lesson } from "@/types"
import type { ScanResult, CourseData, SectionData, FileEntry } from "@/lib/tauri"

function getFileStem(value: string): string {
  const basename = value.replace(/\\/g, "/").split("/").pop() ?? value
  return basename.replace(/\.[^.]+$/, "")
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

function extractSubtitles(files: FileEntry[], videoPath: string): Lesson["subtitles"] {
  const videoBasename = getFileStem(videoPath)

  return files
    .filter((f) => f.file_type === "subtitle")
    .filter((f) => {
      const subtitleBasename = getFileStem(f.name)
      return (
        subtitleBasename === videoBasename ||
        subtitleBasename.startsWith(videoBasename + ".")
      )
    })
    .map((f) => {
      const parts = f.name.split(".")
      const lang = parts.length > 2 ? parts[parts.length - 2] : "default"
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
  }
}

export function processScanResult(result: ScanResult): Course[] {
  return [...result.courses]
    .sort((a, b) => a.name.localeCompare(b.name, undefined, { numeric: true }))
    .map(courseDataToCourse)
}
