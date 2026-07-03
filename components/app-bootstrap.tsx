"use client"

import { useEffect } from "react"
import { log } from "evlog/next/client"
import { initDatabase, loadPersistedLibrary, syncLibrary } from "@/lib/database"
import { indexCourses } from "@/lib/search"
import { useCourseStore } from "@/lib/stores/course-store"
import { isTauri } from "@/lib/tauri"
import { frontendLog } from "@/lib/frontend-log"
import type { Course } from "@/types"

const t0 = typeof performance !== "undefined" ? performance.now() : 0
const t = () => (typeof performance !== "undefined" ? performance.now() - t0 : 0)

type LegacyCourseStore = {
  state?: {
    courses?: unknown
    libraryPath?: unknown
  }
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null
}

function isLegacyCourseArray(value: unknown): value is Course[] {
  return Array.isArray(value) && value.every((course) => (
    isRecord(course) &&
    typeof course.id === "string" &&
    typeof course.name === "string" &&
    typeof course.path === "string" &&
    Array.isArray(course.sections)
  ))
}

function readLegacyLibrary(): { courses: Course[]; libraryPath: string } | null {
  if (typeof window === "undefined") return null

  const raw = window.localStorage.getItem("melearner-storage")
  if (!raw) return null

  const parsed = JSON.parse(raw) as LegacyCourseStore
  const courses = parsed.state?.courses
  const libraryPath = parsed.state?.libraryPath

  if (!isLegacyCourseArray(courses) || typeof libraryPath !== "string" || libraryPath.length === 0) {
    return null
  }

  return { courses, libraryPath }
}

function removeLegacyLibrary(): void {
  if (typeof window === "undefined") return
  window.localStorage.removeItem("melearner-storage")
}

export function AppBootstrap() {
  const courses = useCourseStore((state) => state.courses)
  const hasHydrated = useCourseStore((state) => state.hasHydrated)
  const setHasHydrated = useCourseStore((state) => state.setHasHydrated)
  const setCourses = useCourseStore((state) => state.setCourses)
  const setLibraryPath = useCourseStore((state) => state.setLibraryPath)

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
    let isActive = true

    frontendLog("info", "app.bootstrap.dbInit.start", { ms: Math.round(t()) })

    initDatabase()
      .then(() => {
        if (!isActive) return
        frontendLog("info", "app.bootstrap.dbInit.done", { ms: Math.round(t()) })
        try {
          const legacy = readLegacyLibrary()
          if (legacy) {
            frontendLog("info", "app.bootstrap.legacyStorageFound", {
              coursesCount: legacy.courses.length,
              libraryPath: legacy.libraryPath,
            })
            return syncLibrary(legacy.courses, legacy.libraryPath).then(() => {
              removeLegacyLibrary()
              frontendLog("info", "app.bootstrap.legacyStorageMigrated", {
                coursesCount: legacy.courses.length,
              })
              return loadPersistedLibrary()
            })
          }
          removeLegacyLibrary()
        } catch (error) {
          frontendLog("error", "app.bootstrap.legacyStorageCleanup.failed", { error: String(error) })
          try {
            removeLegacyLibrary()
          } catch {
            // Old builds stored full libraries in localStorage. Cleanup is best-effort only.
          }
        }
        return loadPersistedLibrary()
      })
      .then((library) => {
        if (!isActive || !library) return
        setCourses(library.courses)
        setLibraryPath(library.libraryPath)
        indexCourses(library.courses)
        setHasHydrated(true)
        frontendLog("info", "app.bootstrap.libraryLoaded", {
          ms: Math.round(t()),
          coursesCount: library.courses.length,
          libraryPath: library.libraryPath,
        })
      })
      .catch((error) => {
        frontendLog("error", "app.bootstrap.dbInit.failed", {
          ms: Math.round(t()),
          error: String(error),
        })
        if (isActive) setHasHydrated(true)
      })

    return () => {
      isActive = false
    }
  }, [setCourses, setHasHydrated, setLibraryPath])

  useEffect(() => {
    if (!hasHydrated) return
    const start = t()
    indexCourses(courses)
    frontendLog("info", "app.bootstrap.indexDone", { ms: Math.round(t()), coursesCount: courses.length, durMs: Math.round(t() - start) })
  }, [courses, hasHydrated])

  return null
}
