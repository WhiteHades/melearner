import test from "node:test"
import assert from "node:assert/strict"
import { readFileSync } from "node:fs"
import { join } from "node:path"

const repoRoot = process.cwd()

test("bootstrap applies completed persisted-library hydration", () => {
  const appBootstrap = readFileSync(join(repoRoot, "components/app-bootstrap.tsx"), "utf8")

  assert.equal(
    appBootstrap.includes("app.bootstrap.libraryLoad.done"),
    true,
    "Bootstrap should log when persisted-library hydration completes."
  )
  assert.equal(
    appBootstrap.includes("if (!isActive || !library) return"),
    false,
    "Completed persisted-library hydration must not be discarded after a route remount."
  )
  assert.equal(
    appBootstrap.includes("hydrateLibrary(library.courses, library.libraryPath)"),
    true,
    "Completed persisted-library hydration should populate the course store atomically."
  )
  assert.equal(
    appBootstrap.indexOf("onHydrated?.(library)") <
      appBootstrap.indexOf("hydrateLibrary(library.courses"),
    true,
    "The visible HomeScreen handoff should happen before external-store hydration work."
  )
  assert.equal(
    appBootstrap.includes("schedulePostHydrationWork(() => {"),
    true,
    "Search indexing and thumbnail hydration should be deferred until after the hydrated route can paint."
  )
  assert.equal(
    appBootstrap.indexOf("app.bootstrap.libraryLoaded") <
      appBootstrap.indexOf("schedulePostHydrationWork(() => {"),
    true,
    "Bootstrap should report a loaded library before starting background search and thumbnail work."
  )
  assert.equal(
    appBootstrap.indexOf("schedulePostHydrationWork(() => {") <
      appBootstrap.indexOf("hydrateLibrary(library.courses"),
    true,
    "External-store hydration should be deferred with the other post-hydration work."
  )
  assert.equal(
    appBootstrap.includes("setCourses(library.courses)\n        setLibraryPath(library.libraryPath)"),
    false,
    "Library data and hydration state should not be split across separate store updates."
  )
})

test("bootstrap mounts with query-routed home screen", () => {
  const providers = readFileSync(join(repoRoot, "components/app-providers.tsx"), "utf8")
  const homeScreen = readFileSync(join(repoRoot, "components/home-screen.tsx"), "utf8")
  const appBootstrap = readFileSync(join(repoRoot, "components/app-bootstrap.tsx"), "utf8")

  assert.equal(
    providers.includes("<AppBootstrap />"),
    false,
    "AppBootstrap should not live in the provider chunk because that can create a separate course-store instance."
  )
  assert.equal(
    appBootstrap.includes("export function useAppBootstrap"),
    true,
    "Bootstrap should be exposed as a hook so HomeScreen owns hydration state updates."
  )
  assert.equal(
    homeScreen.includes("useAppBootstrap({"),
    true,
    "HomeScreen should run the bootstrap hook directly instead of receiving updates from a child component."
  )
  assert.equal(
    homeScreen.includes("<AppBootstrap"),
    false,
    "HomeScreen should not rely on a rendered bootstrap child to update parent hydration state."
  )
  assert.equal(
    homeScreen.includes("setBootstrappedLibrary"),
    true,
    "HomeScreen should receive hydrated library data through React state, not only through the external store."
  )
  assert.equal(
    homeScreen.includes("flushSync"),
    false,
    "The bootstrap handoff should not synchronously flush React state from startup effects."
  )
  assert.equal(
    providers.includes("hasHydrated ? ("),
    false,
    "The provider must not keep a top-level loading fallback that can get stuck after hydration."
  )
  assert.equal(
    providers.includes("<NuqsAdapter>"),
    true,
    "Query-routed UI should mount and let HomeScreen own loading state."
  )
})
