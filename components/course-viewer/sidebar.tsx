"use client"

import { useMemo, useState } from "react"
import { ChevronDown, ChevronLeft, CheckCircle2, Circle, FileText, PlayCircle } from "lucide-react"
import { Badge } from "@/components/ui/badge"
import { Button } from "@/components/ui/button"
import { Collapsible, CollapsibleContent, CollapsibleTrigger } from "@/components/ui/collapsible"
import { Progress } from "@/components/ui/progress"
import { ScrollArea } from "@/components/ui/scroll-area"
import { cleanSectionName, cn, formatDuration } from "@/lib/utils"
import type { Course, Lesson, Section } from "@/types"

interface CourseViewerSidebarProps {
  course: Course | null
  currentLessonId?: string
  onSelectLesson?: (lesson: Lesson) => void
  onBack?: () => void
}

export function CourseViewerSidebar({ course, currentLessonId, onSelectLesson, onBack }: CourseViewerSidebarProps) {
  const [openSections, setOpenSections] = useState<Set<string>>(new Set())

  const summary = useMemo(() => {
    if (!course) return { completed: 0, total: 0, progress: 0 }
    const lessons = course.sections.flatMap((section) => section.lessons)
    const completed = lessons.filter((lesson) => lesson.completed).length
    return {
      completed,
      total: lessons.length,
      progress: lessons.length > 0 ? Math.round((completed / lessons.length) * 100) : 0,
    }
  }, [course])

  const activeSectionId = useMemo(() => {
    if (!course) return null
    return course.sections.find((section) => section.lessons.some((lesson) => lesson.id === currentLessonId))?.id ?? null
  }, [course, currentLessonId])

  if (!course) return null

  function setSectionOpen(sectionId: string, open: boolean) {
    setOpenSections((previous) => {
      const next = new Set(previous)
      if (open) next.add(sectionId)
      else next.delete(sectionId)
      return next
    })
  }

  return (
    <aside className="hidden min-h-0 bg-card md:flex md:flex-col">
      <div className="border-b border-border p-4">
        <div className="flex flex-col gap-4">
          <Button type="button" variant="outline" size="sm" onClick={onBack} className="w-fit gap-2 rounded-md">
            <ChevronLeft className="size-4" />
            Back
          </Button>
          <div className="flex flex-col gap-2">
            <Badge variant="secondary" className="w-fit rounded-md">Course outline</Badge>
            <h1 className="line-clamp-2 text-xl font-semibold leading-tight tracking-tight">{course.name}</h1>
            <div className="flex items-center justify-between text-xs text-muted-foreground">
              <span>{summary.completed}/{summary.total} items complete</span>
              <span className="tabular-nums">{summary.progress}%</span>
            </div>
            <Progress value={summary.progress} className="h-2" />
          </div>
        </div>
      </div>

      <ScrollArea className="min-h-0 flex-1">
        <div className="flex flex-col gap-1 p-3">
          {course.sections.map((section, index) => (
            <SectionOutline
              key={section.id}
              section={section}
              index={index}
              isOpen={openSections.has(section.id) || section.id === activeSectionId || (!activeSectionId && index === 0)}
              currentLessonId={currentLessonId}
              onOpenChange={(open) => setSectionOpen(section.id, open)}
              onSelectLesson={onSelectLesson}
            />
          ))}
        </div>
      </ScrollArea>
    </aside>
  )
}

function SectionOutline({
  section,
  index,
  isOpen,
  currentLessonId,
  onOpenChange,
  onSelectLesson,
}: {
  section: Section
  index: number
  isOpen: boolean
  currentLessonId?: string
  onOpenChange: (open: boolean) => void
  onSelectLesson?: (lesson: Lesson) => void
}) {
  const completed = section.lessons.filter((lesson) => lesson.completed).length
  const sectionTitle = cleanSectionName(section.name) || section.name

  return (
    <Collapsible open={isOpen} onOpenChange={onOpenChange}>
      <CollapsibleTrigger asChild>
        <button type="button" className="flex w-full items-start gap-3 rounded-lg px-3 py-3 text-left transition-colors hover:bg-accent hover:text-accent-foreground">
          <div className="mt-0.5 flex size-7 shrink-0 items-center justify-center rounded-md bg-secondary text-xs font-semibold text-secondary-foreground tabular-nums">
            {index + 1}
          </div>
          <div className="min-w-0 flex-1">
            <div className="flex items-center gap-2 text-xs font-medium text-primary">Module {index + 1}</div>
            <div className="line-clamp-2 text-sm font-semibold leading-snug">{sectionTitle}</div>
            <div className="mt-1 text-xs text-muted-foreground">{completed}/{section.lessons.length} complete</div>
          </div>
          <ChevronDown className={cn("mt-1 size-4 shrink-0 text-muted-foreground transition-transform", isOpen && "rotate-180")} />
        </button>
      </CollapsibleTrigger>
      <CollapsibleContent>
        <div className="flex flex-col gap-1 pb-2 pl-7">
          {section.lessons.map((lesson) => (
            <LessonRow key={lesson.id} lesson={lesson} isActive={currentLessonId === lesson.id} onSelect={() => onSelectLesson?.(lesson)} />
          ))}
        </div>
      </CollapsibleContent>
    </Collapsible>
  )
}

function LessonRow({ lesson, isActive, onSelect }: { lesson: Lesson; isActive: boolean; onSelect: () => void }) {
  const Icon = lesson.completed ? CheckCircle2 : lesson.type === "video" || lesson.type === "audio" ? PlayCircle : lesson.type === "document" ? FileText : Circle

  return (
    <button
      type="button"
      onClick={onSelect}
      className={cn(
        "flex w-full items-start gap-3 rounded-lg px-3 py-2 text-left transition-colors",
        isActive ? "bg-accent text-accent-foreground" : "hover:bg-accent/70 hover:text-accent-foreground"
      )}
    >
      <Icon className={cn("mt-0.5 size-4 shrink-0", lesson.completed ? "text-primary" : "text-muted-foreground")} />
      <div className="min-w-0 flex-1">
        <div className="line-clamp-2 text-sm font-medium leading-snug">{lesson.name}</div>
        <div className="mt-0.5 flex items-center gap-1 text-xs text-muted-foreground">
          <span className="capitalize">{lesson.type}</span>
          {lesson.duration > 0 && <span>· {formatDuration(lesson.duration)}</span>}
        </div>
      </div>
    </button>
  )
}
