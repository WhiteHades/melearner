import Database from "@tauri-apps/plugin-sql"
import type { Course, Lesson, Note, Section, SubtitleFile } from "@/types"
import { isTauri, getDatabasePath } from "./tauri"

let db: Database | null = null
let dbPathPromise: Promise<string | null> | null = null
const SQLITE_BATCH_SIZE = 500
const LIBRARY_PATH_SETTING = "libraryPath"

type PersistedCourseRow = {
  id: string
  name: string
  path: string
  total_duration: number
  watched_duration: number
  last_accessed: string | null
  thumbnail_source_path: string | null
}

type PersistedSectionRow = {
  id: string
  course_id: string
  name: string
  order_index: number | null
}

type PersistedLessonRow = {
  id: string
  course_id: string
  section_id: string | null
  section_name: string | null
  name: string
  path: string
  type: Lesson["type"]
  duration: number
  file_size: number | null
  watched_time: number
  last_position: number
  completed: number
  order_index: number | null
}

type PersistedSubtitleRow = {
  lesson_id: string
  path: string
  language: string | null
  label: string | null
  order_index: number | null
}

type PersistedNoteRow = {
  id: string
  lesson_id: string
  timestamp: number
  text: string
  created_at: string
}

type PersistedSettingRow = {
  value: string | null
}

export type PersistedLibrary = {
  courses: Course[]
  libraryPath: string | null
}

async function resolveDatabasePath(): Promise<string | null> {
  if (!dbPathPromise) {
    dbPathPromise = (async () => {
      if (!isTauri()) return null
      try {
        return await getDatabasePath()
      } catch {
        return null
      }
    })()
  }
  return dbPathPromise
}

async function getDatabase(): Promise<Database | null> {
  if (!isTauri()) return null

  if (!db) {
    const path = await resolveDatabasePath()
    if (!path) return null
    db = await Database.load(path)
    await db.execute("PRAGMA foreign_keys = ON")
    await db.execute("PRAGMA journal_mode = WAL")
  }

  return db
}

function createPlaceholders(count: number): string {
  return Array.from({ length: count }, (_, index) => `$${index + 1}`).join(", ")
}

function trimTrailingSeparators(path: string): string {
  if (path === "/") return path
  return path.replace(/[\\/]+$/, "")
}

function escapeLikePattern(value: string): string {
  return value.replace(/[~%_]/g, (char) => `~${char}`)
}

function childPathPattern(path: string): string {
  const normalized = trimTrailingSeparators(path)
  const separator = normalized.includes("\\") ? "\\" : "/"
  const prefix = normalized.endsWith("/") || normalized.endsWith("\\") ? normalized : `${normalized}${separator}`
  return `${escapeLikePattern(prefix)}%`
}

async function executeTransaction<T>(database: Database, work: () => Promise<T>): Promise<T> {
  await database.execute("BEGIN")
  try {
    const result = await work()
    await database.execute("COMMIT")
    return result
  } catch (error) {
    await database.execute("ROLLBACK").catch(() => undefined)
    throw error
  }
}

function makeSubtitleId(lessonId: string, index: number): string {
  return `${lessonId}:subtitle:${index}`
}

async function selectPersistedCourses(paths: string[]): Promise<PersistedCourseRow[]> {
  const database = await getDatabase()
  if (!database || paths.length === 0) return []

  const rows: PersistedCourseRow[] = []

  for (let index = 0; index < paths.length; index += SQLITE_BATCH_SIZE) {
    const batch = paths.slice(index, index + SQLITE_BATCH_SIZE)
    const result = await database.select<PersistedCourseRow[]>(
      `SELECT id, name, path, total_duration, watched_duration, last_accessed, thumbnail_source_path
       FROM courses
       WHERE path IN (${createPlaceholders(batch.length)})`,
      batch
    )

    rows.push(...result)
  }

  return rows
}

async function selectPersistedLessons(paths: string[]): Promise<PersistedLessonRow[]> {
  const database = await getDatabase()
  if (!database || paths.length === 0) return []

  const rows: PersistedLessonRow[] = []

  for (let index = 0; index < paths.length; index += SQLITE_BATCH_SIZE) {
    const batch = paths.slice(index, index + SQLITE_BATCH_SIZE)
    const result = await database.select<PersistedLessonRow[]>(
      `SELECT id, course_id, section_id, section_name, name, path, type, duration, file_size,
              watched_time, last_position, completed, order_index
       FROM lessons
       WHERE path IN (${createPlaceholders(batch.length)})`,
      batch
    )

    rows.push(...result)
  }

  return rows
}

