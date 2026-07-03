"use client"

import { useMemo } from "react"
import { FolderOpen } from "lucide-react"
import { CourseCard } from "@/components/course-card"
import { Card, CardContent } from "@/components/ui/card"
import { Separator } from "@/components/ui/separator"
import { Skeleton } from "@/components/ui/skeleton"
import { search } from "@/lib/search"
import type { Course } from "@/types"

interface CourseGridProps {
  courses: Course[]
  hasHydrated?: boolean
  onCourseSelect: (course: Course) => void
  searchQuery?: string
  viewMode?: "grid" | "list"
}

export function CourseGrid({ courses, hasHydrated = true, onCourseSelect, searchQuery, viewMode = "grid" }: CourseGridProps) {
  const normalizedQuery = searchQuery?.trim() ?? ""

  const visibleCourses = useMemo<Course[]>(() => {
    if (!normalizedQuery) return courses
    const results = search(normalizedQuery, 50)
    const ids = new Set(
      results.map((result) => (result.type === "course" ? result.id : result.courseId)).filter(Boolean)
    )
    return courses.filter((course: Course) => ids.has(course.id))
  }, [courses, normalizedQuery])

  return (
    <div className="flex flex-col gap-6">
      <div className="flex flex-col gap-4">
        <div className="space-y-1">
          <h2 className="text-xl font-bold tracking-tight text-foreground">Your courses</h2>
          <p className="text-sm text-muted-foreground">
            {courses.length} {courses.length === 1 ? "course" : "courses"} in your library
          </p>
        </div>
      </div>

      <Separator />

      {!hasHydrated ? (
        <div className="grid gap-4 sm:grid-cols-2 xl:grid-cols-4">
          {Array.from({ length: 4 }).map((_, index) => (
            <Card key={index} className="border-border/70 bg-card/80">
              <CardContent className="flex flex-col gap-4 p-5">
                <Skeleton className="h-4 w-20" />
                <Skeleton className="h-7 w-3/4" />
                <Skeleton className="h-16 w-full" />
                <Skeleton className="h-4 w-1/2" />
              </CardContent>
            </Card>
          ))}
        </div>
      ) : courses.length === 0 ? (
        <Card className="border-dashed border-border/80 bg-card/75 shadow-none">
          <CardContent className="flex flex-col items-center gap-5 px-6 py-12 text-center">
            <div className="flex size-14 items-center justify-center rounded-xl bg-secondary text-secondary-foreground">
              <FolderOpen className="size-6" />
            </div>
            <div className="space-y-2">
              <h3 className="text-lg font-semibold tracking-tight text-foreground">No courses yet</h3>
              <p className="max-w-md text-sm text-muted-foreground">
                Choose a folder above to scan your course library.
              </p>
            </div>
          </CardContent>
        </Card>
      ) : normalizedQuery && visibleCourses.length === 0 ? (
        <Card className="border-border/70 bg-card/75 shadow-none">
          <CardContent className="px-6 py-10 text-center">
            <h3 className="text-lg font-semibold tracking-tight text-foreground">
              No matches for &quot;{normalizedQuery}&quot;
            </h3>
            <p className="mt-2 text-sm text-muted-foreground">
              Try a course title, lesson name, or section heading.
            </p>
          </CardContent>
        </Card>
      ) : viewMode === "list" ? (
        <div className="flex flex-col gap-2">
          {visibleCourses.map((course) => (
            <CourseCard
              key={course.id}
              course={course}
              onClick={() => onCourseSelect(course)}
            />
          ))}
        </div>
      ) : (
        <div className="grid gap-4 sm:grid-cols-2 lg:grid-cols-3 xl:grid-cols-4">
          {visibleCourses.map((course) => (
            <CourseCard
              key={course.id}
              course={course}
              onClick={() => onCourseSelect(course)}
            />
          ))}
        </div>
      )}
    </div>
  )
}
