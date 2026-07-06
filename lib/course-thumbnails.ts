"use client"

import { convertFileSrc } from "@tauri-apps/api/core"
import type { Course } from "@/types"
import { frontendLog } from "@/lib/frontend-log"
import { generateVideoThumbnail, isTauri } from "@/lib/tauri"

let thumbnailRunId = 0
const THUMBNAIL_UPDATE_BATCH_SIZE = 4

export async function hydrateCourseThumbnails(
  courses: Course[],
  onUpdate: (courses: Course[]) => void
): Promise<void> {
  if (!isTauri()) return

  const runId = ++thumbnailRunId
  let nextCourses = courses
  let pendingHydrated = 0

  function flushHydratedThumbnails() {
    if (runId !== thumbnailRunId || pendingHydrated === 0) return
    onUpdate(nextCourses)
    pendingHydrated = 0
  }

  for (const course of courses) {
    if (runId !== thumbnailRunId) return
    if (course.thumbnail || course.missingSince || !course.thumbnailSourcePath) continue

    try {
      const thumbnail = await generateVideoThumbnail(course.thumbnailSourcePath)
      if (runId !== thumbnailRunId) return
      const thumbnailUrl = convertFileSrc(thumbnail.path)
      let changed = false

      nextCourses = nextCourses.map((existing) => {
        if (existing.id !== course.id || existing.thumbnailSourcePath !== course.thumbnailSourcePath) {
          return existing
        }
        changed = true
        return { ...existing, thumbnail: thumbnailUrl }
      })

      if (changed) {
        pendingHydrated += 1
        if (pendingHydrated >= THUMBNAIL_UPDATE_BATCH_SIZE) flushHydratedThumbnails()
      }
    } catch (error) {
      frontendLog("warn", "course.thumbnail.failed", {
        courseId: course.id,
        path: course.thumbnailSourcePath,
        error: String(error),
      })
    }
  }

  flushHydratedThumbnails()
}