async function readSetting(key: string): Promise<string | null> {
  const database = await getDatabase()
  if (!database) return null

  const rows = await database.select<PersistedSettingRow[]>(
    `SELECT value FROM app_settings WHERE key = $1`,
    [key]
  )

  return rows[0]?.value ?? null
}

async function writeSetting(database: Database, key: string, value: string | null): Promise<void> {
  if (value === null) {
    await database.execute(`DELETE FROM app_settings WHERE key = $1`, [key])
    return
  }

  await database.execute(
    `INSERT INTO app_settings (key, value, updated_at)
     VALUES ($1, $2, CURRENT_TIMESTAMP)
     ON CONFLICT(key) DO UPDATE SET
       value = excluded.value,
       updated_at = excluded.updated_at`,
    [key, value]
  )
}

function rowsToCourses(
  courseRows: PersistedCourseRow[],
  sectionRows: PersistedSectionRow[],
  lessonRows: PersistedLessonRow[],
  subtitleRows: PersistedSubtitleRow[]
): Course[] {
  const sectionsByCourse = new Map<string, PersistedSectionRow[]>()
  for (const section of sectionRows) {
    const list = sectionsByCourse.get(section.course_id) ?? []
    list.push(section)
    sectionsByCourse.set(section.course_id, list)
  }

  const lessonsByCourse = new Map<string, PersistedLessonRow[]>()
  for (const lesson of lessonRows) {
    const list = lessonsByCourse.get(lesson.course_id) ?? []
    list.push(lesson)
    lessonsByCourse.set(lesson.course_id, list)
  }

  const subtitlesByLesson = new Map<string, SubtitleFile[]>()
  for (const subtitle of subtitleRows) {
    const list = subtitlesByLesson.get(subtitle.lesson_id) ?? []
    list.push({
      path: subtitle.path,
      language: subtitle.language ?? "default",
      label: subtitle.label ?? subtitle.language ?? "default",
    })
    subtitlesByLesson.set(subtitle.lesson_id, list)
  }

  return courseRows.map((courseRow) => {
    const courseSections = [...(sectionsByCourse.get(courseRow.id) ?? [])].sort(
      (a, b) => (a.order_index ?? 0) - (b.order_index ?? 0) || a.name.localeCompare(b.name, undefined, { numeric: true })
    )
    const courseLessons = [...(lessonsByCourse.get(courseRow.id) ?? [])].sort(
      (a, b) => (a.order_index ?? 0) - (b.order_index ?? 0) || a.name.localeCompare(b.name, undefined, { numeric: true })
    )

    const sections: Section[] = []
    const sectionByKey = new Map<string, Section>()

    for (const sectionRow of courseSections) {
      const section: Section = {
        id: sectionRow.id,
        name: sectionRow.name,
        lessons: [],
        order: sectionRow.order_index ?? sections.length,
      }
      sections.push(section)
      sectionByKey.set(sectionRow.id, section)
      sectionByKey.set(sectionRow.name, section)
    }

    for (const lessonRow of courseLessons) {
      const fallbackSectionName = lessonRow.section_name ?? "Course"
      let section = lessonRow.section_id ? sectionByKey.get(lessonRow.section_id) : undefined
      section ??= sectionByKey.get(fallbackSectionName)

      if (!section) {
        section = {
          id: `${courseRow.id}:section:${sections.length}`,
          name: fallbackSectionName,
          lessons: [],
          order: sections.length,
        }
        sections.push(section)
        sectionByKey.set(section.id, section)
        sectionByKey.set(section.name, section)
      }

      section.lessons.push({
        id: lessonRow.id,
        courseId: courseRow.id,
        sectionName: section.name,
        name: lessonRow.name,
        path: lessonRow.path,
        type: lessonRow.type,
        duration: lessonRow.duration ?? 0,
        fileSize: lessonRow.file_size ?? 0,
        completed: Boolean(lessonRow.completed),
        watchedTime: lessonRow.watched_time ?? 0,
        lastPosition: lessonRow.last_position ?? 0,
        order: lessonRow.order_index ?? section.lessons.length,
        subtitles: subtitlesByLesson.get(lessonRow.id) ?? [],
      })
    }

    for (const section of sections) {
      section.lessons.sort((a, b) => a.order - b.order || a.name.localeCompare(b.name, undefined, { numeric: true }))
    }

    sections.sort((a, b) => a.order - b.order || a.name.localeCompare(b.name, undefined, { numeric: true }))

    const lessons = sections.flatMap((section) => section.lessons)

    return {
      id: courseRow.id,
      name: courseRow.name,
      path: courseRow.path,
      sections,
      progress: lessons.length > 0 ? Math.round((lessons.filter((lesson) => lesson.completed).length / lessons.length) * 100) : 0,
      totalDuration: courseRow.total_duration ?? 0,
      watchedDuration: courseRow.watched_duration ?? 0,
      lastAccessed: courseRow.last_accessed,
      thumbnail: null,
      thumbnailSourcePath: courseRow.thumbnail_source_path ?? lessons.find((lesson) => lesson.type === "video")?.path ?? null,
    }
  })
}

