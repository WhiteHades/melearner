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
    "Search indexing should be deferred until after the hydrated route can paint."
  )
  assert.equal(
    appBootstrap.includes("hydrateCourseThumbnails"),
    false,
    "Bootstrap should not start course-card thumbnail work while a startup route is opening the viewer."
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
    homeScreen.includes("hydrateCourseThumbnails(sourceCourses, setCourses)"),
    true,
    "Course-card thumbnail hydration should run from the mounted library dashboard, not app bootstrap."
  )
  assert.equal(
    homeScreen.includes("thumbnailSourceKey"),
    true,
    "Course-card thumbnail hydration should be keyed by source paths rather than thumbnail updates."
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

test("startup auto-scan hook is explicit and uses the normal scan path", () => {
  const homeScreen = readFileSync(join(repoRoot, "components/home-screen.tsx"), "utf8")
  const rustEntrypoint = readFileSync(join(repoRoot, "src-tauri/src/lib.rs"), "utf8")

  assert.equal(
    rustEntrypoint.includes("startup_auto_scan_path_from_runtime()"),
    true,
    "Startup auto-scan must be an explicit runtime option, not an unconditional app-start scan."
  )
  assert.equal(
    rustEntrypoint.includes("MELEARNER_AUTO_SCAN_PATH"),
    true,
    "Packaged diagnostics should be able to request an app-path scan through an environment variable."
  )
  assert.equal(
    rustEntrypoint.includes("--auto-scan"),
    true,
    "Packaged diagnostics should be able to request an app-path scan through a CLI argument."
  )
  assert.equal(
    rustEntrypoint.includes("window.__MELEARNER_AUTO_SCAN_PATH__"),
    true,
    "The startup init script should pass only the explicit scan path into the WebView."
  )
  assert.equal(
    homeScreen.includes("function readAutoScanPath()"),
    true,
    "HomeScreen should centralize startup and query auto-scan parsing."
  )
  assert.equal(
    homeScreen.includes("window.__MELEARNER_AUTO_SCAN_PATH__ = null"),
    true,
    "Startup auto-scan should be consumed once."
  )
  assert.equal(
    homeScreen.includes("scanLibraryAt(path)"),
    true,
    "Startup auto-scan should use the same scan/sync operation as the refresh button."
  )
})
