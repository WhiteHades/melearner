import { formatDistanceToNow } from "date-fns"
import { BookOpen, Clock3, PlayCircle } from "lucide-react"
import { Badge } from "@/components/ui/badge"
import {
  Card,
  CardContent,
  CardFooter,
  CardHeader,
  CardTitle,
} from "@/components/ui/card"
import { formatDuration } from "@/lib/utils"
import type { Course } from "@/types"

interface CourseCardProps {
  course: Course
  onClick: () => void
}

export function CourseCard({ course, onClick }: CourseCardProps) {
  const totalLessons = course.sections.reduce((sum, section) => sum + section.lessons.length, 0)
  const completedLessons = course.sections.reduce(
    (sum, section) => sum + section.lessons.filter((lesson) => lesson.completed).length,
    0
  )
  const progress = totalLessons > 0 ? Math.round((completedLessons / totalLessons) * 100) : 0
  const lastAccessedLabel = course.lastAccessed
    ? formatDistanceToNow(new Date(course.lastAccessed), { addSuffix: true })
    : "Not started yet"

  return (
    <button
      type="button"
      onClick={onClick}
      className="block h-full w-full text-left focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-ring focus-visible:ring-offset-2"
    >
      <Card className="flex h-full overflow-hidden rounded-[28px] border-border/70 bg-card/90 transition-[transform,box-shadow,border-color] duration-200 hover:-translate-y-0.5 hover:border-border hover:shadow-[0_24px_64px_-44px_rgba(15,23,42,0.45)] active:scale-[0.99]">
        <CardHeader className="gap-4 pb-4">
          <div className="flex items-start justify-between gap-4">
            <div className="space-y-3">
              <Badge variant="secondary" className="rounded-full px-2.5 py-1 text-xs font-medium">
                {course.sections.length} sections
              </Badge>
              <CardTitle className="text-xl font-bold leading-tight tracking-tight text-foreground text-balance">
                {course.name}
              </CardTitle>
            </div>

            <Badge variant="outline" className="rounded-full font-mono text-xs">
              {progress}%
            </Badge>
          </div>
        </CardHeader>

        <CardContent className="flex flex-1 flex-col gap-5 pb-5">
          <div className="flex flex-wrap items-center gap-4 text-sm text-muted-foreground">
            <div className="flex items-center gap-2">
              <BookOpen className="size-4" />
              <span>{course.sections.length} sections</span>
            </div>
            <div className="flex items-center gap-2">
              <PlayCircle className="size-4" />
              <span>{totalLessons} lessons</span>
            </div>
            {course.totalDuration > 0 && (
              <div className="flex items-center gap-2">
                <Clock3 className="size-4" />
                <span>{formatDuration(course.totalDuration)}</span>
              </div>
            )}
          </div>

          <div className="space-y-2">
            <div className="h-2 w-full overflow-hidden rounded-full bg-secondary">
              <div
                className="h-full rounded-full bg-primary transition-all duration-300 ease-out"
                style={{ width: `${progress}%` }}
              />
            </div>
            <p className="text-sm text-muted-foreground tabular-nums">
              {completedLessons} of {totalLessons} lessons completed
            </p>
          </div>
        </CardContent>

        <CardFooter className="flex-wrap gap-2 border-t border-border/70 pt-4 text-sm text-muted-foreground">
          <span className="truncate text-pretty">{lastAccessedLabel}</span>
          <span className="ml-auto shrink-0 font-medium text-foreground">Open course</span>
        </CardFooter>
      </Card>
    </button>
  )
}
