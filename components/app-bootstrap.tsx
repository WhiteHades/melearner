"use client"

import { useEffect } from "react"
import { log } from "evlog/next/client"
import { initDatabase, loadPersistedLibrary } from "@/lib/database"
import { indexCourses } from "@/lib/search"
import { useCourseStore } from "@/lib/stores/course-store"
import { getStartupRoute, isTauri, type StartupRoute } from "@/lib/tauri"
import { frontendLog } from "@/lib/frontend-log"
import type { Course } from "@/types"

const t0 = typeof performance !== "undefined" ? performance.now() : 0
const t = () => (typeof performance !== "undefined" ? performance.now() - t0 : 0)
const STARTUP_ROUTE_TIMEOUT_MS = 500
const STARTUP_ROUTE_TIMEOUT = Symbol("startup-route-timeout")

type HydratedLibrary = Awaited<ReturnType<typeof loadPersistedLibrary>>

declare global {
  interface Window {
    __MELEARNER_STARTUP_ROUTE__?: StartupRoute | null
  }
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null
}

function schedulePostHydrationWork(work: () => void): void {
  if (typeof window === "undefined") {
    work()
    return
  }

  window.setTimeout(work, 0)
}

function lessonBelongsToCourse(course: Course, lessonId: string | null): lessonId is string {
  if (!lessonId) return false
  return course.sections.some((section) => section.lessons.some((lesson) => lesson.id === lessonId))
}

function startupRouteTimeout(): Promise<typeof STARTUP_ROUTE_TIMEOUT> {
  return new Promise((resolve) => {
    window.setTimeout(() => resolve(STARTUP_ROUTE_TIMEOUT), STARTUP_ROUTE_TIMEOUT_MS)
  })
}

function readInitializedStartupRoute(): StartupRoute | null {
  if (typeof window === "undefined") return null

  const route = window.__MELEARNER_STARTUP_ROUTE__
  window.__MELEARNER_STARTUP_ROUTE__ = null

  if (
    !isRecord(route) ||
    typeof route.courseId !== "string" ||
    (route.lessonId !== null && typeof route.lessonId !== "string")
  ) {
    return null
  }

  return {
    courseId: route.courseId,
    lessonId: route.lessonId,
  }
}

async function getStartupRouteWithTimeout(): Promise<StartupRoute | null> {
  const initializedRoute = readInitializedStartupRoute()
  if (initializedRoute) return initializedRoute

  if (!isTauri() || typeof window === "undefined") return null

  try {
    const route = await Promise.race([getStartupRoute(), startupRouteTimeout()])
    if (route === STARTUP_ROUTE_TIMEOUT) {
      frontendLog("warn", "startup.route.timeout", { timeoutMs: STARTUP_ROUTE_TIMEOUT_MS })
      return null
    }
    return route
  } catch (error) {
    frontendLog("warn", "startup.route.failed", { error: String(error) })
    return null
  }
}

function queueStartupRoute(
  courses: Course[],
  route: StartupRoute | null,
  applyStartupRoute: (route: StartupRoute | null) => void,
): string | null {
  if (!route || courses.length === 0) return null

  const course = courses.find((course) => course.id === route.courseId && !course.missingSince)
  if (!course) {
    frontendLog("warn", "startup.route.courseMissing", { courseId: route.courseId })
    return null
  }

  const selectedLessonId = lessonBelongsToCourse(course, route.lessonId) ? route.lessonId : null
  if (route.lessonId && !selectedLessonId) {
    frontendLog("warn", "startup.route.lessonMissing", {
      courseId: route.courseId,
      lessonId: route.lessonId,
    })
  }

  applyStartupRoute({ courseId: course.id, lessonId: selectedLessonId })
  frontendLog("info", "startup.route.queued", { courseId: course.id, lessonId: selectedLessonId })
  return course.id
}

async function resolveStartupRouteBeforeHydration(
  courses: Course[],
  applyStartupRoute: (route: StartupRoute | null) => void,
): Promise<void> {
  queueStartupRoute(courses, await getStartupRouteWithTimeout(), applyStartupRoute)
}

