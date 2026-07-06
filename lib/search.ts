import type { Course } from "@/types"
import {
  clearLibrarySearch,
  indexLibrarySearch,
  isTauri,
  searchLibrary,
} from "@/lib/tauri"

interface SearchDocument {
  id: string
  type: "lesson"
  name: string
  path: string
  courseId: string
  courseName: string
  sectionName: string
}

export interface SearchResult {
  id: string
  type: "lesson"
  name: string
  path: string
  courseId: string
  courseName: string
  sectionName: string
  score: number
}

const documentsByPath = new Map<string, SearchDocument>()

function normalizePath(path: string): string {
  return path.replace(/\\/g, "/")
}

function indexableLessons(courses: Course[]): SearchDocument[] {
  const documents: SearchDocument[] = []

  for (const course of courses) {
    if (course.missingSince) continue
    for (const section of course.sections) {
      for (const lesson of section.lessons) {
        documents.push({
          id: lesson.id,
          type: "lesson",
          name: lesson.name,
          path: lesson.path,
          courseId: course.id,
          courseName: course.name,
          sectionName: section.name,
        })
      }
    }
  }

  return documents
}

export async function indexCourses(courses: Course[], libraryPath: string | null = null): Promise<void> {
  documentsByPath.clear()

  const documents = indexableLessons(courses)
  for (const document of documents) {
    documentsByPath.set(normalizePath(document.path), document)
  }

  if (!isTauri()) return
  if (!libraryPath || documents.length === 0) {
    await clearLibrarySearch()
    return
  }

  await indexLibrarySearch(libraryPath, documents.map((document) => document.path))
}

export async function search(query: string, limit = 20): Promise<SearchResult[]> {
  if (!isTauri() || !query.trim()) return []

  const hits = await searchLibrary(query, limit)
  return hits.flatMap((hit) => {
    const document = documentsByPath.get(normalizePath(hit.path))
    if (!document) return []
    return [{
      ...document,
      score: hit.score,
    }]
  })
}
