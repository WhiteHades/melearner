"use client"

import { useMemo } from "react"
import { FolderOpen } from "lucide-react"
import { CourseCard } from "@/components/course-card"
import { Card, CardContent } from "@/components/ui/card"
import { Separator } from "@/components/ui/separator"
import { search } from "@/lib/search"
import { trpc } from "@/lib/trpc/client"
import type { Course } from "@/types"

interface CourseGridProps {
  onCourseSelect: (course: Course) => void
  searchQuery?: string
}

export function CourseGrid({ onCourseSelect, searchQuery }: CourseGridProps) {
  const { data: courses = [] } = trpc.courses.list.useQuery()

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

      {courses.length === 0 ? (
        <Card className="border-dashed border-border/80 bg-card/75 shadow-none">
          <CardContent className="flex flex-col items-center gap-5 px-6 py-12 text-center">
            <div className="flex size-14 items-center justify-center rounded-full bg-secondary text-secondary-foreground">
              <FolderOpen className="size-6" />
            </div>
            <div className="space-y-2">
              <h3 className="text-lg font-semibold tracking-tight text-foreground">No courses yet</h3>
              <p className="max-w-md text-sm text-muted-foreground">
                Choose a folder from the sidebar to scan for courses.
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