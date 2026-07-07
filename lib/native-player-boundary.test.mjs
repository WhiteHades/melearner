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

test("native video surface hides when its placeholder leaves the viewport", () => {
  const playerEntrypoint = readFileSync(join(repoRoot, "components/video-player.tsx"), "utf8")

  assert.equal(playerEntrypoint.includes("IntersectionObserver"), true)
  assert.equal(playerEntrypoint.includes("setNativePlayerSurfaceVisible"), true)
  assert.equal(playerEntrypoint.includes("intersection.isIntersecting"), true)
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
  assert.equal(playerEntrypoint.includes("const [loadRequested, setLoadRequested] = useState(() => autoplay)"), true)
  assert.equal(playerEntrypoint.includes("if (!isLoaded)"), true)
  assert.equal(playerEntrypoint.includes("setLoadRequested(true)"), true)
  assert.equal(playerEntrypoint.includes("lesson.duration, lesson.id, lesson.lastPosition, lesson.path, lesson.subtitles"), false)
  assert.equal(playerEntrypoint.includes("[autoplay, isPlayable, libraryRoot, loadRequested, loadSnapshot, updateBounds]"), true)
  assert.equal(courseLayout.includes('key={`${currentLesson?.id ?? "empty-lesson"}:${currentLesson?.path ?? ""}`}'), true)
})

