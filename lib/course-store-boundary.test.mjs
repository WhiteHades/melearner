import test from "node:test"
import assert from "node:assert/strict"
import { readFileSync } from "node:fs"
import { join } from "node:path"

const repoRoot = process.cwd()

test("course store uses a vanilla store with a manual client subscription", () => {
  const courseStore = readFileSync(join(repoRoot, "lib/stores/course-store.ts"), "utf8")

  assert.equal(
    courseStore.includes('from "zustand/vanilla"'),
    true,
    "The course store should keep one vanilla store instance that can be used outside React."
  )
  assert.equal(
    courseStore.includes("useSyncExternalStore"),
    false,
    "The installed WebKit build must not depend on the React external-store path for the initial hydration repaint."
  )
  assert.equal(
    courseStore.includes("useCourseStore.subscribe = useCourseStoreInternal.subscribe"),
    true,
    "The exported hook should preserve the existing vanilla store API."
  )
})
