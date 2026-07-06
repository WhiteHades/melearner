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

test("native video surface follows host window and layout movement", () => {
  const playerEntrypoint = readFileSync(join(repoRoot, "components/video-player.tsx"), "utf8")

  assert.equal(playerEntrypoint.includes("getCurrentWindow"), true)
  assert.equal(playerEntrypoint.includes(".onMoved(requestBoundsUpdate)"), true)
  assert.equal(playerEntrypoint.includes(".onResized(requestBoundsUpdate)"), true)
  assert.equal(playerEntrypoint.includes('window.addEventListener("scroll", requestBoundsUpdate, true)'), true)
  assert.equal(playerEntrypoint.includes('document.addEventListener("fullscreenchange", requestBoundsUpdate)'), true)
  assert.equal(playerEntrypoint.includes("window.requestAnimationFrame"), true)
})

test("native player interpolates visible position between coarse native events", () => {
  const playerEntrypoint = readFileSync(join(repoRoot, "components/video-player.tsx"), "utf8")

  assert.equal(playerEntrypoint.includes("visibleCurrentTime"), true)
  assert.equal(playerEntrypoint.includes("positionRafRef"), true)
  assert.equal(playerEntrypoint.includes("performance.now()"), true)
  assert.equal(playerEntrypoint.includes("state.rate"), true)
  assert.equal(playerEntrypoint.includes("isSeekingRef"), true)
})

test("native player bridge exposes typed track chapter and file-loaded events", () => {
  const nativeBridge = readFileSync(join(repoRoot, "lib/native-player.ts"), "utf8")
  const playerEntrypoint = readFileSync(join(repoRoot, "components/video-player.tsx"), "utf8")

  assert.equal(nativeBridge.includes("NativePlayerTracksEvent"), true)
  assert.equal(nativeBridge.includes("NativePlayerChaptersEvent"), true)
  assert.equal(nativeBridge.includes("NativePlayerFileLoadedEvent"), true)
  assert.equal(nativeBridge.includes('listen<NativePlayerTracksEvent>("native-player://tracks"'), true)
  assert.equal(nativeBridge.includes('listen<NativePlayerChaptersEvent>("native-player://chapters"'), true)
  assert.equal(nativeBridge.includes('listen<NativePlayerFileLoadedEvent>("native-player://file-loaded"'), true)
  assert.equal(playerEntrypoint.includes("onTracks:"), true)
  assert.equal(playerEntrypoint.includes("onChapters:"), true)
  assert.equal(playerEntrypoint.includes("onFileLoaded:"), true)
})

test("native player state exposes native surface diagnostics", () => {
  const nativeBridge = readFileSync(join(repoRoot, "lib/native-player.ts"), "utf8")
  const playerEntrypoint = readFileSync(join(repoRoot, "components/video-player.tsx"), "utf8")

  assert.equal(nativeBridge.includes("surfaceAttached: boolean"), true)
  assert.equal(nativeBridge.includes("surfaceBackend: string | null"), true)
  assert.equal(playerEntrypoint.includes("surfaceAttached: false"), true)
  assert.equal(playerEntrypoint.includes("surfaceBackend: null"), true)
})
