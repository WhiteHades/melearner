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
  assert.equal(playerEntrypoint.includes("boundsTimerRef"), true)
  assert.equal(playerEntrypoint.includes("window.setTimeout(() =>"), true)
  assert.equal(playerEntrypoint.includes("window.cancelAnimationFrame(boundsRafRef.current)"), false)
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
  assert.equal(playerEntrypoint.includes("const [loadRequested, setLoadRequested] = useState(true)"), true)
  assert.equal(playerEntrypoint.includes("if (!isLoaded)"), true)
  assert.equal(playerEntrypoint.includes("setLoadRequested(true)"), true)
  assert.equal(playerEntrypoint.includes("lesson.duration, lesson.id, lesson.lastPosition, lesson.path, lesson.subtitles"), false)
  assert.equal(playerEntrypoint.includes("[autoplay, isPlayable, libraryRoot, loadRequested, loadSnapshot, updateBounds]"), true)
  assert.equal(courseLayout.includes('key={`${currentLesson?.id ?? "empty-lesson"}:${currentLesson?.path ?? ""}`}'), true)
})

test("native player loads selected media paused before explicit playback", () => {
  const playerEntrypoint = readFileSync(join(repoRoot, "components/video-player.tsx"), "utf8")
  const loadEffect = sourceBetween(playerEntrypoint, "useEffect(() => {\n    if (!loadRequested", "  useEffect(() => {\n    isSeekingRef.current = false")

  assert.equal(playerEntrypoint.includes("const [loadRequested, setLoadRequested] = useState(true)"), true)
  assert.equal(playerEntrypoint.includes("const autoplayNextLoadRef = useRef(autoplay)"), true)
  assert.equal(loadEffect.includes("autoplay: shouldAutoplay"), true)
  assert.equal(loadEffect.includes("await updateBounds()"), true)
})

test("native player logs load boundary and surface diagnostics", () => {
  const playerEntrypoint = readFileSync(join(repoRoot, "components/video-player.tsx"), "utf8")

  assert.equal(playerEntrypoint.includes("frontendLog"), true)
  assert.equal(playerEntrypoint.includes('"native.player.load.start"'), true)
  assert.equal(playerEntrypoint.includes('"native.player.load.ready"'), true)
  assert.equal(playerEntrypoint.includes('"native.player.load.failed"'), true)
  assert.equal(playerEntrypoint.includes("surfaceAttached: next.surfaceAttached"), true)
  assert.equal(playerEntrypoint.includes("surfaceRenderedFrames: next.surfaceRenderedFrames"), true)
  assert.equal(playerEntrypoint.includes("audioTracks: next.audioTracks.length"), true)
  assert.equal(playerEntrypoint.includes("chapters: next.chapters.length"), true)
})

test("installed native playback verifier can assert metadata counts", () => {
  const verifier = readFileSync(join(repoRoot, "scripts/verify-installed-native-playback.sh"), "utf8")

  assert.equal(verifier.includes("MELEARNER_EXPECT_AUDIO_TRACKS"), true)
  assert.equal(verifier.includes("MELEARNER_EXPECT_SUBTITLE_TRACKS"), true)
  assert.equal(verifier.includes("MELEARNER_EXPECT_CHAPTERS"), true)
  assert.equal(verifier.includes('require_count "audio track" "audioTracks" "$expect_audio_tracks"'), true)
  assert.equal(verifier.includes('require_count "subtitle track" "subtitleTracks" "$expect_subtitle_tracks"'), true)
  assert.equal(verifier.includes('require_count "chapter" "chapters" "$expect_chapters"'), true)
  assert.equal(verifier.includes('json_number_field "surfaceRenderedFrames"'), true)
})

