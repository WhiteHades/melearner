import { formatDistanceToNow } from "date-fns"
import { BookOpen, Clock3, PlayCircle, CheckCircle2 } from "lucide-react"
import { Badge } from "@/components/ui/badge"
import { Button } from "@/components/ui/button"
import {
  Card,
  CardContent,
  CardFooter,
  CardHeader,
  CardTitle,
} from "@/components/ui/card"
import { Progress } from "@/components/ui/progress"
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
    <Card className="h-full overflow-hidden border-border/70 bg-card/90 transition-[transform,box-shadow,border-color] duration-200 hover:-translate-y-0.5 hover:border-border hover:shadow-[0_24px_64px_-44px_rgba(15,23,42,0.45)] active:scale-[0.99]" onClick={onClick}>
      <CardHeader className="pb-3">
        <div className="flex items-start justify-between gap-4">
          <div className="space-y-2 min-w-0">
            <div className="flex items-center gap-2">
              <Badge variant="secondary" className="rounded-full px-2.5 py-1 text-xs font-medium">
                {course.sections.length} sections
              </Badge>
              {progress > 0 && (
                <Badge variant="outline" className="rounded-full font-mono text-xs">
                  {progress}%
                </Badge>
              )}
            </div>
            <CardTitle className="text-lg font-semibold leading-tight tracking-tight text-foreground text-balance truncate">
              {course.name}
            </CardTitle>
          </div>
          {progress === 100 && (
            <Button variant="ghost" size="icon" className="shrink-0 text-primary hover:text-primary" disabled>
              <CheckCircle2 className="size-4" />
            </Button>
          )}
        </div>
      </CardHeader>

      <CardContent className="flex flex-1 flex-col gap-4 pb-4">
        <div className="flex flex-wrap items-center gap-3 text-sm text-muted-foreground">
          <div className="flex items-center gap-1.5">
            <BookOpen className="size-3.5" />
            <span>{course.sections.length} sections</span>
          </div>
          <div className="flex items-center gap-1.5">
            <PlayCircle className="size-3.5" />
            <span>{totalLessons} lessons</span>
          </div>
          {course.totalDuration > 0 && (
            <div className="flex items-center gap-1.5">
              <Clock3 className="size-3.5" />
              <span>{formatDuration(course.totalDuration)}</span>
            </div>
          )}
        </div>

        <div className="space-y-1.5">
          <div className="flex items-center justify-between text-xs text-muted-foreground">
            <span>{completedLessons} of {totalLessons} lessons completed</span>
            <span className="font-medium tabular-nums text-foreground">{progress}%</span>
          </div>
          <Progress value={progress} className="h-1.5" />
        </div>
      </CardContent>

      <CardFooter className="flex items-center justify-between gap-2 border-t border-border/70 pt-3 text-sm text-muted-foreground">
        <span className="truncate text-pretty">{lastAccessedLabel}</span>
        <span className="shrink-0 font-medium text-foreground">Open course</span>
      </CardFooter>
    </Card>
  )
}