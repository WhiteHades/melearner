import test from "node:test"
import assert from "node:assert/strict"
import { readFileSync } from "node:fs"
import { join } from "node:path"

const repoRoot = process.cwd()

test("fast Arch install builds the production Tauri binary", () => {
  const script = readFileSync(join(repoRoot, "scripts/install-arch-fast.sh"), "utf8")

  assert.equal(script.includes("pnpm tauri build --no-bundle --ci"), true)
  assert.equal(script.includes("cargo build --manifest-path src-tauri/Cargo.toml --release"), false)
})
