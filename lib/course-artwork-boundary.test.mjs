import test from "node:test"
import assert from "node:assert/strict"
import { readFileSync } from "node:fs"
import { join } from "node:path"

const repoRoot = process.cwd()

test("course thumbnails render as real images with fallback", () => {
  const source = readFileSync(join(repoRoot, "components/course-artwork.tsx"), "utf8")

  assert.equal(source.includes("backgroundImage"), false)
  assert.equal(source.includes("<img"), true)
  assert.equal(source.includes("course.thumbnail.load.failed"), true)
  assert.equal(source.includes("setFailedThumbnail(course.thumbnail)"), true)
  assert.equal(source.includes("data-course-fallback"), true)
})