function inferLibraryPath(courseRows: PersistedCourseRow[]): string | null {
  if (courseRows.length === 0) return null
  if (courseRows.length === 1) return courseRows[0].path

  const normalizedPaths = courseRows.map((course) => trimTrailingSeparators(course.path).replace(/\\/g, "/"))
  const splitPaths = normalizedPaths.map((path) => path.split("/"))
  const shortest = Math.min(...splitPaths.map((parts) => parts.length))
  const common: string[] = []

  for (let index = 0; index < shortest - 1; index++) {
    const part = splitPaths[0][index]
    if (splitPaths.every((parts) => parts[index] === part)) common.push(part)
    else break
  }

  if (common.length === 0) return null
  return common.length === 1 && common[0] === "" ? "/" : common.join("/")
}

export async function initDatabase(): Promise<void> {
  await getDatabase()
}

export async function loadPersistedLibrary(): Promise<PersistedLibrary> {
  const database = await getDatabase()
  if (!database) return { courses: [], libraryPath: null }

  const libraryPath = await readSetting(LIBRARY_PATH_SETTING)
  const courseRows = libraryPath
    ? await database.select<PersistedCourseRow[]>(
        `SELECT id, name, path, total_duration, watched_duration, last_accessed, thumbnail_source_path
         FROM courses
         WHERE path = $1 OR path LIKE $2 ESCAPE '~'
         ORDER BY COALESCE(last_accessed, '') DESC, name COLLATE NOCASE ASC`,
        [libraryPath, childPathPattern(libraryPath)]
      )
    : await database.select<PersistedCourseRow[]>(
        `SELECT id, name, path, total_duration, watched_duration, last_accessed, thumbnail_source_path
         FROM courses
         ORDER BY COALESCE(last_accessed, '') DESC, name COLLATE NOCASE ASC`
      )

  if (courseRows.length === 0) return { courses: [], libraryPath }

  const courseIds = courseRows.map((course) => course.id)
  const sectionRows: PersistedSectionRow[] = []
  const lessonRows: PersistedLessonRow[] = []

  for (let index = 0; index < courseIds.length; index += SQLITE_BATCH_SIZE) {
    const batch = courseIds.slice(index, index + SQLITE_BATCH_SIZE)
    sectionRows.push(...await database.select<PersistedSectionRow[]>(
      `SELECT id, course_id, name, order_index
       FROM sections
       WHERE course_id IN (${createPlaceholders(batch.length)})`,
      batch
    ))
    lessonRows.push(...await database.select<PersistedLessonRow[]>(
      `SELECT id, course_id, section_id, section_name, name, path, type, duration, file_size,
              watched_time, last_position, completed, order_index
       FROM lessons
       WHERE course_id IN (${createPlaceholders(batch.length)})`,
      batch
    ))
  }

  const lessonIds = lessonRows.map((lesson) => lesson.id)
  const subtitleRows: PersistedSubtitleRow[] = []
  for (let index = 0; index < lessonIds.length; index += SQLITE_BATCH_SIZE) {
    const batch = lessonIds.slice(index, index + SQLITE_BATCH_SIZE)
    subtitleRows.push(...await database.select<PersistedSubtitleRow[]>(
      `SELECT lesson_id, path, language, label, order_index
       FROM lesson_subtitles
       WHERE lesson_id IN (${createPlaceholders(batch.length)})
       ORDER BY order_index ASC`,
      batch
    ))
  }

  return {
    courses: rowsToCourses(courseRows, sectionRows, lessonRows, subtitleRows),
    libraryPath: libraryPath ?? inferLibraryPath(courseRows),
  }
}

