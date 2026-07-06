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

test("home screen applies startup route after hydrated library validation", () => {
  const homeScreen = readFileSync(join(repoRoot, "components/home-screen.tsx"), "utf8")

  assert.equal(homeScreen.includes("getStartupRoute"), true)
  assert.equal(homeScreen.includes("startupRouteAppliedRef"), true)
  assert.equal(homeScreen.includes("if (!hasHydrated || !isTauri() || startupRouteAppliedRef.current) return"), true)
  assert.equal(homeScreen.includes("const course = courses.find((course) => course.id === route.courseId && !course.missingSince)"), true)
  assert.equal(homeScreen.includes("lessonBelongsToCourse(course, route.lessonId)"), true)
  assert.equal(homeScreen.includes('setViewParam("viewer")'), true)
  assert.equal(homeScreen.includes("markCourseAccessed(course.id)"), true)
})
