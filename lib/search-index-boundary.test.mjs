import test from "node:test"
import assert from "node:assert/strict"
import { readFileSync } from "node:fs"
import { join } from "node:path"

const repoRoot = process.cwd()

test("thumbnail hydration does not rebuild the search index", () => {
  const appBootstrap = readFileSync(join(repoRoot, "components/app-bootstrap.tsx"), "utf8")
  const operations = readFileSync(join(repoRoot, "lib/operations.ts"), "utf8")

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
})
