"use client"

import { ChevronLeft, CheckCircle2, Circle, PlayCircle } from "lucide-react"
import { Button } from "@/components/ui/button"
import { ScrollArea } from "@/components/ui/scroll-area"
import { Separator } from "@/components/ui/separator"
import { cn, cleanSectionName } from "@/lib/utils"
import type { Course, Lesson, Section } from "@/types"

interface SidebarProps {
  className?: string
  onBack?: () => void
  course: Course | null
  currentLessonId?: string
  onSelectLesson?: (lesson: Lesson) => void
}

export function Sidebar({ className, onBack, course, currentLessonId, onSelectLesson }: SidebarProps) {
  if (!course) return null

  const totalLessons = course.sections.reduce((sum, section) => sum + section.lessons.length, 0)
  const completedLessons = course.sections.reduce(
    (sum, section) => sum + section.lessons.filter((lesson) => lesson.completed).length,
    0
  )

  return (
    <div
      className={cn(
        "flex h-full min-h-0 min-w-0 flex-col bg-sidebar text-sidebar-foreground",
        className
      )}
    >
      <div className="space-y-5 px-4 py-5 sm:px-5">
        <Button variant="ghost" size="sm" onClick={onBack} className="h-9 w-fit px-3">
          <ChevronLeft data-icon="inline-start" />
          Back to library
        </Button>

        <div className="space-y-3">
          <p className="text-sm font-medium text-muted-foreground">Course outline</p>
          <div className="space-y-2">
            <h2 className="text-xl font-bold leading-tight tracking-tight text-balance">{course.name}</h2>
            <div className="flex flex-wrap items-center gap-3 text-sm text-muted-foreground">
              <span>{course.sections.length} sections</span>
              <span>{totalLessons} lessons</span>
              <span>{completedLessons} completed</span>
            </div>
          </div>
        </div>
      </div>

      <Separator className="bg-sidebar-border" />

      <ScrollArea className="flex-1 min-h-0 min-w-0">
        <div className="space-y-6 px-4 py-5 sm:px-5">
          {course.sections.map((section: Section, sectionIndex: number) => (
            <section key={section.id} className="space-y-3">
              <p className="break-words text-sm font-semibold text-foreground">
                Module {sectionIndex + 1} <span className="font-normal text-muted-foreground">&mdash; {cleanSectionName(section.name)}</span>
              </p>

              <div className="flex flex-col gap-2">
                {section.lessons.map((lesson: Lesson) => {
                  const isActive = currentLessonId === lesson.id

                  return (
                    <button
                      key={lesson.id}
                      type="button"
                      onClick={() => onSelectLesson?.(lesson)}
                      className={cn(
                        "w-full rounded-2xl border px-3 py-3 text-left transition-[transform,box-shadow,border-color,background-color] duration-200 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-ring focus-visible:ring-offset-2",
                        isActive
                          ? "border-sidebar-primary/25 bg-sidebar-primary/10 shadow-[0_18px_40px_-36px_rgba(15,23,42,0.45)]"
                          : "border-transparent bg-background/65 hover:-translate-y-0.5 hover:border-sidebar-border hover:bg-background"
                      )}
                    >
                      <div className="flex items-start gap-3">
                        <div className="mt-0.5 shrink-0">
                          {lesson.completed ? (
                            <CheckCircle2 className="size-4 text-primary" />
                          ) : isActive ? (
                            <PlayCircle className="size-4 text-primary" />
                          ) : (
                            <Circle className="size-4 text-muted-foreground" />
                          )}
                        </div>

                        <div className="min-w-0 space-y-1">
                          <p className="truncate text-sm font-medium text-foreground">{lesson.name}</p>
                          <p className="text-xs text-muted-foreground capitalize">
                            {lesson.completed ? "Completed" : lesson.type}
                          </p>
                        </div>
                      </div>
                    </button>
                  )
                })}
              </div>
            </section>
          ))}
        </div>
      </ScrollArea>
    </div>
  )
}
