"use client"

import { useEffect } from "react"
import { initDatabase } from "@/lib/database"
import { indexCourses } from "@/lib/search"
import { useCourseStore } from "@/lib/stores/course-store"

export function AppBootstrap() {
  const courses = useCourseStore((state) => state.courses)
  const hasHydrated = useCourseStore((state) => state.hasHydrated)
  const setHasHydrated = useCourseStore((state) => state.setHasHydrated)

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
