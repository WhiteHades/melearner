import test from "node:test"
import assert from "node:assert/strict"
import { readFileSync } from "node:fs"
import { join } from "node:path"

const repoRoot = process.cwd()

test("thumbnail cache directories are asset-protocol scoped", () => {
  const config = JSON.parse(readFileSync(join(repoRoot, "src-tauri/tauri.conf.json"), "utf8"))
  const scope = config.app.security.assetProtocol.scope

  assert.equal(scope.includes("$HOME/.cache/melearner/**"), true)
  assert.equal(scope.includes("$HOME/Library/Caches/melearner/**"), true)
  assert.equal(scope.includes("$LOCALAPPDATA/melearner/**"), true)
})
