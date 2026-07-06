"use client"

import { BookOpen, FileText, Headphones, Play } from "lucide-react"
import { cn } from "@/lib/utils"
import type { Course } from "@/types"

export function CourseArtwork({ course, className }: { course: Course; className?: string }) {
  let Icon = BookOpen

  for (const section of course.sections) {
    const lesson = section.lessons.find((item) => item.type === "video" || item.type === "audio" || item.type === "document")
    if (!lesson) continue
    Icon = lesson.type === "video" ? Play : lesson.type === "audio" ? Headphones : FileText
    break
  }

  return (
    <div className={cn("course-art relative min-h-36 overflow-hidden", className)}>
      {course.thumbnail && (
        <img
          alt=""
          className="absolute inset-0 size-full object-cover opacity-95 transition-opacity duration-300"
          data-course-thumbnail=""
          draggable={false}
          src={course.thumbnail}
        />
      )}
      <div className={cn("absolute inset-0", course.thumbnail ? "bg-background/20" : "bg-background/35")} />
      {!course.thumbnail && (
        <div className="absolute inset-0 z-10 flex items-center justify-center" data-course-fallback="">
          <div className="flex size-12 items-center justify-center rounded-xl border border-white/20 bg-white/15 text-primary shadow-[var(--shadow-whisper)] backdrop-blur">
            <Icon className="size-5" />
          </div>
        </div>
      )}
    </div>
  )
}
