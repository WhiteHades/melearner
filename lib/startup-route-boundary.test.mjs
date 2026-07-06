import test from "node:test"
import assert from "node:assert/strict"
import { readFileSync } from "node:fs"
import { join } from "node:path"

const repoRoot = process.cwd()

test("startup route is owned by Rust before frontend bootstrap", () => {
  const tauriBridge = readFileSync(join(repoRoot, "lib/tauri.ts"), "utf8")
  const rustEntrypoint = readFileSync(join(repoRoot, "src-tauri/src/lib.rs"), "utf8")

  assert.equal(tauriBridge.includes("StartupRoute"), false)
  assert.equal(tauriBridge.includes("get_startup_route"), false)
  assert.equal(rustEntrypoint.includes("startup_route_from_sources"), true)
  assert.equal(rustEntrypoint.includes("navigate_startup_route"), true)
  assert.equal(rustEntrypoint.includes('get_webview_window("main")'), true)
  assert.equal(rustEntrypoint.includes("tauri://localhost/"), true)
  assert.equal(rustEntrypoint.includes("--open-course"), true)
  assert.equal(rustEntrypoint.includes("--open-lesson"), true)
  assert.equal(rustEntrypoint.includes("MELEARNER_OPEN_COURSE_ID"), true)
})

test("frontend bootstrap does not block hydration on startup route invoke", () => {
  const appBootstrap = readFileSync(join(repoRoot, "components/app-bootstrap.tsx"), "utf8")
  const homeScreen = readFileSync(join(repoRoot, "components/home-screen.tsx"), "utf8")

  assert.equal(appBootstrap.includes("getStartupRoute"), false)
  assert.equal(appBootstrap.includes("applyStartupRoute"), false)
  assert.equal(appBootstrap.includes("window.history.replaceState"), false)
  assert.equal(appBootstrap.includes("hydrateLibrary(library.courses, library.libraryPath)"), true)
  assert.equal(homeScreen.includes("getStartupRoute"), false)
  assert.equal(homeScreen.includes("startupRouteAppliedRef"), false)
})
