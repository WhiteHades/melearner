"use client"

import { useEffect } from "react"
import { log } from "evlog/next/client"
import { initDatabase } from "@/lib/database"
import { indexCourses } from "@/lib/search"
import { useCourseStore } from "@/lib/stores/course-store"
import { isTauri } from "@/lib/tauri"
import { frontendLog } from "@/lib/frontend-log"

const t0 = typeof performance !== "undefined" ? performance.now() : 0
const t = () => (typeof performance !== "undefined" ? performance.now() - t0 : 0)

export function AppBootstrap() {
  const courses = useCourseStore((state) => state.courses)
  const hasHydrated = useCourseStore((state) => state.hasHydrated)
  const setHasHydrated = useCourseStore((state) => state.setHasHydrated)

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
        if (isActive) {
          frontendLog("info", "app.bootstrap.dbInit.done", { ms: Math.round(t()) })
        }
      })
      .catch((error) => {
        frontendLog("error", "app.bootstrap.dbInit.failed", {
          ms: Math.round(t()),
          error: String(error),
        })
      })

    const stopListening = useCourseStore.persist.onFinishHydration(() => {
      if (!isActive) return
      frontendLog("info", "app.bootstrap.zustandHydrated", { ms: Math.round(t()) })
      setHasHydrated(true)
    })

    if (useCourseStore.persist.hasHydrated()) {
      indexCourses(useCourseStore.getState().courses)
      setHasHydrated(true)
    }

    return () => {
      isActive = false
      stopListening()
    }
  }, [setHasHydrated])

  useEffect(() => {
    if (!hasHydrated) return
    const start = t()
    indexCourses(courses)
    frontendLog("info", "app.bootstrap.indexDone", { ms: Math.round(t()), coursesCount: courses.length, durMs: Math.round(t() - start) })
  }, [courses, hasHydrated])

  return null
}
