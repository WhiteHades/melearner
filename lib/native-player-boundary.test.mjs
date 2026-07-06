import test from "node:test"
import assert from "node:assert/strict"
import { existsSync, readFileSync } from "node:fs"
import { join } from "node:path"

const repoRoot = process.cwd()

function sourceBetween(source, start, end) {
  const startIndex = source.indexOf(start)
  assert.notEqual(startIndex, -1, `${start} should exist`)
  const endIndex = source.indexOf(end, startIndex + start.length)
  assert.notEqual(endIndex, -1, `${end} should exist after ${start}`)
  return source.slice(startIndex, endIndex)
}

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
  assert.equal(playerEntrypoint.includes('window.addEventListener("scroll", requestBoundsUpdate, true)'), true)
  assert.equal(playerEntrypoint.includes("window.requestAnimationFrame"), true)
})

test("native player fullscreen uses Tauri window fullscreen", () => {
  const playerEntrypoint = readFileSync(join(repoRoot, "components/video-player.tsx"), "utf8")

  assert.equal(playerEntrypoint.includes(".isFullscreen()"), true)
  assert.equal(playerEntrypoint.includes(".setFullscreen(nextFullscreen)"), true)
  assert.equal(playerEntrypoint.includes("surface.requestFullscreen"), false)
  assert.equal(playerEntrypoint.includes("document.exitFullscreen"), false)
  assert.equal(playerEntrypoint.includes('document.addEventListener("fullscreenchange"'), false)
})

test("native player interpolates visible position between coarse native events", () => {
  const playerEntrypoint = readFileSync(join(repoRoot, "components/video-player.tsx"), "utf8")

  assert.equal(playerEntrypoint.includes("visibleCurrentTime"), true)
  assert.equal(playerEntrypoint.includes("positionRafRef"), true)
  assert.equal(playerEntrypoint.includes("performance.now()"), true)
  assert.equal(playerEntrypoint.includes("state.rate"), true)
  assert.equal(playerEntrypoint.includes("isSeekingRef"), true)
})

test("native player keeps documented keyboard playback shortcuts", () => {
  const playerEntrypoint = readFileSync(join(repoRoot, "components/video-player.tsx"), "utf8")

  assert.equal(playerEntrypoint.includes('document.addEventListener("keydown", handlePlayerKeyDown)'), true)
  assert.equal(playerEntrypoint.includes('case "Space":'), true)
  assert.equal(playerEntrypoint.includes('case "KeyK":'), true)
  assert.equal(playerEntrypoint.includes('case "KeyM":'), true)
  assert.equal(playerEntrypoint.includes('case "KeyF":'), true)
  assert.equal(playerEntrypoint.includes('case "KeyJ":'), true)
  assert.equal(playerEntrypoint.includes('case "ArrowLeft":'), true)
  assert.equal(playerEntrypoint.includes('case "KeyL":'), true)
  assert.equal(playerEntrypoint.includes('case "ArrowRight":'), true)
  assert.equal(playerEntrypoint.includes('mode: "relative"'), true)
})

test("native player does not reload media on progress-only lesson updates", () => {
  const playerEntrypoint = readFileSync(join(repoRoot, "components/video-player.tsx"), "utf8")
  const courseLayout = readFileSync(join(repoRoot, "components/course-viewer/layout.tsx"), "utf8")

  assert.equal(playerEntrypoint.includes("const [loadSnapshot] = useState"), true)
  assert.equal(playerEntrypoint.includes("lesson.duration, lesson.id, lesson.lastPosition, lesson.path, lesson.subtitles"), false)
  assert.equal(playerEntrypoint.includes("[autoplay, isPlayable, libraryRoot, loadSnapshot, updateBounds]"), true)
  assert.equal(courseLayout.includes('key={`${currentLesson?.id ?? "empty-lesson"}:${currentLesson?.path ?? ""}`}'), true)
})

test("native player bridge exposes typed track chapter and file-loaded events", () => {
  const nativeBridge = readFileSync(join(repoRoot, "lib/native-player.ts"), "utf8")
  const playerEntrypoint = readFileSync(join(repoRoot, "components/video-player.tsx"), "utf8")

  assert.equal(nativeBridge.includes("NativePlayerTracksEvent"), true)
  assert.equal(nativeBridge.includes("NativePlayerChaptersEvent"), true)
  assert.equal(nativeBridge.includes("NativePlayerFileLoadedEvent"), true)
  assert.equal(nativeBridge.includes("NativePlayerPositionEvent"), true)
  assert.equal(nativeBridge.includes('listen<NativePlayerTracksEvent>("native-player://tracks"'), true)
  assert.equal(nativeBridge.includes('listen<NativePlayerChaptersEvent>("native-player://chapters"'), true)
  assert.equal(nativeBridge.includes('listen<NativePlayerFileLoadedEvent>("native-player://file-loaded"'), true)
  assert.equal(nativeBridge.includes('listen<NativePlayerPositionEvent>("native-player://position"'), true)
  assert.equal(playerEntrypoint.includes("onTracks:"), true)
  assert.equal(playerEntrypoint.includes("onChapters:"), true)
  assert.equal(playerEntrypoint.includes("onFileLoaded:"), true)
  assert.equal(playerEntrypoint.includes("onPosition:"), true)
})

test("native player bridge exposes native surface visibility control", () => {
  const nativeBridge = readFileSync(join(repoRoot, "lib/native-player.ts"), "utf8")
  const rustPlayer = readFileSync(join(repoRoot, "src-tauri/src/native_player.rs"), "utf8")
  const tauriEntrypoint = readFileSync(join(repoRoot, "src-tauri/src/lib.rs"), "utf8")

  assert.equal(nativeBridge.includes("setNativePlayerSurfaceVisible"), true)
  assert.equal(nativeBridge.includes('invoke<void>("native_player_set_surface_visible", { visible })'), true)
  assert.equal(rustPlayer.includes("pub fn native_player_set_surface_visible"), true)
  assert.equal(tauriEntrypoint.includes("native_player::native_player_set_surface_visible"), true)
})

test("native player position polling does not re-emit full track and chapter metadata", () => {
  const rustPlayer = readFileSync(join(repoRoot, "src-tauri/src/native_player.rs"), "utf8")
  const positionLoop = sourceBetween(rustPlayer, "fn start_position_events", "#[tauri::command]")

  assert.equal(rustPlayer.includes('const EVENT_POSITION: &str = "native-player://position";'), true)
  assert.equal(positionLoop.includes("emit_native_position"), true)
  assert.equal(positionLoop.includes("emit_native_state"), false)
})

test("native player state exposes native surface diagnostics", () => {
  const nativeBridge = readFileSync(join(repoRoot, "lib/native-player.ts"), "utf8")
  const playerEntrypoint = readFileSync(join(repoRoot, "components/video-player.tsx"), "utf8")

  assert.equal(nativeBridge.includes("surfaceAttached: boolean"), true)
  assert.equal(nativeBridge.includes("surfaceBackend: string | null"), true)
  assert.equal(playerEntrypoint.includes("surfaceAttached: false"), true)
  assert.equal(playerEntrypoint.includes("surfaceBackend: null"), true)
})