test("native controls do not command libmpv before media is loaded", () => {
  const playerEntrypoint = readFileSync(join(repoRoot, "components/video-player.tsx"), "utf8")
  const commitSeek = sourceBetween(playerEntrypoint, "const commitSeek = useCallback", "const changeVolume = useCallback")
  const changeVolume = sourceBetween(playerEntrypoint, "const changeVolume = useCallback", "const toggleMute = useCallback")
  const toggleMute = sourceBetween(playerEntrypoint, "const toggleMute = useCallback", "const changeRate = useCallback")
  const changeRate = sourceBetween(playerEntrypoint, "const changeRate = useCallback", "const applyNativeState = useCallback")
  const applyNativeState = sourceBetween(playerEntrypoint, "const applyNativeState = useCallback", "const changeAudioTrack = useCallback")
  const playerIconButton = sourceBetween(playerEntrypoint, "function PlayerIconButton", "function PlayerMenu")
  const playerMenu = sourceBetween(playerEntrypoint, "function PlayerMenu", "function formatTrackLabel")

  assert.equal(commitSeek.includes("if (!isLoaded) return"), true)
  assert.equal(changeVolume.includes("if (!isLoaded) return"), true)
  assert.equal(toggleMute.includes("if (!isLoaded) return"), true)
  assert.equal(changeRate.includes("if (!isLoaded) return"), true)
  assert.equal(applyNativeState.includes("if (!isLoaded) return"), true)
  assert.equal(applyNativeState.includes("action: () => Promise<NativePlayerState>"), true)
  assert.equal(playerEntrypoint.includes("applyNativeState(seekNativePlayer("), false)
  assert.equal(playerEntrypoint.includes("disabled={!isLoaded}"), true)
  assert.equal(playerIconButton.includes("disabled?: boolean"), true)
  assert.equal(playerIconButton.includes("disabled={disabled}"), true)
  assert.equal(playerMenu.includes("disabled: boolean"), true)
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

test("native player bounds sync does not initialize libmpv before load", () => {
  const rustPlayer = readFileSync(join(repoRoot, "src-tauri/src/native_player.rs"), "utf8")
  const setBoundsCommand = sourceBetween(
    rustPlayer,
    "pub async fn native_player_set_bounds",
    "#[tauri::command]\npub fn native_player_set_surface_visible"
  )
  const loadCommand = sourceBetween(
    rustPlayer,
    "pub fn native_player_load",
    "#[tauri::command]\npub fn native_player_state"
  )

  assert.equal(rustPlayer.includes("PENDING_BOUNDS"), true)
  assert.equal(setBoundsCommand.includes("remember_pending_bounds(bounds)"), true)
  assert.equal(setBoundsCommand.includes("with_existing_player"), true)
  assert.equal(setBoundsCommand.includes("with_player("), false)
  assert.equal(loadCommand.includes("current_pending_bounds()"), true)
  assert.equal(loadCommand.includes("player.set_bounds(&app, bounds)?"), true)
})

test("native player state reads do not initialize libmpv before load", () => {
  const rustPlayer = readFileSync(join(repoRoot, "src-tauri/src/native_player.rs"), "utf8")
  const stateCommand = sourceBetween(
    rustPlayer,
    "pub fn native_player_state",
    "#[tauri::command]\npub fn native_player_play"
  )

  assert.equal(stateCommand.includes("with_existing_player"), true)
  assert.equal(stateCommand.includes("unwrap_or_else(empty_state)"), true)
  assert.equal(stateCommand.includes("with_player("), false)
})

test("native player media commands require a loaded player", () => {
  const rustPlayer = readFileSync(join(repoRoot, "src-tauri/src/native_player.rs"), "utf8")
  const helper = sourceBetween(rustPlayer, "fn with_loaded_player", "fn remember_pending_bounds")
  const commandRanges = [
    ["pub fn native_player_play", "#[tauri::command]\npub fn native_player_pause"],
    ["pub fn native_player_pause", "#[tauri::command]\npub fn native_player_seek"],
    ["pub fn native_player_seek", "#[tauri::command]\npub fn native_player_set_volume"],
    ["pub fn native_player_set_volume", "#[tauri::command]\npub fn native_player_set_muted"],
    ["pub fn native_player_set_muted", "#[tauri::command]\npub fn native_player_set_rate"],
    ["pub fn native_player_set_rate", "#[tauri::command]\npub fn native_player_select_audio_track"],
    ["pub fn native_player_select_audio_track", "#[tauri::command]\npub fn native_player_select_subtitle_track"],
    ["pub fn native_player_select_subtitle_track", "#[tauri::command]\npub fn native_player_select_chapter"],
    ["pub fn native_player_select_chapter", "#[tauri::command]\npub async fn native_player_set_bounds"],
    ["pub fn native_player_step_frame", "#[tauri::command]\npub fn native_player_screenshot"],
    ["pub fn native_player_screenshot", "#[tauri::command]\npub fn native_player_destroy"],
  ]

  assert.equal(rustPlayer.includes('const PLAYER_NOT_LOADED_ERROR: &str = "native player media is not loaded";'), true)
  assert.equal(helper.includes("guard.as_mut()"), true)
  assert.equal(helper.includes("player.path.is_none()"), true)
  assert.equal(helper.includes("NativePlayer::new"), false)
  for (const [start, end] of commandRanges) {
    const command = sourceBetween(rustPlayer, start, end)
    assert.equal(command.includes("with_loaded_player"), true, `${start} should require loaded media`)
    assert.equal(command.includes("with_player("), false, `${start} should not initialize libmpv`)
  }
})

test("linux startup does not force x11 for normal hyprland launches", () => {
  const rustEntrypoint = readFileSync(join(repoRoot, "src-tauri/src/main.rs"), "utf8")

  assert.equal(rustEntrypoint.includes("configure_linux_gtk_backend"), true)
  assert.equal(rustEntrypoint.includes('std::env::var_os("GDK_BACKEND").is_some()'), true)
  assert.equal(rustEntrypoint.includes("MELEARNER_FORCE_GDK_X11"), true)
  assert.equal(rustEntrypoint.includes('as_deref() == Some("window-handle")'), true)
  assert.equal(rustEntrypoint.includes('set_var("GDK_BACKEND", "x11")'), true)
  assert.ok(
    rustEntrypoint.indexOf('std::env::var_os("GDK_BACKEND").is_some()') <
      rustEntrypoint.indexOf('set_var("GDK_BACKEND", "x11")'),
    "Linux startup should respect an existing backend and avoid unconditional XWayland"
  )
})

test("native player position polling does not re-emit full track and chapter metadata", () => {
  const rustPlayer = readFileSync(join(repoRoot, "src-tauri/src/native_player.rs"), "utf8")
  const positionLoop = sourceBetween(rustPlayer, "fn start_position_events", "fn start_playback_events")

  assert.equal(rustPlayer.includes('const EVENT_POSITION: &str = "native-player://position";'), true)
  assert.equal(positionLoop.includes("emit_native_position"), true)
  assert.equal(positionLoop.includes("emit_native_state"), false)
})

test("native player file lifecycle events come from libmpv events", () => {
  const rustPlayer = readFileSync(join(repoRoot, "src-tauri/src/native_player.rs"), "utf8")
  const loadCommand = sourceBetween(rustPlayer, "pub fn native_player_load", "#[tauri::command]\npub fn native_player_state")
  const positionLoop = sourceBetween(rustPlayer, "fn start_position_events", "fn start_playback_events")

  assert.equal(rustPlayer.includes("fn start_playback_events"), true)
  assert.equal(rustPlayer.includes(".wait_event("), true)
  assert.equal(rustPlayer.includes("Event::FileLoaded"), true)
  assert.equal(rustPlayer.includes("Event::EndFile"), true)
  assert.equal(loadCommand.includes("emit_file_loaded: true"), false)
  assert.equal(loadCommand.includes("finish_state_command(&app, result, true)"), false)
  assert.equal(positionLoop.includes("EVENT_END_FILE"), false)
})

test("native player state exposes native surface diagnostics", () => {
  const nativeBridge = readFileSync(join(repoRoot, "lib/native-player.ts"), "utf8")
  const playerEntrypoint = readFileSync(join(repoRoot, "components/video-player.tsx"), "utf8")
  const rustPlayer = readFileSync(join(repoRoot, "src-tauri/src/native_player.rs"), "utf8")
  const surfaceBackend = readFileSync(join(repoRoot, "src-tauri/src/native_player/surface.rs"), "utf8")

  assert.equal(nativeBridge.includes("surfaceAttached: boolean"), true)
  assert.equal(nativeBridge.includes("surfaceBackend: string | null"), true)
  assert.equal(nativeBridge.includes("surfaceRenderApi: boolean"), true)
  assert.equal(nativeBridge.includes("surfaceRenderThreadAlive: boolean"), true)
  assert.equal(nativeBridge.includes("surfaceRenderedFrames: number"), true)
  assert.equal(nativeBridge.includes("surfaceRenderError: string | null"), true)
  assert.equal(rustPlayer.includes("surface_render_api: bool"), true)
  assert.equal(rustPlayer.includes("surface_render_thread_alive: bool"), true)
  assert.equal(rustPlayer.includes("surface_rendered_frames: u64"), true)
  assert.equal(rustPlayer.includes("surface_render_error: Option<String>"), true)
  assert.equal(surfaceBackend.includes("uses_render_api"), true)
  assert.equal(surfaceBackend.includes("rendered_frames"), true)
  assert.equal(surfaceBackend.includes("last_error"), true)
  assert.equal(surfaceBackend.includes("MELEARNER_SURFACE_BACKEND"), true)
  assert.equal(surfaceBackend.includes('None | Some("render-api") => Ok(Self::RenderApi)'), true)
  assert.equal(surfaceBackend.includes('RenderApi("opengl")'), true)
  assert.equal(surfaceBackend.includes('mpv.set_property("vo", "libmpv")'), true)
  assert.equal(surfaceBackend.includes("falling back to window-handle surface"), true)
  assert.equal(surfaceBackend.includes("create_render_context"), true)
  assert.equal(playerEntrypoint.includes("surfaceAttached: false"), true)
  assert.equal(playerEntrypoint.includes("surfaceBackend: null"), true)
  assert.equal(playerEntrypoint.includes("surfaceRenderApi: false"), true)
  assert.equal(playerEntrypoint.includes("surfaceRenderThreadAlive: false"), true)
  assert.equal(playerEntrypoint.includes("surfaceRenderedFrames: 0"), true)
  assert.equal(playerEntrypoint.includes("surfaceRenderError: null"), true)
})

test("native player position events refresh native surface diagnostics", () => {
  const nativeBridge = readFileSync(join(repoRoot, "lib/native-player.ts"), "utf8")
  const playerEntrypoint = readFileSync(join(repoRoot, "components/video-player.tsx"), "utf8")
  const rustPlayer = readFileSync(join(repoRoot, "src-tauri/src/native_player.rs"), "utf8")
  const positionEvent = sourceBetween(rustPlayer, "pub struct NativePlayerPositionEvent", "pub struct NativePlayerBounds")
  const onPosition = sourceBetween(playerEntrypoint, "onPosition: (next) => {", "onFileLoaded:")

  assert.equal(nativeBridge.includes("surfaceRenderThreadAlive: boolean"), true)
  assert.equal(nativeBridge.includes("surfaceRenderedFrames: number"), true)
  assert.equal(nativeBridge.includes("surfaceRenderError: string | null"), true)
  assert.equal(positionEvent.includes("surface_render_thread_alive: bool"), true)
  assert.equal(positionEvent.includes("surface_rendered_frames: u64"), true)
  assert.equal(positionEvent.includes("surface_render_error: Option<String>"), true)
  assert.equal(onPosition.includes("surfaceRenderThreadAlive: next.surfaceRenderThreadAlive"), true)
  assert.equal(onPosition.includes("surfaceRenderedFrames: next.surfaceRenderedFrames"), true)
  assert.equal(onPosition.includes("surfaceRenderError: next.surfaceRenderError"), true)
})

test("native surface backend is isolated from player command state", () => {
  const rustPlayer = readFileSync(join(repoRoot, "src-tauri/src/native_player.rs"), "utf8")
  const surfaceBackend = readFileSync(join(repoRoot, "src-tauri/src/native_player/surface.rs"), "utf8")

  assert.equal(rustPlayer.includes("mod surface;"), true)
  assert.equal(rustPlayer.includes("NativeVideoSurface"), true)
  assert.equal(rustPlayer.includes("fn build_surface_window"), false)
  assert.equal(rustPlayer.includes("fn mpv_window_handle"), false)
  assert.equal(rustPlayer.includes("RawWindowHandle"), false)
  assert.equal(surfaceBackend.includes("pub struct NativeVideoSurface"), true)
  assert.equal(surfaceBackend.includes("window-handle:"), true)
})
