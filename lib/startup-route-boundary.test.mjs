import test from "node:test"
import assert from "node:assert/strict"
import { readFileSync } from "node:fs"
import { join } from "node:path"

const repoRoot = process.cwd()

test("startup route is exposed through the Tauri bridge", () => {
  const tauriBridge = readFileSync(join(repoRoot, "lib/tauri.ts"), "utf8")
  const rustEntrypoint = readFileSync(join(repoRoot, "src-tauri/src/lib.rs"), "utf8")

  assert.equal(tauriBridge.includes("export interface StartupRoute"), true)
  assert.equal(tauriBridge.includes('invoke<StartupRoute | null>("get_startup_route")'), true)
  assert.equal(rustEntrypoint.includes("get_startup_route"), true)
  assert.equal(rustEntrypoint.includes("--open-course"), true)
  assert.equal(rustEntrypoint.includes("--open-lesson"), true)
})

test("bootstrap applies startup route before mounting the home screen", () => {
  const appBootstrap = readFileSync(join(repoRoot, "components/app-bootstrap.tsx"), "utf8")
  const homeScreen = readFileSync(join(repoRoot, "components/home-screen.tsx"), "utf8")

  assert.equal(appBootstrap.includes("getStartupRoute"), true)
  assert.equal(appBootstrap.includes("await applyStartupRoute(library.courses)"), true)
  assert.equal(appBootstrap.includes("window.history.replaceState"), true)
  assert.equal(appBootstrap.includes('url.searchParams.set("view", "viewer")'), true)
  assert.equal(appBootstrap.includes("const course = courses.find((course) => course.id === route.courseId && !course.missingSince)"), true)
  assert.equal(appBootstrap.includes("lessonBelongsToCourse(course, route.lessonId)"), true)
  assert.equal(appBootstrap.includes("if (startupCourseId) void markCourseAccessed(startupCourseId)"), true)
  assert.ok(appBootstrap.indexOf("await applyStartupRoute(library.courses)") < appBootstrap.indexOf("hydrateLibrary(library.courses"))
  assert.equal(homeScreen.includes("getStartupRoute"), false)
  assert.equal(homeScreen.includes("startupRouteAppliedRef"), false)
})
