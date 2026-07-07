import test from "node:test"
import assert from "node:assert/strict"
import { readFileSync } from "node:fs"
import { join } from "node:path"

const repoRoot = process.cwd()

test("scan sync retries transient sqlite locks without changing normal startup", () => {
  const source = readFileSync(join(repoRoot, "lib/operations.ts"), "utf8")

  assert.equal(source.includes("const SYNC_RETRY_DELAYS_MS = [250, 500, 1000, 2000, 4000]"), true)
  assert.equal(source.includes("function isDatabaseLockedError"), true)
  assert.equal(source.includes('message.includes("database is locked")'), true)
  assert.equal(source.includes('message.includes("code: 5")'), true)
  assert.equal(source.includes("async function syncLibraryWithLockRetry"), true)
  assert.equal(source.includes("return await syncLibrary(courses, path)"), true)
  assert.equal(source.includes('frontendLog("warn", "scan.sync.retry", context)'), true)
  assert.equal(source.includes("throw new Error(`Saving scan failed: ${errorMessage(err)}`)"), true)
})
