import test from "node:test"
import assert from "node:assert/strict"
import { readFileSync } from "node:fs"
import { join } from "node:path"
import { DatabaseSync } from "node:sqlite"
import {
  __setDatabaseForTests,
  listLessonActivityDays,
  loadPersistedLibrary,
  syncLibrary,
  updateLessonProgress,
} from "./database.ts"

const repoRoot = process.cwd()

test("database connection waits briefly on sqlite locks", () => {
  const source = readFileSync(join(repoRoot, "lib/database.ts"), "utf8")

  assert.equal(source.includes('db.execute("PRAGMA journal_mode = WAL")'), true)
  assert.equal(source.includes('db.execute("PRAGMA busy_timeout = 10000")'), true)
  assert.ok(
    source.indexOf('db.execute("PRAGMA journal_mode = WAL")') <
      source.indexOf('db.execute("PRAGMA busy_timeout = 10000")'),
    "busy timeout should be configured after WAL mode opens the shared database"
  )
})

class TestDatabase {
  constructor() {
    this.db = new DatabaseSync(":memory:")
    this.db.exec(`
      PRAGMA foreign_keys = ON;

      CREATE TABLE courses (
        id TEXT PRIMARY KEY,
        identity_id TEXT,
        name TEXT NOT NULL,
        path TEXT UNIQUE NOT NULL,
        fingerprint TEXT,
        total_duration INTEGER DEFAULT 0,
        watched_duration INTEGER DEFAULT 0,
        last_accessed TEXT,
        thumbnail_source_path TEXT,
        last_scanned_at TEXT,
        missing_since TEXT
      );
      CREATE UNIQUE INDEX idx_courses_identity_id ON courses(identity_id) WHERE identity_id IS NOT NULL;
      CREATE INDEX idx_courses_fingerprint ON courses(fingerprint);

      CREATE TABLE sections (
        id TEXT PRIMARY KEY,
        course_id TEXT NOT NULL,
        name TEXT NOT NULL,
        order_index INTEGER DEFAULT 0,
        updated_at TEXT,
        UNIQUE(course_id, name),
        FOREIGN KEY (course_id) REFERENCES courses(id) ON DELETE CASCADE
      );

      CREATE TABLE lessons (
        id TEXT PRIMARY KEY,
        course_id TEXT NOT NULL,
        section_id TEXT,
        section_name TEXT,
        name TEXT NOT NULL,
        path TEXT UNIQUE NOT NULL,
        relative_path TEXT,
        type TEXT,
        duration INTEGER DEFAULT 0,
        file_size INTEGER DEFAULT 0,
        watched_time INTEGER DEFAULT 0,
        completed INTEGER DEFAULT 0,
        order_index INTEGER,
        last_position REAL DEFAULT 0,
        updated_at TEXT,
        FOREIGN KEY (course_id) REFERENCES courses(id) ON DELETE CASCADE
      );
      CREATE INDEX idx_lessons_course ON lessons(course_id);
      CREATE INDEX idx_lessons_course_relative_path ON lessons(course_id, relative_path);

      CREATE TABLE lesson_subtitles (
        id TEXT PRIMARY KEY,
        lesson_id TEXT NOT NULL,
        path TEXT NOT NULL,
        language TEXT,
        label TEXT,
        order_index INTEGER DEFAULT 0,
        UNIQUE(lesson_id, path),
        FOREIGN KEY (lesson_id) REFERENCES lessons(id) ON DELETE CASCADE
      );

      CREATE TABLE app_settings (
        key TEXT PRIMARY KEY,
        value TEXT,
        updated_at TEXT DEFAULT CURRENT_TIMESTAMP
      );

      CREATE TABLE lesson_activity (
        id TEXT PRIMARY KEY,
        course_id TEXT NOT NULL,
        lesson_id TEXT NOT NULL,
        activity_date TEXT NOT NULL,
        watched_seconds INTEGER DEFAULT 0,
        completed INTEGER DEFAULT 0,
        created_at TEXT DEFAULT CURRENT_TIMESTAMP,
        FOREIGN KEY (course_id) REFERENCES courses(id) ON DELETE CASCADE,
        FOREIGN KEY (lesson_id) REFERENCES lessons(id) ON DELETE CASCADE
      );
      CREATE INDEX idx_lesson_activity_date ON lesson_activity(activity_date);
      CREATE INDEX idx_lesson_activity_lesson ON lesson_activity(lesson_id);
    `)
  }