export async function syncLibrary(courses: Course[], libraryPath: string): Promise<Course[]> {
  const database = await getDatabase()
  if (!database) {
    return courses
  }

  const coursePaths = courses.map((course) => course.path)
  const lessonPaths = courses.flatMap((course) =>
    course.sections.flatMap((section) => section.lessons.map((lesson) => lesson.path))
  )

  const [persistedCourses, persistedLessons] = await Promise.all([
    selectPersistedCourses(coursePaths),
    selectPersistedLessons(lessonPaths),
  ])

  const persistedCourseByPath = new Map(
    persistedCourses.map((course) => [course.path, course])
  )
  const persistedLessonByPath = new Map(
    persistedLessons.map((lesson) => [lesson.path, lesson])
  )

  const resolvedCourses = courses.map((course) => {
    const persistedCourse = persistedCourseByPath.get(course.path)
    const courseId = persistedCourse?.id ?? course.id

    return {
      ...course,
      id: courseId,
      lastAccessed: persistedCourse?.last_accessed ?? course.lastAccessed,
      sections: course.sections.map((section) => ({
        ...section,
        lessons: section.lessons.map((lesson) => {
          const persistedLesson = persistedLessonByPath.get(lesson.path)

          return {
            ...lesson,
            id: persistedLesson?.id ?? lesson.id,
            courseId,
            watchedTime: persistedLesson?.watched_time ?? lesson.watchedTime,
            lastPosition: persistedLesson?.last_position ?? lesson.lastPosition,
            completed:
              persistedLesson !== undefined
                ? Boolean(persistedLesson.completed)
                : lesson.completed,
          }
        }),
      })),
    }
  })

  await executeTransaction(database, async () => {
    await writeSetting(database, LIBRARY_PATH_SETTING, libraryPath)
    const scanStamp = new Date().toISOString()

    for (const course of resolvedCourses) {
      const lessons = course.sections.flatMap((section) => section.lessons)
      const thumbnailSourcePath = course.thumbnailSourcePath ?? lessons.find((lesson) => lesson.type === "video")?.path ?? null

      await database.execute(
        `INSERT INTO courses (id, name, path, total_duration, watched_duration, last_accessed, thumbnail_source_path, last_scanned_at)
         VALUES ($1, $2, $3, $4, $5, $6, $7, $8)
         ON CONFLICT(path) DO UPDATE SET
           id = excluded.id,
           name = excluded.name,
           total_duration = excluded.total_duration,
           watched_duration = excluded.watched_duration,
           last_accessed = excluded.last_accessed,
           thumbnail_source_path = excluded.thumbnail_source_path,
           last_scanned_at = excluded.last_scanned_at`,
        [
          course.id,
          course.name,
          course.path,
          course.totalDuration,
          course.watchedDuration,
          course.lastAccessed,
          thumbnailSourcePath,
          scanStamp,
        ]
      )

      for (const section of course.sections) {
        await database.execute(
          `INSERT INTO sections (id, course_id, name, order_index, updated_at)
           VALUES ($1, $2, $3, $4, $5)
           ON CONFLICT(course_id, name) DO UPDATE SET
             id = excluded.id,
             order_index = excluded.order_index,
             updated_at = excluded.updated_at`,
          [section.id, course.id, section.name, section.order, scanStamp]
        )

        for (const lesson of section.lessons) {
          await database.execute(
            `INSERT INTO lessons (
              id,
              course_id,
              section_id,
              section_name,
              name,
              path,
              type,
              duration,
              file_size,
              watched_time,
              completed,
              order_index,
              last_position,
              updated_at
            ) VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14)
            ON CONFLICT(path) DO UPDATE SET
              id = excluded.id,
              course_id = excluded.course_id,
              section_id = excluded.section_id,
              section_name = excluded.section_name,
              name = excluded.name,
              type = excluded.type,
              duration = excluded.duration,
              file_size = excluded.file_size,
              order_index = excluded.order_index,
              updated_at = excluded.updated_at`,
            [
              lesson.id,
              course.id,
              section.id,
              lesson.sectionName,
              lesson.name,
              lesson.path,
              lesson.type,
              lesson.duration,
              lesson.fileSize,
              lesson.watchedTime,
              lesson.completed ? 1 : 0,
              lesson.order,
              lesson.lastPosition,
              scanStamp,
            ]
          )

          await database.execute(`DELETE FROM lesson_subtitles WHERE lesson_id = $1`, [lesson.id])

          for (let index = 0; index < lesson.subtitles.length; index++) {
            const subtitle = lesson.subtitles[index]
            await database.execute(
              `INSERT INTO lesson_subtitles (id, lesson_id, path, language, label, order_index)
               VALUES ($1, $2, $3, $4, $5, $6)
               ON CONFLICT(lesson_id, path) DO UPDATE SET
                 language = excluded.language,
                 label = excluded.label,
                 order_index = excluded.order_index`,
              [makeSubtitleId(lesson.id, index), lesson.id, subtitle.path, subtitle.language, subtitle.label, index]
            )
          }
        }
      }

      await database.execute(
        `DELETE FROM lessons WHERE course_id = $1 AND (updated_at IS NULL OR updated_at <> $2)`,
        [course.id, scanStamp]
      )
      await database.execute(
        `DELETE FROM sections WHERE course_id = $1 AND (updated_at IS NULL OR updated_at <> $2)`,
        [course.id, scanStamp]
      )
    }

    await database.execute(
      `DELETE FROM courses WHERE (path = $1 OR path LIKE $2 ESCAPE '~') AND (last_scanned_at IS NULL OR last_scanned_at <> $3)`,
      [libraryPath, childPathPattern(libraryPath), scanStamp]
    )
    await database.execute(
      `DELETE FROM lesson_subtitles WHERE lesson_id NOT IN (SELECT id FROM lessons)`
    )
  })

  return resolvedCourses
}

