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
    appBootstrap.indexOf("hydrateLibrary(library.courses") <
      appBootstrap.indexOf("onHydrated?.(library)"),
    true,
    "The course store should hydrate before HomeScreen applies startup-route operations."
  )
  assert.equal(
    appBootstrap.includes("schedulePostHydrationWork(() => {"),
    true,
    "Search indexing and thumbnail hydration should be deferred until after the hydrated route can paint."
  )
  assert.equal(
    appBootstrap.includes("onHydrated?.({ courses, libraryPath: library.libraryPath })"),
    false,
    "Background thumbnail batches must not be reported as full bootstrap hydration events."
  )
  assert.equal(
    appBootstrap.indexOf("app.bootstrap.libraryLoaded") <
      appBootstrap.indexOf("schedulePostHydrationWork(() => {"),
    true,
    "Bootstrap should report a loaded library before starting background search and thumbnail work."
  )
  assert.equal(
    appBootstrap.indexOf("hydrateLibrary(library.courses") <
      appBootstrap.indexOf("schedulePostHydrationWork(() => {"),
    true,
    "External-store hydration should not be deferred behind hidden-workspace paint scheduling."
  )
  assert.equal(
    appBootstrap.includes("setCourses(library.courses)\n        setLibraryPath(library.libraryPath)"),
    false,
    "Library data and hydration state should not be split across separate store updates."
  )
})

test("bootstrap mounts with local-url-routed home screen", () => {
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
    homeScreen.includes("storeHasHydrated ? storeCourses : bootstrappedLibrary?.courses ?? storeCourses"),
    true,
    "HomeScreen should use the external store after hydration so background updates do not replay bootstrap."
  )
  assert.equal(
    homeScreen.includes("scheduleHomeStateUpdate"),
    true,
    "Installed WebKit needs bootstrap-driven HomeScreen state writes scheduled after the initial effect stack."
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
    providers.includes("NuqsAdapter"),
    false,
    "The first screen should not depend on nuqs/Suspense query state before hydration can paint."
  )
  assert.equal(
    homeScreen.includes("useQueryState"),
    false,
    "The first screen should own URL state locally instead of suspending through query-state hooks."
  )
  assert.equal(
    homeScreen.includes("readRouteState"),
    true,
    "HomeScreen should synchronously read the initial route from window.location."
  )
  assert.equal(
    homeScreen.includes("writeRouteState"),
    true,
    "HomeScreen should update the URL through window.history after React state is committed."
  )
})
