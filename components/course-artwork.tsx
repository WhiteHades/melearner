"use client"

import { useEffect, useMemo, useRef, useState } from "react"
import { convertFileSrc } from "@tauri-apps/api/core"
import { generateVideoThumbnail, isTauri } from "@/lib/tauri"
import { cn } from "@/lib/utils"
import type { Course } from "@/types"

const thumbnailCache = new Map<string, string | null>()
const courseVideoCache = new Map<string, string | null>()

function randomVideoPath(course: Course): string | null {
  const cached = courseVideoCache.get(course.id)
  if (cached !== undefined) return cached

  const paths = new Set<string>()
  if (course.thumbnailSourcePath) paths.add(course.thumbnailSourcePath)

  for (const section of course.sections) {
    for (const lesson of section.lessons) {
      if (lesson.type === "video") paths.add(lesson.path)
    }
  }

  const videos = Array.from(paths)
  const selected = videos.length > 0 ? videos[Math.floor(Math.random() * videos.length)] : null
  courseVideoCache.set(course.id, selected)
  return selected
}

function buildMediaUrl(path: string): string {
  return isTauri() ? convertFileSrc(path) : path
}

async function captureVideoThumbnail(path: string): Promise<string | null> {
  if (typeof document === "undefined") return null

  const cached = thumbnailCache.get(path)
  if (cached !== undefined) return cached

  const seed = Math.random()

  if (isTauri()) {
    try {
      const bytes = await generateVideoThumbnail(path, seed)
      if (bytes.length > 0) {
        const url = URL.createObjectURL(new Blob([new Uint8Array(bytes)], { type: "image/jpeg" }))
        thumbnailCache.set(path, url)
        return url
      }
    } catch {
      // Fall back to browser capture when ffmpeg is unavailable or cannot decode a file.
    }
  }

  const video = document.createElement("video")
  video.muted = true
  video.playsInline = true
  video.preload = "metadata"
  video.crossOrigin = "anonymous"

  const result = await new Promise<string | null>((resolve) => {
    let done = false
    const finish = (value: string | null) => {
      if (done) return
      done = true
      video.removeAttribute("src")
      video.load()
      resolve(value)
    }

    const timeout = window.setTimeout(() => finish(null), 8000)

    video.addEventListener("error", () => {
      window.clearTimeout(timeout)
      finish(null)
    }, { once: true })

    video.addEventListener("loadedmetadata", () => {
      const duration = Number.isFinite(video.duration) ? video.duration : 0
      video.currentTime = duration > 12
        ? Math.min(duration - 1, Math.max(1, duration * (0.08 + seed * 0.45)))
        : Math.max(0, duration * 0.5)
    }, { once: true })

    video.addEventListener("seeked", () => {
      window.clearTimeout(timeout)
      try {
        const width = video.videoWidth || 640
        const height = video.videoHeight || 360
        const canvas = document.createElement("canvas")
        canvas.width = 640
        canvas.height = Math.max(240, Math.round((640 / width) * height))
        const context = canvas.getContext("2d")
        if (!context) {
          finish(null)
          return
        }
        context.drawImage(video, 0, 0, canvas.width, canvas.height)
        finish(canvas.toDataURL("image/jpeg", 0.72))
      } catch {
        finish(null)
      }
    }, { once: true })

    video.src = buildMediaUrl(path)
  })

  thumbnailCache.set(path, result)
  return result
}

export function CourseArtwork({ course, className }: { course: Course; className?: string }) {
  const ref = useRef<HTMLDivElement | null>(null)
  const [isVisible, setIsVisible] = useState(false)
  const [thumbnail, setThumbnail] = useState<string | null>(course.thumbnail)
  const videoPath = useMemo(() => randomVideoPath(course), [course])

  useEffect(() => {
    const node = ref.current
    if (!node) return

    const observer = new IntersectionObserver(
      ([entry]) => {
        if (entry.isIntersecting) {
          setIsVisible(true)
          observer.disconnect()
        }
      },
      { rootMargin: "200px" }
    )

    observer.observe(node)
    return () => observer.disconnect()
  }, [])

  useEffect(() => {
    if (!isVisible || !videoPath) return
    let cancelled = false

    captureVideoThumbnail(videoPath).then((result) => {
      if (!cancelled && result) setThumbnail(result)
    })

    return () => {
      cancelled = true
    }
  }, [isVisible, videoPath])

  return (
    <div ref={ref} className={cn("course-art relative min-h-36 overflow-hidden", className)}>
      {thumbnail && (
        <div
          className="absolute inset-0 bg-cover bg-center opacity-90 transition-opacity duration-300"
          style={{ backgroundImage: `url(${thumbnail})` }}
        />
      )}
      <div className="absolute inset-0 bg-background/45" />
    </div>
  )
}