export function useAppBootstrap({
  onHydrated,
  onStartupRoute,
}: {
  onHydrated?: (library: HydratedLibrary) => void
  onStartupRoute?: (route: StartupRoute | null) => void
}) {
  const setHasHydrated = useCourseStore((state) => state.setHasHydrated)
  const hydrateLibrary = useCourseStore((state) => state.hydrateLibrary)

  useEffect(() => {
    frontendLog("info", "app.bootstrap", {
      ms: Math.round(t()),
      isTauri: isTauri(),
      userAgent: typeof navigator !== "undefined" ? navigator.userAgent : "no-navigator",
      url: typeof window !== "undefined" ? window.location.href : "no-window",
      documentReadyState:
        typeof document !== "undefined" ? document.readyState : "no-document",
    })
    log.info({
      action: "app.bootstrap",
      runtime: {
        ms: Math.round(t()),
        isTauri: isTauri(),
        userAgent: typeof navigator !== "undefined" ? navigator.userAgent : "no-navigator",
        url: typeof window !== "undefined" ? window.location.href : "no-window",
        documentReadyState:
          typeof document !== "undefined" ? document.readyState : "no-document",
      },
    })

    if (typeof window !== "undefined") {
      const onError = (event: ErrorEvent) => {
        frontendLog("error", "app.error", {
          message: event.message,
          filename: event.filename,
          line: event.lineno,
          col: event.colno,
          stack: event.error instanceof Error ? event.error.stack : undefined,
        })
        log.error({
          action: "app.error",
          message: event.message,
          filename: event.filename,
          line: event.lineno,
          col: event.colno,
          stack: event.error instanceof Error ? event.error.stack : undefined,
        })
      }
      const onUnhandledRejection = (event: PromiseRejectionEvent) => {
        frontendLog("error", "app.unhandledRejection", {
          reason:
            event.reason instanceof Error
              ? { message: event.reason.message, stack: event.reason.stack }
              : event.reason,
        })
        log.error({
          action: "app.unhandledRejection",
          reason:
            event.reason instanceof Error
              ? { message: event.reason.message, stack: event.reason.stack }
              : event.reason,
        })
      }
      window.addEventListener("error", onError)
      window.addEventListener("unhandledrejection", onUnhandledRejection)

      const onLoad = () => {
        frontendLog("info", "window.load", { ms: Math.round(t()) })
      }
      if (document.readyState === "complete") {
        onLoad()
      } else {
        window.addEventListener("load", onLoad, { once: true })
      }

      return () => {
        window.removeEventListener("error", onError)
        window.removeEventListener("unhandledrejection", onUnhandledRejection)
        window.removeEventListener("load", onLoad)
      }
    }
  }, [])

  useEffect(() => {
    void (async () => {
      frontendLog("info", "app.bootstrap.dbInit.start", { ms: Math.round(t()) })
      await initDatabase()
      frontendLog("info", "app.bootstrap.dbInit.done", { ms: Math.round(t()) })

      frontendLog("info", "app.bootstrap.libraryLoad.start", { ms: Math.round(t()) })
      const library = await loadPersistedLibrary()
      hydrateLibrary(library.courses, library.libraryPath)
      await resolveStartupRouteBeforeHydration(library.courses, (route) => {
        onStartupRoute?.(route)
      })
      onHydrated?.(library)
      frontendLog("info", "app.bootstrap.libraryLoad.done", {
        ms: Math.round(t()),
        coursesCount: library.courses.length,
        libraryPath: library.libraryPath,
      })
      frontendLog("info", "app.bootstrap.libraryLoaded", {
        ms: Math.round(t()),
        coursesCount: library.courses.length,
        libraryPath: library.libraryPath,
      })
      schedulePostHydrationWork(() => {
        void indexCourses(library.courses, library.libraryPath).catch((error) => {
          frontendLog("warn", "app.bootstrap.searchIndex.failed", { error: String(error) })
        })
      })
    })()
      .catch((error) => {
        frontendLog("error", "app.bootstrap.dbInit.failed", {
          ms: Math.round(t()),
          error: String(error),
        })
        setHasHydrated(true)
      })
  }, [hydrateLibrary, onHydrated, onStartupRoute, setHasHydrated])

}