test("cross-platform native playback verifier checks packaged render diagnostics", () => {
  const packageJson = readFileSync(join(repoRoot, "package.json"), "utf8")
  const verifier = readFileSync(join(repoRoot, "scripts/verify-native-playback.mjs"), "utf8")

  assert.equal(packageJson.includes('"verify:native-playback": "node scripts/verify-native-playback.mjs"'), true)
  assert.equal(verifier.includes("render-api:gtk-opengl"), true)
  assert.equal(verifier.includes("render-api:appkit-opengl"), true)
  assert.equal(verifier.includes("render-api:wgl-opengl"), true)
  assert.equal(verifier.includes("native gtk render-api submitted first frame"), true)
  assert.equal(verifier.includes("native macos render-api submitted first frame"), true)
  assert.equal(verifier.includes("native windows render-api submitted first frame"), true)
  assert.equal(verifier.includes('event.message === "native.player.load.ready"'), true)
  assert.equal(verifier.includes("surfaceRenderedFrames"), true)
  assert.equal(verifier.includes("surfaceRenderWidth"), true)
  assert.equal(verifier.includes("surfaceRenderHeight"), true)
  assert.equal(verifier.includes("surfaceRenderThreadAlive"), true)
  assert.equal(verifier.includes("surfaceRenderError"), true)
  assert.equal(verifier.includes("MELEARNER_EXPECT_AUDIO_TRACKS"), true)
  assert.equal(verifier.includes("MELEARNER_EXPECT_SUBTITLE_TRACKS"), true)
  assert.equal(verifier.includes("MELEARNER_EXPECT_CHAPTERS"), true)
  assert.equal(verifier.includes("ffmpeg/ffprobe"), true)
  assert.equal(verifier.includes("lineCount("), true)
  assert.equal(verifier.includes("readSinceLine("), true)
})

test("native player configures C numeric locale before app startup", () => {
  const rustEntrypoint = readFileSync(join(repoRoot, "src-tauri/src/main.rs"), "utf8")

  assert.equal(rustEntrypoint.includes("configure_libmpv_numeric_locale();"), true)
  assert.equal(rustEntrypoint.includes("libc::setlocale(libc::LC_NUMERIC"), true)
  assert.ok(
    rustEntrypoint.indexOf("configure_libmpv_numeric_locale();") <
      rustEntrypoint.indexOf("melearner_lib::run();"),
    "libmpv locale must be configured before Tauri starts"
  )
})