  bind(params) {
    return Object.fromEntries(params.map((value, index) => [`$${index + 1}`, value]))
  }

  async execute(sql, params = []) {
    const statement = this.db.prepare(sql)
    if (params.length > 0) {
      statement.run(this.bind(params))
    } else {
      statement.run()
    }
  }

  async select(sql, params = []) {
    const statement = this.db.prepare(sql)
    const rows = params.length > 0 ? statement.all(this.bind(params)) : statement.all()
    return rows.map((row) => ({ ...row }))
  }

  close() {
    this.db.close()
  }
}

class SlowBeginDatabase extends TestDatabase {
  beginDelayUsed = false

  async execute(sql, params = []) {
    if (sql.trim() === "BEGIN" && !this.beginDelayUsed) {
      this.beginDelayUsed = true
      await super.execute(sql, params)
      await new Promise((resolve) => setTimeout(resolve, 5))
      return
    }

    return super.execute(sql, params)
  }
}

function testCourse({ id, path, name = "Arm Assembly", fingerprint = "fp-arm" }) {
  return {
    id,
    identityId: id,
    markerIdentityId: null,
    name,
    path,
    fingerprint,
    missingSince: null,
    sections: [
      {
        id: `${id}:section:intro`,
        name: "Intro",
        order: 0,
        lessons: [
          {
            id: `${id}:lesson:welcome`,
            courseId: id,
            sectionName: "Intro",
            name: "welcome",
            path: `${path}/Intro/welcome.mp4`,
            relativePath: "Intro/welcome.mp4",
            type: "video",
            duration: 100,
            fileSize: 7,
            completed: false,
            watchedTime: 0,
            lastPosition: 0,
            order: 0,
            subtitles: [],
          },
        ],
      },
    ],
    progress: 0,
    totalDuration: 100,
    watchedDuration: 0,
    lastAccessed: null,
    thumbnail: null,
    thumbnailSourcePath: `${path}/Intro/welcome.mp4`,
  }
}

test("syncLibrary marks missing courses and later reconnects moved courses with progress", async (t) => {
  const database = new TestDatabase()
  __setDatabaseForTests(database)
  t.after(() => {
    __setDatabaseForTests(null)
    database.close()
  })

  const original = testCourse({ id: "scan-course-a", path: "/library/Arm Assembly" })
  await syncLibrary([original], "/library")
  await updateLessonProgress("scan-course-a:lesson:welcome", 42, 42, false)

  await syncLibrary([], "/library")
  const missingLibrary = await loadPersistedLibrary()
  const missingCourse = missingLibrary.courses[0]
  assert.equal(missingCourse.id, "scan-course-a")
  assert.equal(missingCourse.sections[0].lessons[0].watchedTime, 42)
  assert.match(missingCourse.missingSince ?? "", /^\d{4}-\d{2}-\d{2}T/)

  const moved = testCourse({ id: "scan-course-b", path: "/library/Renamed Arm Assembly" })
  const result = await syncLibrary([moved], "/library")
  assert.deepEqual(result.warnings, [])
  assert.equal(result.courses[0].id, "scan-course-a")
  assert.equal(result.courses[0].sections[0].lessons[0].id, "scan-course-a:lesson:welcome")
  assert.equal(result.courses[0].sections[0].lessons[0].watchedTime, 42)

  const reconnectedLibrary = await loadPersistedLibrary()
  assert.equal(reconnectedLibrary.courses.length, 1)
  assert.equal(reconnectedLibrary.courses[0].path, "/library/Renamed Arm Assembly")
  assert.equal(reconnectedLibrary.courses[0].missingSince, null)
  assert.equal(reconnectedLibrary.courses[0].sections[0].lessons[0].lastPosition, 42)
})

