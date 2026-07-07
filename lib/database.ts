import Database from "@tauri-apps/plugin-sql"
import type { ActivityDay, Course, Lesson, Note, Section, SubtitleFile } from "@/types"
import { isTauri, getDatabasePath } from "./tauri.ts"
import { frontendLog } from "./frontend-log.ts"
import {
  persistedLessonIdentitySignature,
  scannedLessonIdentitySignature,
  selectCourseIdentityMatch,
  selectLessonIdentityMatch,
} from "./course-identity.ts"

type DatabaseConnection = Pick<Database, "execute" | "select">

let db: DatabaseConnection | null = null
let testDatabase: DatabaseConnection | null = null
let dbPathPromise: Promise<string | null> | null = null
let databaseWriteQueue: Promise<void> = Promise.resolve()
const SQLITE_BATCH_SIZE = 500
const SQLITE_MAX_PARAMETERS = 900
const LIBRARY_PATH_SETTING = "libraryPath"

const databaseLog = (message: string, context?: Record<string, unknown>) => {
  frontendLog("info", `database.${message}`, context)
}

type PersistedCourseRow = {
  id: string
  identity_id: string | null
  name: string
  path: string
  fingerprint: string | null
  total_duration: number
  watched_duration: number
  last_accessed: string | null
  thumbnail_source_path: string | null
  missing_since: string | null
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
  relative_path: string | null
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

type PersistedLessonProgressRow = {
  course_id: string
  watched_time: number | null
  last_position: number | null
  completed: number | null
}

type PersistedActivityDayRow = {
  activity_date: string
  watched_seconds: number | null
  lessons_touched: number | null
  completions: number | null
}

type SyncLibraryResult = {
  courses: Course[]
  warnings: string[]
}

const COURSE_SELECT_COLUMNS = [
  "id",
  "identity_id",
  "name",
  "path",
  "fingerprint",
  "total_duration",
  "watched_duration",
  "last_accessed",
  "thumbnail_source_path",
  "missing_since",
].join(", ")

const LESSON_SELECT_COLUMNS = [
  "id",
  "course_id",
  "section_id",
  "section_name",
  "name",
  "path",
  "relative_path",
  "type",
  "duration",
  "file_size",
  "watched_time",
  "last_position",
  "completed",
  "order_index",
].join(", ")

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

async function getDatabase(): Promise<DatabaseConnection | null> {
  if (testDatabase) return testDatabase
  if (!isTauri()) return null

  if (!db) {
    const path = await resolveDatabasePath()
    if (!path) return null
    db = await Database.load(path)
    await db.execute("PRAGMA foreign_keys = ON")
    await db.execute("PRAGMA journal_mode = WAL")
    await db.execute("PRAGMA busy_timeout = 10000")
  }

  return db
}

export function __setDatabaseForTests(database: DatabaseConnection | null): void {
  testDatabase = database
  db = null
  dbPathPromise = null
  databaseWriteQueue = Promise.resolve()
}

function createPlaceholders(count: number): string {
  return Array.from({ length: count }, (_, index) => `$${index + 1}`).join(", ")
}

function createRowPlaceholders(rowCount: number, fieldCount: number): string {
  let next = 1
  return Array.from({ length: rowCount }, () => {
    const row = Array.from({ length: fieldCount }, () => `$${next++}`).join(", ")
    return `(${row})`
  }).join(", ")
}

function batchSize(fieldCount: number): number {
  return Math.max(1, Math.floor(SQLITE_MAX_PARAMETERS / fieldCount))
}

function chunk<T>(items: T[], size: number): T[][] {
  const chunks: T[][] = []
  for (let index = 0; index < items.length; index += size) {
    chunks.push(items.slice(index, index + size))
  }
  return chunks
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

async function serializeDatabaseWrite<T>(work: () => Promise<T>): Promise<T> {
  const result = databaseWriteQueue.then(work, work)
  databaseWriteQueue = result.then(() => undefined, () => undefined)
  return result
}

function makeSubtitleId(lessonId: string, index: number): string {
  return `${lessonId}:subtitle:${index}`
}

function makeActivityId(lessonId: string): string {
  if (typeof crypto !== "undefined" && "randomUUID" in crypto) {
    return crypto.randomUUID()
  }

  return `${lessonId}:activity:${Date.now()}:${Math.random().toString(36).slice(2)}`
}

function todayKey(): string {
  return new Date().toISOString().slice(0, 10)
}

async function selectPersistedCourses(paths: string[]): Promise<PersistedCourseRow[]> {
  const database = await getDatabase()
  if (!database || paths.length === 0) return []

  const rows: PersistedCourseRow[] = []

  for (let index = 0; index < paths.length; index += SQLITE_BATCH_SIZE) {
    const batch = paths.slice(index, index + SQLITE_BATCH_SIZE)
    const result = await database.select<PersistedCourseRow[]>(
      `SELECT ${COURSE_SELECT_COLUMNS}
       FROM courses
       WHERE path IN (${createPlaceholders(batch.length)})`,
      batch
    )

    rows.push(...result)
  }

  return rows
}

async function selectPersistedCoursesByFingerprint(fingerprints: string[]): Promise<PersistedCourseRow[]> {
  const database = await getDatabase()
  if (!database || fingerprints.length === 0) return []

  const rows: PersistedCourseRow[] = []

  for (let index = 0; index < fingerprints.length; index += SQLITE_BATCH_SIZE) {
    const batch = fingerprints.slice(index, index + SQLITE_BATCH_SIZE)
    const result = await database.select<PersistedCourseRow[]>(
      `SELECT ${COURSE_SELECT_COLUMNS}
       FROM courses
       WHERE fingerprint IN (${createPlaceholders(batch.length)})`,
      batch
    )

    rows.push(...result)
  }

  return rows
}

async function selectPersistedCoursesByIdentityIds(identityIds: string[]): Promise<PersistedCourseRow[]> {
  const database = await getDatabase()
  if (!database || identityIds.length === 0) return []

  const rows: PersistedCourseRow[] = []

  for (let index = 0; index < identityIds.length; index += SQLITE_BATCH_SIZE) {
    const batch = identityIds.slice(index, index + SQLITE_BATCH_SIZE)
    const result = await database.select<PersistedCourseRow[]>(
      `SELECT ${COURSE_SELECT_COLUMNS}
       FROM courses
       WHERE identity_id IN (${createPlaceholders(batch.length)})`,
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
      `SELECT ${LESSON_SELECT_COLUMNS}
       FROM lessons
       WHERE path IN (${createPlaceholders(batch.length)})`,
      batch
    )

    rows.push(...result)
  }

  return rows
}

async function selectPersistedLessonsByCourseIds(courseIds: string[]): Promise<PersistedLessonRow[]> {
  const database = await getDatabase()
  if (!database || courseIds.length === 0) return []

  const rows: PersistedLessonRow[] = []

  for (let index = 0; index < courseIds.length; index += SQLITE_BATCH_SIZE) {
    const batch = courseIds.slice(index, index + SQLITE_BATCH_SIZE)
    const result = await database.select<PersistedLessonRow[]>(
      `SELECT ${LESSON_SELECT_COLUMNS}
       FROM lessons
       WHERE course_id IN (${createPlaceholders(batch.length)})`,
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

async function writeSetting(database: DatabaseConnection, key: string, value: string | null): Promise<void> {
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
        relativePath: lessonRow.relative_path,
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
      identityId: courseRow.identity_id ?? courseRow.id,
      markerIdentityId: null,
      name: courseRow.name,
      path: courseRow.path,
      fingerprint: courseRow.fingerprint,
      missingSince: courseRow.missing_since,
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

function uniqueValues(values: Array<string | null | undefined>): string[] {
  return [...new Set(values.filter((value): value is string => Boolean(value)))]
}

function duplicateValues(values: string[]): Set<string> {
  const counts = new Map<string, number>()
  for (const value of values) {
    counts.set(value, (counts.get(value) ?? 0) + 1)
  }
  return new Set([...counts].filter(([, count]) => count > 1).map(([value]) => value))
}

function groupBy<T>(items: T[], keyFor: (item: T) => string | null | undefined): Map<string, T[]> {
  const groups = new Map<string, T[]>()
  for (const item of items) {
    const key = keyFor(item)
    if (!key) continue
    const list = groups.get(key) ?? []
    list.push(item)
    groups.set(key, list)
  }
  return groups
}

export async function initDatabase(): Promise<void> {
  await getDatabase()
}

export async function loadPersistedLibrary(): Promise<PersistedLibrary> {
  const startedAt = typeof performance !== "undefined" ? performance.now() : 0
  const elapsed = () => typeof performance !== "undefined" ? Math.round(performance.now() - startedAt) : 0
  databaseLog("loadPersistedLibrary.start")
  const database = await getDatabase()
  if (!database) return { courses: [], libraryPath: null }
  databaseLog("loadPersistedLibrary.databaseReady", { ms: elapsed() })

  const libraryPath = await readSetting(LIBRARY_PATH_SETTING)
  databaseLog("loadPersistedLibrary.settingLoaded", { ms: elapsed(), hasLibraryPath: Boolean(libraryPath) })
  const courseRows = libraryPath
    ? await database.select<PersistedCourseRow[]>(
        `SELECT ${COURSE_SELECT_COLUMNS}
         FROM courses
         WHERE path = $1 OR path LIKE $2 ESCAPE '~'
         ORDER BY COALESCE(last_accessed, '') DESC, name COLLATE NOCASE ASC`,
        [libraryPath, childPathPattern(libraryPath)]
      )
    : await database.select<PersistedCourseRow[]>(
        `SELECT ${COURSE_SELECT_COLUMNS}
         FROM courses
         ORDER BY COALESCE(last_accessed, '') DESC, name COLLATE NOCASE ASC`
      )
  databaseLog("loadPersistedLibrary.coursesLoaded", { ms: elapsed(), count: courseRows.length })

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
      `SELECT ${LESSON_SELECT_COLUMNS}
       FROM lessons
       WHERE course_id IN (${createPlaceholders(batch.length)})`,
      batch
    ))
  }
  databaseLog("loadPersistedLibrary.outlineLoaded", {
    ms: elapsed(),
    sectionCount: sectionRows.length,
    lessonCount: lessonRows.length,
  })

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
  databaseLog("loadPersistedLibrary.subtitlesLoaded", { ms: elapsed(), count: subtitleRows.length })

  const courses = rowsToCourses(courseRows, sectionRows, lessonRows, subtitleRows)
  databaseLog("loadPersistedLibrary.done", { ms: elapsed(), count: courses.length })

  return {
    courses,
    libraryPath: libraryPath ?? inferLibraryPath(courseRows),
  }
}

export async function syncLibrary(courses: Course[], libraryPath: string): Promise<SyncLibraryResult> {
  const database = await getDatabase()
  if (!database) {
    return { courses, warnings: [] }
  }

  return serializeDatabaseWrite(() => syncLibraryWithDatabase(database, courses, libraryPath))
}

async function syncLibraryWithDatabase(database: DatabaseConnection, courses: Course[], libraryPath: string): Promise<SyncLibraryResult> {
  const warnings: string[] = []
  const coursePaths = courses.map((course) => course.path)
  const courseFingerprints = uniqueValues(courses.map((course) => course.fingerprint))
  const scannedMarkerIdentityIds = courses
    .map((course) => course.markerIdentityId)
    .filter((value): value is string => Boolean(value))
  const courseMarkerIdentityIds = uniqueValues(scannedMarkerIdentityIds)
  const duplicateScannedMarkerIdentityIds = duplicateValues(scannedMarkerIdentityIds)
  const lessonPaths = courses.flatMap((course) =>
    course.sections.flatMap((section) => section.lessons.map((lesson) => lesson.path))
  )

  const [persistedCourses, persistedCoursesByFingerprint, persistedCoursesByIdentityId, persistedLessonsByPathRows] = await Promise.all([
    selectPersistedCourses(coursePaths),
    selectPersistedCoursesByFingerprint(courseFingerprints),
    selectPersistedCoursesByIdentityIds(courseMarkerIdentityIds),
    selectPersistedLessons(lessonPaths),
  ])

  const persistedCourseByPath = new Map(
    persistedCourses.map((course) => [course.path, course])
  )
  const persistedCoursesByFingerprintGroup = groupBy(
    persistedCoursesByFingerprint,
    (course) => course.fingerprint
  )
  const persistedCoursesByIdentityIdGroup = groupBy(
    persistedCoursesByIdentityId,
    (course) => course.identity_id
  )
  const claimedCourseIds = new Set<string>()

  const resolvedCourses = courses.map((course) => {
    const markerIdentityId = course.markerIdentityId && !duplicateScannedMarkerIdentityIds.has(course.markerIdentityId)
      ? course.markerIdentityId
      : null
    if (course.markerIdentityId && duplicateScannedMarkerIdentityIds.has(course.markerIdentityId)) {
      warnings.push(`Skipped marker identity for "${course.name}": the same marker identity appears in multiple scanned courses.`)
    }

    const { match: persistedCourse, warning } = selectCourseIdentityMatch(
      { ...course, markerIdentityId },
      persistedCourseByPath.get(course.path),
      markerIdentityId ? persistedCoursesByIdentityIdGroup.get(markerIdentityId) ?? [] : [],
      course.fingerprint ? persistedCoursesByFingerprintGroup.get(course.fingerprint) ?? [] : [],
      claimedCourseIds
    )
    if (warning) warnings.push(warning)

    const courseId = persistedCourse?.id ?? course.id
    const identityId = persistedCourse?.identity_id ?? markerIdentityId ?? course.identityId ?? courseId

    return {
      ...course,
      id: courseId,
      identityId,
      lastAccessed: persistedCourse?.last_accessed ?? course.lastAccessed,
      missingSince: null,
      sections: course.sections.map((section) => ({
        ...section,
        lessons: section.lessons.map((lesson) => ({ ...lesson, courseId })),
      })),
    }
  })

  const resolvedCourseIds = resolvedCourses.map((course) => course.id)
  const persistedLessonsByCourseRows = await selectPersistedLessonsByCourseIds(resolvedCourseIds)
  const persistedLessonRows = [
    ...new Map(
      [...persistedLessonsByPathRows, ...persistedLessonsByCourseRows].map((lesson) => [lesson.id, lesson])
    ).values(),
  ]
  const persistedLessonByPath = new Map(
    persistedLessonRows.map((lesson) => [lesson.path, lesson])
  )
  const persistedLessonsByRelativePath = groupBy(
    persistedLessonRows,
    (lesson) => lesson.relative_path ? `${lesson.course_id}\u0000${lesson.relative_path}` : null
  )
  const persistedLessonsBySignature = groupBy(
    persistedLessonRows,
    (lesson) => `${lesson.course_id}\u0000${persistedLessonIdentitySignature(lesson)}`
  )
  const claimedLessonIds = new Set<string>()

  function matchPersistedLesson(course: Course, lesson: Lesson): PersistedLessonRow | null {
    const { match, warning } = selectLessonIdentityMatch(
      course.name,
      course.id,
      lesson,
      persistedLessonByPath.get(lesson.path),
      lesson.relativePath ? persistedLessonsByRelativePath.get(`${course.id}\u0000${lesson.relativePath}`) ?? [] : [],
      persistedLessonsBySignature.get(`${course.id}\u0000${scannedLessonIdentitySignature(lesson)}`) ?? [],
      claimedLessonIds
    )
    if (warning) warnings.push(warning)
    return match
  }

  const hydratedCourses = resolvedCourses.map((course) => ({
    ...course,
    sections: course.sections.map((section) => ({
      ...section,
      lessons: section.lessons.map((lesson) => {
        const persistedLesson = matchPersistedLesson(course, lesson)

        return {
          ...lesson,
          id: persistedLesson?.id ?? lesson.id,
          watchedTime: persistedLesson?.watched_time ?? lesson.watchedTime,
          lastPosition: persistedLesson?.last_position ?? lesson.lastPosition,
          completed:
            persistedLesson !== null
              ? Boolean(persistedLesson.completed)
              : lesson.completed,
        }
      }),
    })),
  }))

  const resolvedSections = hydratedCourses.flatMap((course) =>
    course.sections.map((section) => ({ course, section }))
  )
  const resolvedLessons = resolvedSections.flatMap(({ course, section }) =>
    section.lessons.map((lesson) => ({ course, section, lesson }))
  )
  const resolvedSubtitles = resolvedLessons.flatMap(({ lesson }) =>
    lesson.subtitles.map((subtitle, index) => ({ lesson, subtitle, index }))
  )
  const resolvedLessonIds = resolvedLessons.map(({ lesson }) => lesson.id)

  await writeSetting(database, LIBRARY_PATH_SETTING, libraryPath)
  const scanStamp = new Date().toISOString()

  for (const batch of chunk(hydratedCourses, batchSize(11))) {
    await database.execute(
      `INSERT INTO courses (
        id,
        identity_id,
        name,
        path,
        fingerprint,
        total_duration,
        watched_duration,
        last_accessed,
        thumbnail_source_path,
        last_scanned_at,
        missing_since
      ) VALUES ${createRowPlaceholders(batch.length, 11)}
       ON CONFLICT(id) DO UPDATE SET
         identity_id = excluded.identity_id,
         name = excluded.name,
         path = excluded.path,
         fingerprint = excluded.fingerprint,
         total_duration = excluded.total_duration,
         watched_duration = excluded.watched_duration,
         last_accessed = excluded.last_accessed,
         thumbnail_source_path = excluded.thumbnail_source_path,
         last_scanned_at = excluded.last_scanned_at,
         missing_since = NULL`,
      batch.flatMap((course) => {
        const lessons = course.sections.flatMap((section) => section.lessons)
        const thumbnailSourcePath = course.thumbnailSourcePath ?? lessons.find((lesson) => lesson.type === "video")?.path ?? null

        return [
          course.id,
          course.identityId,
          course.name,
          course.path,
          course.fingerprint,
          course.totalDuration,
          course.watchedDuration,
          course.lastAccessed,
          thumbnailSourcePath,
          scanStamp,
          null,
        ]
      })
    )
  }

  for (const batch of chunk(resolvedSections, batchSize(5))) {
    await database.execute(
      `INSERT INTO sections (id, course_id, name, order_index, updated_at)
       VALUES ${createRowPlaceholders(batch.length, 5)}
       ON CONFLICT(course_id, name) DO UPDATE SET
         id = excluded.id,
         order_index = excluded.order_index,
         updated_at = excluded.updated_at`,
      batch.flatMap(({ course, section }) => [section.id, course.id, section.name, section.order, scanStamp])
    )
  }

  for (const batch of chunk(resolvedLessons, batchSize(14))) {
    await database.execute(
      `INSERT INTO lessons (
        id,
        course_id,
        section_id,
        section_name,
        name,
        path,
        relative_path,
        type,
        duration,
        file_size,
        watched_time,
        completed,
        order_index,
        last_position,
        updated_at
      ) VALUES ${createRowPlaceholders(batch.length, 15)}
      ON CONFLICT(id) DO UPDATE SET
        course_id = excluded.course_id,
        section_id = excluded.section_id,
        section_name = excluded.section_name,
        name = excluded.name,
        path = excluded.path,
        relative_path = excluded.relative_path,
        type = excluded.type,
        duration = excluded.duration,
        file_size = excluded.file_size,
        order_index = excluded.order_index,
        updated_at = excluded.updated_at`,
      batch.flatMap(({ course, section, lesson }) => [
        lesson.id,
        course.id,
        section.id,
        lesson.sectionName,
        lesson.name,
        lesson.path,
        lesson.relativePath,
        lesson.type,
        lesson.duration,
        lesson.fileSize,
        lesson.watchedTime,
        lesson.completed ? 1 : 0,
        lesson.order,
        lesson.lastPosition,
        scanStamp,
      ])
    )
  }

  for (const batch of chunk(resolvedLessonIds, SQLITE_BATCH_SIZE)) {
    await database.execute(
      `DELETE FROM lesson_subtitles WHERE lesson_id IN (${createPlaceholders(batch.length)})`,
      batch
    )
  }

  for (const batch of chunk(resolvedSubtitles, batchSize(6))) {
    await database.execute(
      `INSERT INTO lesson_subtitles (id, lesson_id, path, language, label, order_index)
       VALUES ${createRowPlaceholders(batch.length, 6)}
       ON CONFLICT(lesson_id, path) DO UPDATE SET
         language = excluded.language,
         label = excluded.label,
         order_index = excluded.order_index`,
      batch.flatMap(({ lesson, subtitle, index }) => [
        makeSubtitleId(lesson.id, index),
        lesson.id,
        subtitle.path,
        subtitle.language,
        subtitle.label,
        index,
      ])
    )
  }

  for (const batch of chunk(resolvedCourseIds, SQLITE_BATCH_SIZE)) {
    await database.execute(
      `DELETE FROM lessons WHERE course_id IN (${createPlaceholders(batch.length)}) AND (updated_at IS NULL OR updated_at <> $${batch.length + 1})`,
      [...batch, scanStamp]
    )
    await database.execute(
      `DELETE FROM sections WHERE course_id IN (${createPlaceholders(batch.length)}) AND (updated_at IS NULL OR updated_at <> $${batch.length + 1})`,
      [...batch, scanStamp]
    )
  }

  const missingExclusion = resolvedCourseIds.length > 0
    ? `AND id NOT IN (${resolvedCourseIds.map((_, index) => `$${index + 4}`).join(", ")})`
    : ""
  await database.execute(
    `UPDATE courses
     SET missing_since = COALESCE(missing_since, $3),
         last_scanned_at = $3
     WHERE (path = $1 OR path LIKE $2 ESCAPE '~')
       ${missingExclusion}
       AND (last_scanned_at IS NULL OR last_scanned_at <> $3)`,
    [libraryPath, childPathPattern(libraryPath), scanStamp, ...resolvedCourseIds]
  )
  await database.execute(
    `DELETE FROM lesson_subtitles WHERE lesson_id NOT IN (SELECT id FROM lessons)`
  )

  return { courses: hydratedCourses, warnings }
}

export async function updateLessonProgress(
  lessonId: string,
  watchedTime: number,
  lastPosition: number,
  completed: boolean
): Promise<void> {
  const database = await getDatabase()
  if (!database) return

  await serializeDatabaseWrite(async () => {
    const rows = await database.select<PersistedLessonProgressRow[]>(
      `SELECT course_id, watched_time, last_position, completed FROM lessons WHERE id = $1`,
      [lessonId]
    )
    const previous = rows[0]
    if (!previous) return

    const previousWatchedTime = previous.watched_time ?? 0
    const watchedDelta = Math.max(0, Math.round(watchedTime - previousWatchedTime))
    const completedValue = completed ? 1 : 0
    const completionChanged = Boolean(previous.completed) !== completed

    await database.execute(
      `UPDATE lessons SET watched_time = $1, last_position = $2, completed = $3, updated_at = CURRENT_TIMESTAMP WHERE id = $4`,
      [watchedTime, lastPosition, completedValue, lessonId]
    )

    if (watchedDelta > 0 || completionChanged) {
      await database.execute(
        `INSERT INTO lesson_activity (
          id,
          course_id,
          lesson_id,
          activity_date,
          watched_seconds,
          completed,
          created_at
        ) VALUES ($1, $2, $3, $4, $5, $6, CURRENT_TIMESTAMP)`,
        [
          makeActivityId(lessonId),
          previous.course_id,
          lessonId,
          todayKey(),
          watchedDelta,
          completionChanged && completed ? 1 : 0,
        ]
      )
    }
  })
}

export async function listLessonActivityDays(limitDays = 84): Promise<ActivityDay[]> {
  const database = await getDatabase()
  if (!database) return []

  const rows = await database.select<PersistedActivityDayRow[]>(
    `SELECT
       activity_date,
       SUM(watched_seconds) AS watched_seconds,
       COUNT(DISTINCT lesson_id) AS lessons_touched,
       SUM(completed) AS completions
     FROM lesson_activity
     WHERE activity_date >= date('now', $1)
     GROUP BY activity_date
     ORDER BY activity_date ASC`,
    [`-${limitDays} days`]
  )

  return rows.map((row) => ({
    date: row.activity_date,
    watchedSeconds: row.watched_seconds ?? 0,
    lessonsTouched: row.lessons_touched ?? 0,
    completions: row.completions ?? 0,
  }))
}

export async function updateCourseLastAccessed(
  courseId: string,
  timestamp: string
): Promise<void> {
  const database = await getDatabase()
  if (!database) return

  await serializeDatabaseWrite(() =>
    database.execute(`UPDATE courses SET last_accessed = $1 WHERE id = $2`, [timestamp, courseId])
  )
}

export async function saveNote(note: Note): Promise<void> {
  const database = await getDatabase()
  if (!database) return

  await serializeDatabaseWrite(() =>
    database.execute(
      `INSERT INTO notes (id, lesson_id, timestamp, text, created_at) VALUES ($1, $2, $3, $4, $5)`,
      [note.id, note.lessonId, note.timestamp, note.text, note.createdAt]
    )
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

  await serializeDatabaseWrite(() => database.execute(`DELETE FROM notes WHERE id = $1`, [noteId]))
}
