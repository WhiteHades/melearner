"use client"

import { useEffect } from "react"
import { log } from "evlog/next/client"
import { initDatabase } from "@/lib/database"
import { indexCourses } from "@/lib/search"
import { useCourseStore } from "@/lib/stores/course-store"
import { isTauri } from "@/lib/tauri"
import { frontendLog } from "@/lib/frontend-log"

export function AppBootstrap() {
  const courses = useCourseStore((state) => state.courses)
  const hasHydrated = useCourseStore((state) => state.hasHydrated)
  const setHasHydrated = useCourseStore((state) => state.setHasHydrated)

  useEffect(() => {
    frontendLog("info", "app.bootstrap", {
      isTauri: isTauri(),
      userAgent: typeof navigator !== "undefined" ? navigator.userAgent : "no-navigator",
      url: typeof window !== "undefined" ? window.location.href : "no-window",
      documentReadyState:
        typeof document !== "undefined" ? document.readyState : "no-document",
    })
    log.info({
      action: "app.bootstrap",
      runtime: {
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
      return () => {
        window.removeEventListener("error", onError)
        window.removeEventListener("unhandledrejection", onUnhandledRejection)
      }
    }
  }, [])

  useEffect(() => {
    let isActive = true

    initDatabase().catch((error) => {
      console.error("Failed to initialize database", error)
    })

    const stopListening = useCourseStore.persist.onFinishHydration(() => {
      if (!isActive) return
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
    indexCourses(courses)
  }, [courses, hasHydrated])

  return null
}