test("syncLibrary refuses ambiguous duplicate fingerprints in SQLite", async (t) => {
  const database = new TestDatabase()
  __setDatabaseForTests(database)
  t.after(() => {
    __setDatabaseForTests(null)
    database.close()
  })

  await syncLibrary([
    testCourse({ id: "scan-course-a", path: "/library/Copy A" }),
    testCourse({ id: "scan-course-b", path: "/library/Copy B" }),
  ], "/library")
  await updateLessonProgress("scan-course-a:lesson:welcome", 55, 55, true)

  const result = await syncLibrary([
    testCourse({ id: "scan-course-c", path: "/library/Renamed Copy" }),
  ], "/library")

  assert.equal(result.courses[0].id, "scan-course-c")
  assert.equal(result.courses[0].sections[0].lessons[0].watchedTime, 0)
  assert.match(result.warnings.join(" "), /multiple existing courses have the same fingerprint/)

  const library = await loadPersistedLibrary()
  assert.equal(library.courses.length, 3)
  assert.equal(
    library.courses.filter((course) => course.missingSince !== null).length,
    2
  )
})

test("syncLibrary matches moved courses by marker identity before fingerprint", async (t) => {
  const database = new TestDatabase()
  __setDatabaseForTests(database)
  t.after(() => {
    __setDatabaseForTests(null)
    database.close()
  })

  const original = testCourse({ id: "scan-course-a", path: "/library/Original", fingerprint: "fp-old" })
  await syncLibrary([original], "/library")
  await updateLessonProgress("scan-course-a:lesson:welcome", 31, 31, false)

  const moved = {
    ...testCourse({ id: "scan-course-b", path: "/library/Moved", fingerprint: "fp-new" }),
    markerIdentityId: "scan-course-a",
  }
  const result = await syncLibrary([moved], "/library")

  assert.deepEqual(result.warnings, [])
  assert.equal(result.courses[0].id, "scan-course-a")
  assert.equal(result.courses[0].sections[0].lessons[0].watchedTime, 31)
})

test("syncLibrary ignores duplicate scanned marker identities", async (t) => {
  const database = new TestDatabase()
  __setDatabaseForTests(database)
  t.after(() => {
    __setDatabaseForTests(null)
    database.close()
  })

  const first = {
    ...testCourse({ id: "scan-course-a", path: "/library/Copy A", fingerprint: "fp-a" }),
    markerIdentityId: "shared-marker",
  }
  const second = {
    ...testCourse({ id: "scan-course-b", path: "/library/Copy B", fingerprint: "fp-b" }),
    markerIdentityId: "shared-marker",
  }

  const result = await syncLibrary([first, second], "/library")

  assert.equal(result.courses[0].identityId, "scan-course-a")
  assert.equal(result.courses[1].identityId, "scan-course-b")
  assert.match(result.warnings.join(" "), /same marker identity appears in multiple scanned courses/)
})

test("updateLessonProgress records daily lesson activity in SQLite", async (t) => {
  const database = new TestDatabase()
  __setDatabaseForTests(database)
  t.after(() => {
    __setDatabaseForTests(null)
    database.close()
  })

  await syncLibrary([testCourse({ id: "scan-course-a", path: "/library/Activity" })], "/library")
  await updateLessonProgress("scan-course-a:lesson:welcome", 10, 10, false)
  await updateLessonProgress("scan-course-a:lesson:welcome", 15, 15, true)
  await updateLessonProgress("scan-course-a:lesson:welcome", 12, 12, true)

  const days = await listLessonActivityDays(7)
  assert.equal(days.length, 1)
  assert.equal(days[0].watchedSeconds, 15)
  assert.equal(days[0].lessonsTouched, 1)
  assert.equal(days[0].completions, 1)
})

test("updateLessonProgress serializes concurrent SQLite writes", async (t) => {
  const database = new SlowBeginDatabase()
  __setDatabaseForTests(database)
  t.after(() => {
    __setDatabaseForTests(null)
    database.close()
  })

  await syncLibrary([testCourse({ id: "scan-course-a", path: "/library/Concurrent" })], "/library")

  await Promise.all([
    updateLessonProgress("scan-course-a:lesson:welcome", 10, 10, false),
    updateLessonProgress("scan-course-a:lesson:welcome", 20, 20, false),
  ])

  const days = await listLessonActivityDays(7)
  assert.equal(days.length, 1)
  assert.equal(days[0].watchedSeconds, 20)
})
