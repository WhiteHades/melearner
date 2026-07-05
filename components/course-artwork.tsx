"use client"

import { cn } from "@/lib/utils"
import type { Course } from "@/types"

export function CourseArtwork({ course, className }: { course: Course; className?: string }) {
  return (
    <div className={cn("course-art relative min-h-36 overflow-hidden", className)}>
      {course.thumbnail && (
        <div
          className="absolute inset-0 bg-cover bg-center opacity-90 transition-opacity duration-300"
          style={{ backgroundImage: `url(${course.thumbnail})` }}
        />
      )}
      <div className="absolute inset-0 bg-background/45" />
    </div>
  )
}
