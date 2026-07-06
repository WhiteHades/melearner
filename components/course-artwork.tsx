"use client"

import { BookOpen, FileText, Headphones, Play } from "lucide-react"
import { useState } from "react"
import { frontendLog } from "@/lib/frontend-log"
import { cn } from "@/lib/utils"
import type { Course } from "@/types"

export function CourseArtwork({ course, className }: { course: Course; className?: string }) {
  const [failedThumbnail, setFailedThumbnail] = useState<string | null>(null)
  let Icon = BookOpen
  const showThumbnail = Boolean(course.thumbnail) && failedThumbnail !== course.thumbnail

  for (const section of course.sections) {
    const lesson = section.lessons.find((item) => item.type === "video" || item.type === "audio" || item.type === "document")
    if (!lesson) continue
    Icon = lesson.type === "video" ? Play : lesson.type === "audio" ? Headphones : FileText
    break
  }

  return (
    <div className={cn("course-art relative min-h-36 overflow-hidden", className)}>
      {showThumbnail && course.thumbnail && (
        // eslint-disable-next-line @next/next/no-img-element
        <img
          alt=""
          aria-hidden="true"
          className="absolute inset-0 z-10 size-full object-cover opacity-95 transition-opacity duration-300"
          data-course-thumbnail=""
          src={course.thumbnail}
          onError={() => {
            frontendLog("warn", "course.thumbnail.load.failed", {
              courseId: course.id,
              thumbnail: course.thumbnail,
            })
            setFailedThumbnail(course.thumbnail)
          }}
        />
      )}
      <div className={cn("absolute inset-0 z-20", showThumbnail ? "bg-background/20" : "bg-background/35")} />
      {!showThumbnail && (
        <div className="absolute inset-0 z-30 flex items-center justify-center" data-course-fallback="">
          <div className="flex size-12 items-center justify-center rounded-xl border border-white/20 bg-white/15 text-primary shadow-[var(--shadow-whisper)] backdrop-blur">
            <Icon className="size-5" />
          </div>
        </div>
      )}
    </div>
  )
}