export async function updateLessonProgress(
  lessonId: string,
  watchedTime: number,
  lastPosition: number,
  completed: boolean
): Promise<void> {
  const database = await getDatabase()
  if (!database) return

  await database.execute(
    `UPDATE lessons SET watched_time = $1, last_position = $2, completed = $3, updated_at = CURRENT_TIMESTAMP WHERE id = $4`,
    [watchedTime, lastPosition, completed ? 1 : 0, lessonId]
  )
}

export async function updateCourseLastAccessed(
  courseId: string,
  timestamp: string
): Promise<void> {
  const database = await getDatabase()
  if (!database) return

  await database.execute(`UPDATE courses SET last_accessed = $1 WHERE id = $2`, [timestamp, courseId])
}

export async function saveNote(note: Note): Promise<void> {
  const database = await getDatabase()
  if (!database) return

  await database.execute(
    `INSERT INTO notes (id, lesson_id, timestamp, text, created_at) VALUES ($1, $2, $3, $4, $5)`,
    [note.id, note.lessonId, note.timestamp, note.text, note.createdAt]
  )
}

export async function listNotesByLesson(lessonId: string): Promise<Note[]> {
  const database = await getDatabase()
  if (!database) return []

  const rows = await database.select<PersistedNoteRow[]>(
    `SELECT id, lesson_id, timestamp, text, created_at
     FROM notes
     WHERE lesson_id = $1
     ORDER BY timestamp ASC, created_at ASC`,
    [lessonId]
  )

  return rows.map((row) => ({
    id: row.id,
    lessonId: row.lesson_id,
    timestamp: row.timestamp,
    text: row.text,
    createdAt: row.created_at,
  }))
}

export async function deleteNote(noteId: string): Promise<void> {
  const database = await getDatabase()
  if (!database) return

  await database.execute(`DELETE FROM notes WHERE id = $1`, [noteId])
}
