import test from "node:test"
import assert from "node:assert/strict"
import { readFileSync } from "node:fs"
import { join } from "node:path"

const repoRoot = process.cwd()

test("thumbnail hydration does not rebuild the search index", () => {
  const appBootstrap = readFileSync(join(repoRoot, "components/app-bootstrap.tsx"), "utf8")
  const operations = readFileSync(join(repoRoot, "lib/operations.ts"), "utf8")
  const thumbnails = readFileSync(join(repoRoot, "lib/course-thumbnails.ts"), "utf8")

  assert.equal(
    appBootstrap.includes("indexCourses(library.courses, library.libraryPath)"),
    true,
    "Initial library load should build the FFF search index from the persisted root path."
  )
  assert.equal(
    operations.includes("indexCourses(hydrated, path)"),
    true,
    "A scan should rebuild the FFF search index from the selected root path."
  )
  assert.equal(
    appBootstrap.includes("app.bootstrap.indexDone"),
    false,
    "AppBootstrap should not rebuild the full search index on every course-store update."
  )
  assert.equal(
    appBootstrap.includes("indexCourses(courses)\n        })"),
    false,
    "Initial thumbnail hydration updates should not rebuild search."
  )
  assert.equal(
    operations.includes("indexCourses(courses)\n  })"),
    false,
    "Scan thumbnail hydration updates should not rebuild search."
  )
  assert.equal(
    thumbnails.includes("if (changed) onUpdate(nextCourses)"),
    false,
    "Thumbnail hydration should not write the course store once per thumbnail."
  )
  assert.equal(
    thumbnails.includes("const THUMBNAIL_UPDATE_BATCH_SIZE = 4"),
    true,
    "Thumbnail hydration should publish progressive batches so visible images do not wait for the whole run."
  )
  assert.equal(
    thumbnails.includes("function flushHydratedThumbnails()"),
    true,
    "Thumbnail hydration should keep batched updates behind one flush helper."
  )
  assert.equal(
    thumbnails.includes("pendingHydrated >= THUMBNAIL_UPDATE_BATCH_SIZE"),
    true,
    "Thumbnail hydration should flush after a small batch instead of waiting for every course."
  )
})
