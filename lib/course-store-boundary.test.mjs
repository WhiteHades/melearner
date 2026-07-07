import test from "node:test"
import assert from "node:assert/strict"
import { readFileSync } from "node:fs"
import { join } from "node:path"

const repoRoot = process.cwd()

test("course store uses a vanilla store with React external-store subscription", () => {
  const courseStore = readFileSync(join(repoRoot, "lib/stores/course-store.ts"), "utf8")

  assert.equal(
    courseStore.includes('from "zustand/vanilla"'),
    true,
    "The course store should keep one vanilla store instance that can be used outside React."
  )
  assert.equal(
    courseStore.includes("useSyncExternalStore"),
    true,
    "React should read course-store snapshots through its external-store API."
  )
  assert.equal(
    courseStore.includes("useCourseStore.subscribe = useCourseStoreInternal.subscribe"),
    true,
    "The exported hook should preserve the existing vanilla store API."
  )
  assert.equal(
    courseStore.includes("update(useCourseStoreInternal.getState())"),
    false,
    "The hook should not depend on a passive-effect replay to notice hydration."
  )
})
