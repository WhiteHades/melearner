import test from "node:test"
import assert from "node:assert/strict"
import { readFileSync } from "node:fs"
import { join } from "node:path"

const repoRoot = process.cwd()

test("startup route is owned by Rust before frontend bootstrap", () => {
  const tauriBridge = readFileSync(join(repoRoot, "lib/tauri.ts"), "utf8")
  const rustEntrypoint = readFileSync(join(repoRoot, "src-tauri/src/lib.rs"), "utf8")
  const tauriConfig = JSON.parse(readFileSync(join(repoRoot, "src-tauri/tauri.conf.json"), "utf8"))

  assert.equal(tauriBridge.includes("export interface StartupRoute"), true)
  assert.equal(tauriBridge.includes('invoke<StartupRoute | null>("get_startup_route")'), true)
  assert.deepEqual(tauriConfig.app.windows, [])
  assert.equal(rustEntrypoint.includes("startup_route_from_sources"), true)
  assert.equal(rustEntrypoint.includes("build_main_window"), true)
  assert.equal(rustEntrypoint.includes("WebviewWindowBuilder::new"), true)
  assert.equal(rustEntrypoint.includes('WebviewUrl::App("index.html".into())'), true)
  assert.equal(rustEntrypoint.includes("StartupRouteState"), true)
  assert.equal(rustEntrypoint.includes("startup_route_webview_path"), false)
  assert.equal(rustEntrypoint.includes(".navigate("), false)
  assert.equal(rustEntrypoint.includes("--open-course"), true)
  assert.equal(rustEntrypoint.includes("--open-lesson"), true)
  assert.equal(rustEntrypoint.includes("MELEARNER_OPEN_COURSE_ID"), true)
})

test("frontend bootstrap does not block hydration on startup route invoke", () => {
  const appBootstrap = readFileSync(join(repoRoot, "components/app-bootstrap.tsx"), "utf8")
  const homeScreen = readFileSync(join(repoRoot, "components/home-screen.tsx"), "utf8")

  assert.equal(appBootstrap.includes("getStartupRouteWithTimeout"), true)
  assert.equal(appBootstrap.includes("Promise.race"), true)
  assert.equal(appBootstrap.includes("applyStartupRoute"), true)
  assert.equal(appBootstrap.includes("window.history.replaceState"), true)
  assert.ok(appBootstrap.indexOf("applyStartupRoute(library.courses") < appBootstrap.indexOf("hydrateLibrary(library.courses"))
  assert.equal(appBootstrap.includes("hydrateLibrary(library.courses, library.libraryPath)"), true)
  assert.equal(homeScreen.includes("getStartupRoute"), false)
  assert.equal(homeScreen.includes("startupRouteAppliedRef"), false)
})
