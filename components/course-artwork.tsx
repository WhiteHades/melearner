"use client"

import { useEffect, useMemo, useRef, useState } from "react"
import { convertFileSrc } from "@tauri-apps/api/core"
import { GraduationCap } from "lucide-react"
import { isTauri } from "@/lib/tauri"
import { cn } from "@/lib/utils"
import type { Course } from "@/types"

const thumbnailCache = new Map<string, string | null>()

function firstVideoPath(course: Course): string | null {
  if (course.thumbnailSourcePath) return course.thumbnailSourcePath

  for (const section of course.sections) {
    const lesson = section.lessons.find((item) => item.type === "video")
    if (lesson) return lesson.path
  }

  return null
}

function buildMediaUrl(path: string): string {
  return isTauri() ? convertFileSrc(path) : path
}

async function captureVideoThumbnail(path: string): Promise<string | null> {
  if (typeof document === "undefined") return null

  const cached = thumbnailCache.get(path)
  if (cached !== undefined) return cached

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
      video.currentTime = duration > 12 ? Math.min(24, duration * 0.08) : Math.max(0, duration * 0.5)
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

export function CourseArtwork({ course, className, iconClassName }: { course: Course; className?: string; iconClassName?: string }) {
  const ref = useRef<HTMLDivElement | null>(null)
  const [isVisible, setIsVisible] = useState(false)
  const [thumbnail, setThumbnail] = useState<string | null>(course.thumbnail)
  const videoPath = useMemo(() => firstVideoPath(course), [course])

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
      <div className={cn("absolute bottom-3 left-3 flex size-11 items-center justify-center rounded-md border border-border bg-background/90 text-primary shadow-[var(--shadow-whisper)]", iconClassName)}>
        <GraduationCap className="size-5" />
      </div>
    </div>
  )
}
