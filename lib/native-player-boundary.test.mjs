import test from "node:test"
import assert from "node:assert/strict"
import { existsSync, readFileSync } from "node:fs"
import { join } from "node:path"

const repoRoot = process.cwd()

test("native playback boundary has no Shaka or Limeplay player stack", () => {
  const removedPaths = [
    "components/limeplay",
    "hooks/limeplay",
    "components/video-player/player.tsx",
    "components/video-player/lib",
    "lib/time.ts",
  ]

  for (const path of removedPaths) {
    assert.equal(existsSync(join(repoRoot, path)), false, `${path} should not exist`)
  }

  const packageJson = JSON.parse(readFileSync(join(repoRoot, "package.json"), "utf8"))
  assert.equal(packageJson.dependencies["shaka-player"], undefined)
  assert.equal(packageJson.dependencies["@base-ui/react"], undefined)

  const componentsJson = readFileSync(join(repoRoot, "components.json"), "utf8")
  assert.equal(componentsJson.includes("@limeplay"), false)

  const playerEntrypoint = readFileSync(join(repoRoot, "components/video-player.tsx"), "utf8")
  assert.equal(playerEntrypoint.includes("Limeplay"), false)
  assert.equal(playerEntrypoint.includes("shaka"), false)
  assert.equal(playerEntrypoint.includes("<video"), false)
  assert.equal(playerEntrypoint.includes("<audio"), false)
})