test("native player configures C numeric locale before libmpv initialization", () => {
  const rustPlayer = readFileSync(join(repoRoot, "src-tauri/src/native_player.rs"), "utf8")
  const newPlayer = sourceBetween(rustPlayer, "fn new() -> Result<Self, String> {", ".map_err(|err| format!(\"failed to initialize libmpv: {err}\"))?")

  assert.equal(rustPlayer.includes("libc::setlocale(libc::LC_NUMERIC"), true)
  assert.equal(newPlayer.includes("configure_libmpv_numeric_locale();"), true)
  assert.ok(
    newPlayer.indexOf("configure_libmpv_numeric_locale();") <
      newPlayer.indexOf("Mpv::with_initializer"),
    "native player must configure locale immediately before libmpv initialization"
  )
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

test("native player track and chapter controls use native mpv metadata", () => {
  const nativeBridge = readFileSync(join(repoRoot, "lib/native-player.ts"), "utf8")
  const playerEntrypoint = readFileSync(join(repoRoot, "components/video-player.tsx"), "utf8")
  const rustPlayer = readFileSync(join(repoRoot, "src-tauri/src/native_player.rs"), "utf8")
  const tracksHandler = sourceBetween(playerEntrypoint, "onTracks: (next) => {", "onChapters:")
  const chaptersHandler = sourceBetween(playerEntrypoint, "onChapters: (next) => {", "onPosition:")
  const playerMenu = sourceBetween(playerEntrypoint, "function PlayerMenu", "function formatTrackLabel")

  assert.equal(rustPlayer.includes('MpvNode::get(&self.mpv, "track-list")'), true)
  assert.equal(rustPlayer.includes('MpvNode::get(&self.mpv, "chapter-list")'), true)
  assert.equal(rustPlayer.includes("parse_track_list"), true)
  assert.equal(rustPlayer.includes("parse_chapter_list"), true)
  assert.equal(nativeBridge.includes('invoke<NativePlayerState>("native_player_select_audio_track", { id })'), true)
  assert.equal(nativeBridge.includes('invoke<NativePlayerState>("native_player_select_subtitle_track", { id })'), true)
  assert.equal(nativeBridge.includes('invoke<NativePlayerState>("native_player_select_chapter", { id })'), true)
  assert.equal(tracksHandler.includes("audioTracks: next.audioTracks"), true)
  assert.equal(tracksHandler.includes("subtitleTracks: next.subtitleTracks"), true)
  assert.equal(tracksHandler.includes("selectedAudioTrackId: next.selectedAudioTrackId"), true)
  assert.equal(tracksHandler.includes("selectedSubtitleTrackId: next.selectedSubtitleTrackId"), true)
  assert.equal(chaptersHandler.includes("chapters: next.chapters"), true)
  assert.equal(chaptersHandler.includes("currentChapterId: next.currentChapterId"), true)
  assert.equal(playerMenu.includes("state.subtitleTracks.map"), true)
  assert.equal(playerMenu.includes('value={state.selectedSubtitleTrackId ?? "off"}'), true)
  assert.equal(playerMenu.includes('onSubtitleTrackChange(value === "off" ? null : value)'), true)
  assert.equal(playerMenu.includes("state.audioTracks.map"), true)
  assert.equal(playerMenu.includes("value={state.selectedAudioTrackId ?? state.audioTracks[0]?.id}"), true)
  assert.equal(playerMenu.includes("onValueChange={onAudioTrackChange}"), true)
  assert.equal(playerMenu.includes("state.chapters.map"), true)
  assert.equal(playerMenu.includes("onSelect={() => onChapterChange(chapter.id)}"), true)
})

test("native frame step applies returned player state", () => {
  const playerEntrypoint = readFileSync(join(repoRoot, "components/video-player.tsx"), "utf8")
  const stepFrame = sourceBetween(playerEntrypoint, "const stepFrame = useCallback", "const toggleFullscreen = useCallback")

  assert.equal(stepFrame.includes("applyNativeState(stepNativePlayerFrame)"), true)
  assert.equal(playerEntrypoint.includes('label="Step frame"'), true)
  assert.equal(playerEntrypoint.includes("onClick={stepFrame}"), true)
  assert.equal(playerEntrypoint.includes("void stepNativePlayerFrame().catch"), false)
})

test("native screenshot confirms saved output path", () => {
  const playerEntrypoint = readFileSync(join(repoRoot, "components/video-player.tsx"), "utf8")
  const takeScreenshot = sourceBetween(playerEntrypoint, "const takeScreenshot = useCallback", "const toggleFullscreen = useCallback")

  assert.equal(takeScreenshot.includes("takeNativePlayerScreenshot()"), true)
  assert.equal(takeScreenshot.includes(".then((path) => {"), true)
  assert.equal(takeScreenshot.includes("setStatusMessage(`Screenshot saved to ${path}`)"), true)
  assert.equal(takeScreenshot.includes("setStatusMessage(null)"), true)
  assert.equal(playerEntrypoint.includes("aria-live=\"polite\""), true)
  assert.equal(playerEntrypoint.includes("onClick={takeScreenshot}"), true)
  assert.equal(playerEntrypoint.includes("void takeNativePlayerScreenshot().catch"), false)
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
  assert.equal(rustEntrypoint.includes('as_deref() == Some("window-handle")'), false)
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

test("native player load preserves current event loops until replacement media is accepted", () => {
  const rustPlayer = readFileSync(join(repoRoot, "src-tauri/src/native_player.rs"), "utf8")
  const loadCommand = sourceBetween(rustPlayer, "pub fn native_player_load", "#[tauri::command]\npub fn native_player_state")
  const acceptedLoad = loadCommand.indexOf("Ok((state, event_client))")
  const stopPosition = loadCommand.indexOf("stop_position_events()")
  const stopPlayback = loadCommand.indexOf("stop_playback_events()")
  const startPosition = loadCommand.indexOf("start_position_events(app.clone())")
  const startPlayback = loadCommand.indexOf("start_playback_events(app, event_client)")

  assert.notEqual(acceptedLoad, -1)
  assert.ok(stopPosition > acceptedLoad, "position events should stop only after replacement media is accepted")
  assert.ok(stopPlayback > acceptedLoad, "playback events should stop only after replacement media is accepted")
  assert.ok(stopPosition < startPosition, "old position loop should stop before the new one starts")
  assert.ok(stopPlayback < startPlayback, "old playback loop should stop before the new one starts")
})

test("native player requests a native surface render after accepted media load", () => {
  const rustPlayer = readFileSync(join(repoRoot, "src-tauri/src/native_player.rs"), "utf8")
  const surfaceBackend = readFileSync(join(repoRoot, "src-tauri/src/native_player/surface.rs"), "utf8")
  const linuxGtkSurface = readFileSync(join(repoRoot, "src-tauri/src/native_player/surface/linux_gtk.rs"), "utf8")
  const loadMethod = sourceBetween(rustPlayer, "    fn load(\n", "    fn load_visible")

  assert.equal(loadMethod.includes("surface.request_render()?"), true)
  assert.equal(surfaceBackend.includes("pub(super) fn request_render"), true)
  assert.equal(linuxGtkSurface.includes("pub(super) fn request_render"), true)
  assert.equal(linuxGtkSurface.includes("surface.request_render()"), true)
  assert.equal(linuxGtkSurface.includes("fn render_now(&self)"), true)
  assert.equal(linuxGtkSurface.includes("state.realize(&self.gl_area);"), true)
  assert.equal(linuxGtkSurface.includes("self.render_now();"), true)
  assert.equal(linuxGtkSurface.includes("state.render(&self.gl_area);"), true)
})

test("native player state exposes native surface diagnostics", () => {
  const nativeBridge = readFileSync(join(repoRoot, "lib/native-player.ts"), "utf8")
  const playerEntrypoint = readFileSync(join(repoRoot, "components/video-player.tsx"), "utf8")
  const rustPlayer = readFileSync(join(repoRoot, "src-tauri/src/native_player.rs"), "utf8")
  const surfaceBackend = readFileSync(join(repoRoot, "src-tauri/src/native_player/surface.rs"), "utf8")
  const linuxGtkSurface = readFileSync(join(repoRoot, "src-tauri/src/native_player/surface/linux_gtk.rs"), "utf8")

  assert.equal(nativeBridge.includes("surfaceAttached: boolean"), true)
  assert.equal(nativeBridge.includes("surfaceBackend: string | null"), true)
  assert.equal(nativeBridge.includes("surfaceRenderApi: boolean"), true)
  assert.equal(nativeBridge.includes("surfaceRenderThreadAlive: boolean"), true)
  assert.equal(nativeBridge.includes("surfaceRenderedFrames: number"), true)
  assert.equal(nativeBridge.includes("surfaceRenderWidth: number | null"), true)
  assert.equal(nativeBridge.includes("surfaceRenderHeight: number | null"), true)
  assert.equal(nativeBridge.includes("surfaceRenderUpdateFlags: number"), true)
  assert.equal(nativeBridge.includes("surfaceRenderError: string | null"), true)
  assert.equal(rustPlayer.includes("surface_render_api: bool"), true)
  assert.equal(rustPlayer.includes("surface_render_thread_alive: bool"), true)
  assert.equal(rustPlayer.includes("surface_rendered_frames: u64"), true)
  assert.equal(rustPlayer.includes("surface_render_width: Option<u32>"), true)
  assert.equal(rustPlayer.includes("surface_render_height: Option<u32>"), true)
  assert.equal(rustPlayer.includes("surface_render_update_flags: u64"), true)
  assert.equal(rustPlayer.includes("surface_render_error: Option<String>"), true)
  assert.equal(surfaceBackend.includes("uses_render_api"), true)
  assert.equal(surfaceBackend.includes("rendered_frames"), true)
  assert.equal(surfaceBackend.includes("last_render_width"), true)
  assert.equal(surfaceBackend.includes("last_render_height"), true)
  assert.equal(surfaceBackend.includes("last_render_update_flags"), true)
  assert.equal(surfaceBackend.includes("last_error"), true)
  assert.equal(surfaceBackend.includes("MELEARNER_SURFACE_BACKEND"), true)
  assert.equal(surfaceBackend.includes('None | Some("render-api") => Ok(Self::RenderApi)'), true)
  assert.equal(surfaceBackend.includes('RenderApi("gtk-opengl")'), true)
  assert.equal(surfaceBackend.includes('mpv.set_property("vo", "libmpv")'), true)
  assert.equal(surfaceBackend.includes("falling back to window-handle surface"), false)
  assert.equal(surfaceBackend.includes("NativeSurfaceBackend::WindowHandle"), false)
  assert.equal(surfaceBackend.includes("NativeSurfaceAttachment::WindowHandle"), false)
  assert.equal(surfaceBackend.includes("window-handle:"), false)
  assert.equal(surfaceBackend.includes('mpv.set_property("wid"'), false)
  assert.equal(surfaceBackend.includes("RenderApiSurface"), false)
  assert.equal(linuxGtkSurface.includes("mpv_render_context_create"), true)
  assert.equal(playerEntrypoint.includes("surfaceAttached: false"), true)
  assert.equal(playerEntrypoint.includes("surfaceBackend: null"), true)
  assert.equal(playerEntrypoint.includes("surfaceRenderApi: false"), true)
  assert.equal(playerEntrypoint.includes("surfaceRenderThreadAlive: false"), true)
  assert.equal(playerEntrypoint.includes("surfaceRenderedFrames: 0"), true)
  assert.equal(playerEntrypoint.includes("surfaceRenderWidth: null"), true)
  assert.equal(playerEntrypoint.includes("surfaceRenderHeight: null"), true)
  assert.equal(playerEntrypoint.includes("surfaceRenderUpdateFlags: 0"), true)
  assert.equal(playerEntrypoint.includes("surfaceRenderError: null"), true)
  assert.equal(playerEntrypoint.includes("surfaceRenderWidth: next.surfaceRenderWidth"), true)
  assert.equal(playerEntrypoint.includes("surfaceRenderHeight: next.surfaceRenderHeight"), true)
  assert.equal(playerEntrypoint.includes("surfaceRenderUpdateFlags: next.surfaceRenderUpdateFlags"), true)
})

test("native player position events refresh native surface diagnostics", () => {
  const nativeBridge = readFileSync(join(repoRoot, "lib/native-player.ts"), "utf8")
  const playerEntrypoint = readFileSync(join(repoRoot, "components/video-player.tsx"), "utf8")
  const rustPlayer = readFileSync(join(repoRoot, "src-tauri/src/native_player.rs"), "utf8")
  const positionEvent = sourceBetween(rustPlayer, "pub struct NativePlayerPositionEvent", "pub struct NativePlayerBounds")
  const onPosition = sourceBetween(playerEntrypoint, "onPosition: (next) => {", "onFileLoaded:")

  assert.equal(nativeBridge.includes("surfaceRenderThreadAlive: boolean"), true)
  assert.equal(nativeBridge.includes("surfaceRenderedFrames: number"), true)
  assert.equal(nativeBridge.includes("surfaceRenderWidth: number | null"), true)
  assert.equal(nativeBridge.includes("surfaceRenderHeight: number | null"), true)
  assert.equal(nativeBridge.includes("surfaceRenderUpdateFlags: number"), true)
  assert.equal(nativeBridge.includes("surfaceRenderError: string | null"), true)
  assert.equal(positionEvent.includes("surface_render_thread_alive: bool"), true)
  assert.equal(positionEvent.includes("surface_rendered_frames: u64"), true)
  assert.equal(positionEvent.includes("surface_render_width: Option<u32>"), true)
  assert.equal(positionEvent.includes("surface_render_height: Option<u32>"), true)
  assert.equal(positionEvent.includes("surface_render_update_flags: u64"), true)
  assert.equal(positionEvent.includes("surface_render_error: Option<String>"), true)
  assert.equal(onPosition.includes("surfaceRenderThreadAlive: next.surfaceRenderThreadAlive"), true)
  assert.equal(onPosition.includes("surfaceRenderedFrames: next.surfaceRenderedFrames"), true)
  assert.equal(onPosition.includes("surfaceRenderWidth: next.surfaceRenderWidth"), true)
  assert.equal(onPosition.includes("surfaceRenderHeight: next.surfaceRenderHeight"), true)
  assert.equal(onPosition.includes("surfaceRenderUpdateFlags: next.surfaceRenderUpdateFlags"), true)
  assert.equal(onPosition.includes("surfaceRenderError: next.surfaceRenderError"), true)
})

test("native surface backend is isolated from player command state", () => {
  const rustPlayer = readFileSync(join(repoRoot, "src-tauri/src/native_player.rs"), "utf8")
  const surfaceBackend = readFileSync(join(repoRoot, "src-tauri/src/native_player/surface.rs"), "utf8")
  const surfaceStruct = sourceBetween(surfaceBackend, "pub struct NativeVideoSurface", "enum NativeSurfaceAttachment")
  const attachmentEnum = sourceBetween(surfaceBackend, "enum NativeSurfaceAttachment", "impl NativeVideoSurface")

  assert.equal(rustPlayer.includes("mod surface;"), true)
  assert.equal(rustPlayer.includes("NativeVideoSurface"), true)
  assert.equal(rustPlayer.includes("fn build_surface_window"), false)
  assert.equal(rustPlayer.includes("fn mpv_window_handle"), false)
  assert.equal(rustPlayer.includes("RawWindowHandle"), false)
  assert.equal(surfaceBackend.includes("pub struct NativeVideoSurface"), true)
  assert.equal(surfaceStruct.includes("window: Window"), false)
  assert.equal(attachmentEnum.includes("GtkInWindow"), true)
  assert.equal(attachmentEnum.includes("WindowHandle"), false)
  assert.equal(attachmentEnum.includes("window: Window"), false)
  assert.equal(attachmentEnum.includes("RenderApi {\n        window: Window"), false)
  assert.equal(surfaceBackend.includes("window-handle:"), false)
  assert.equal(surfaceBackend.includes('mpv.set_property("wid"'), false)
})

test("linux playback does not open a separate video window", () => {
  const rustSurface = readFileSync(join(repoRoot, "src-tauri/src/native_player/surface.rs"), "utf8")
  const attach = sourceBetween(rustSurface, "pub(super) fn attach", "    fn attach_surface_to_mpv")

  assert.equal(rustSurface.includes("MELEARNER_ALLOW_OVERLAY_SURFACE"), false)
  assert.equal(attach.includes("Self::attach_gtk_in_window(parent, bounds)"), true)
  assert.equal(rustSurface.includes("fn attach_window_handle"), false)
  assert.equal(rustSurface.includes("fn attach_render_api"), false)
  assert.equal(rustSurface.includes("fn build_surface_window"), false)
  assert.equal(rustSurface.includes("tauri::WindowBuilder::new"), false)
  assert.equal(rustSurface.includes("next_surface_window_label"), false)
  assert.equal(rustSurface.includes("NativeSurfaceAttachment::WindowHandle"), false)
  assert.equal(rustSurface.includes("WindowHandle {"), false)
  assert.equal(rustSurface.includes('mpv.set_property("wid"'), false)
  assert.equal(rustSurface.includes("falling back to window-handle surface"), false)
  assert.equal(rustSurface.includes("window-handle:"), false)
  assert.equal(rustSurface.includes('"melearner video"'), false)
  assert.equal(rustSurface.includes('RenderApi("gtk-opengl")'), true)
  assert.equal(rustSurface.includes("native in-window render surface is not implemented"), true)
})

test("linux render-api surface uses gtk in-window host", () => {
  const rustSurface = readFileSync(join(repoRoot, "src-tauri/src/native_player/surface.rs"), "utf8")
  const linuxGtkSurface = readFileSync(join(repoRoot, "src-tauri/src/native_player/surface/linux_gtk.rs"), "utf8")
  const attach = sourceBetween(rustSurface, "NativeSurfaceBackendPreference::RenderApi => {", "    fn attach_surface_to_mpv")
  const requestRender = sourceBetween(linuxGtkSurface, "    fn request_render(&self)", "    fn render_now(&self)")

  assert.equal(rustSurface.includes("mod linux_gtk;"), true)
  assert.equal(rustSurface.includes('RenderApi("gtk-opengl")'), true)
  assert.equal(attach.includes("Self::attach_gtk_in_window(parent, bounds)"), true)
  assert.equal(linuxGtkSurface.includes("gtk::GLArea::new()"), true)
  assert.equal(linuxGtkSurface.includes(".default_vbox()"), true)
  assert.equal(linuxGtkSurface.includes("gtk::Overlay::new()"), true)
  assert.equal(linuxGtkSurface.includes("connect_get_child_position"), true)
  assert.equal(linuxGtkSurface.includes("layer_allocation"), true)
  assert.equal(linuxGtkSurface.includes("gtk::Rectangle::new"), true)
  assert.equal(linuxGtkSurface.includes("gl_area.set_auto_render(false)"), true)
  assert.equal(linuxGtkSurface.includes("mpv_render_context_create"), true)
  assert.equal(linuxGtkSurface.includes("mpv_render_context_set_update_callback"), true)
  assert.equal(linuxGtkSurface.includes("queue_render"), true)
  assert.equal(linuxGtkSurface.includes("fn schedule_render(&self)"), true)
  assert.equal(linuxGtkSurface.includes("Duration::from_millis(50)"), true)
  assert.equal(linuxGtkSurface.includes("gl_area.set_halign(gtk::Align::Start)"), true)
  assert.equal(linuxGtkSurface.includes("gl_area.set_valign(gtk::Align::Start)"), true)
  assert.equal(linuxGtkSurface.includes("overlay.add_overlay(gl_area)"), true)
  assert.equal(linuxGtkSurface.includes("overlay.set_overlay_pass_through(gl_area, true)"), true)
  assert.equal(linuxGtkSurface.includes("gl_area.size_allocate(&layer_allocation.borrow())"), true)
  assert.equal(linuxGtkSurface.includes("overlay.queue_resize()"), true)
  assert.equal(linuxGtkSurface.includes("gl_area.queue_resize()"), true)
  assert.equal(requestRender.includes("self.render_now();"), true)
  assert.equal(requestRender.includes("state.render(&self.gl_area)"), false)
  assert.equal(linuxGtkSurface.includes("tauri::WindowBuilder"), false)
  assert.equal(linuxGtkSurface.includes('"melearner video"'), false)
})

test("macos render-api surface uses appkit in-window host", () => {
  const rustSurface = readFileSync(join(repoRoot, "src-tauri/src/native_player/surface.rs"), "utf8")
  const macosSurface = readFileSync(join(repoRoot, "src-tauri/src/native_player/surface/macos_appkit.rs"), "utf8")

  assert.equal(rustSurface.includes("mod macos_appkit;"), true)
  assert.equal(rustSurface.includes("attach_macos_in_window"), true)
  assert.equal(rustSurface.includes('RenderApi("appkit-opengl")'), true)
  assert.equal(macosSurface.includes("NSOpenGLView"), true)
  assert.equal(macosSurface.includes("webview.addSubview(view.as_super())"), true)
  assert.equal(macosSurface.includes("mpv_render_context_create"), true)
  assert.equal(macosSurface.includes("mpv_render_context_set_update_callback"), true)
  assert.equal(macosSurface.includes("run_on_main_thread"), true)
  assert.equal(macosSurface.includes("mpv.set_property(\"wid\""), false)
  assert.equal(macosSurface.includes("tauri::WindowBuilder"), false)
  assert.equal(macosSurface.includes('"melearner video"'), false)
})

test("windows render-api surface uses child hwnd wgl in-window host", () => {
  const cargoToml = readFileSync(join(repoRoot, "src-tauri/Cargo.toml"), "utf8")
  const rustSurface = readFileSync(join(repoRoot, "src-tauri/src/native_player/surface.rs"), "utf8")
  const windowsSurface = readFileSync(join(repoRoot, "src-tauri/src/native_player/surface/windows_opengl.rs"), "utf8")
  const updateCallback = sourceBetween(windowsSurface, "unsafe extern \"C\" fn windows_mpv_update_callback", "unsafe extern \"C\" fn windows_get_proc_address")

  assert.equal(cargoToml.includes("[target.'cfg(target_os = \"windows\")'.dependencies]"), true)
  assert.equal(cargoToml.includes("Win32_Graphics_OpenGL"), true)
  assert.equal(cargoToml.includes("Win32_UI_WindowsAndMessaging"), true)
  assert.equal(rustSurface.includes("mod windows_opengl;"), true)
  assert.equal(rustSurface.includes("attach_windows_in_window"), true)
  assert.equal(rustSurface.includes('RenderApi("wgl-opengl")'), true)
  assert.equal(windowsSurface.includes("WindowsInWindowSurfaceHandle"), true)
  assert.equal(windowsSurface.includes("CreateWindowExW"), true)
  assert.equal(windowsSurface.includes("WS_CHILD | WS_VISIBLE"), true)
  assert.equal(windowsSurface.includes("wglCreateContext"), true)
  assert.equal(windowsSurface.includes("wglMakeCurrent"), true)
  assert.equal(windowsSurface.includes("iPixelType: PFD_TYPE_RGBA,"), true)
  assert.equal(windowsSurface.includes("iPixelType: PFD_TYPE_RGBA.0"), false)
  assert.equal(windowsSurface.includes("SwapBuffers"), true)
  assert.equal(windowsSurface.includes("mpv_render_context_create"), true)
  assert.equal(windowsSurface.includes("mpv_render_context_set_update_callback"), true)
  assert.equal(windowsSurface.includes("mpv_render_context_render"), true)
  assert.equal(windowsSurface.includes("WindowsRenderCallback"), true)
  assert.equal(updateCallback.includes("parent.run_on_main_thread"), true)
  assert.equal(updateCallback.includes("WINDOWS_SURFACES.with"), false)
  assert.equal(windowsSurface.includes("SetWindowPos"), true)
  assert.equal(windowsSurface.includes("ShowWindow"), true)
  assert.equal(windowsSurface.includes("DestroyWindow"), true)
  assert.equal(windowsSurface.includes("mpv.set_property(\"wid\""), false)
  assert.equal(windowsSurface.includes("tauri::WindowBuilder"), false)
  assert.equal(windowsSurface.includes('"melearner video"'), false)
})

test("native surface does not keep raw-window separate renderer", () => {
  const rustSurface = readFileSync(join(repoRoot, "src-tauri/src/native_player/surface.rs"), "utf8")

  assert.equal(rustSurface.includes("RenderApiSurface"), false)
  assert.equal(rustSurface.includes("wait_for_render_api_handles"), false)
  assert.equal(rustSurface.includes("display_handle()"), false)
  assert.equal(rustSurface.includes("window_handle()"), false)
  assert.equal(rustSurface.includes("RENDER_HANDLE_ATTEMPTS"), false)
})
