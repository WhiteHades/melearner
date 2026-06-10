"use client"

import * as React from "react"
import { ChevronLeft, ChevronRight, CheckCircle2, Circle, PlayCircle, FolderOpen } from "lucide-react"
import { Button } from "@/components/ui/button"
import { Collapsible, CollapsibleContent, CollapsibleTrigger } from "@/components/ui/collapsible"
import { ScrollArea } from "@/components/ui/scroll-area"
import { Separator } from "@/components/ui/separator"
import { cn, cleanSectionName } from "@/lib/utils"
import type { Course, Lesson, Section } from "@/types"
import {
  Sidebar,
  SidebarContent,
  SidebarGroup,
  SidebarGroupContent,
  SidebarGroupLabel,
  SidebarHeader,
  SidebarMenu,
  SidebarMenuButton,
  SidebarMenuItem,
  SidebarRail,
} from "@/components/ui/sidebar"

interface CourseViewerSidebarProps {
  course: Course | null
  currentLessonId?: string
  onSelectLesson?: (lesson: Lesson) => void
  onBack?: () => void
}

export function CourseViewerSidebar({ course, currentLessonId, onSelectLesson, onBack }: CourseViewerSidebarProps) {
  if (!course) return null

  return (
    <Sidebar collapsible="offcanvas" side="left" variant="sidebar">
      <SidebarHeader>
        <SidebarMenu>
          <SidebarMenuItem>
            <SidebarMenuButton variant="outline" size="lg" onClick={onBack} className="w-full justify-start gap-2">
              <ChevronLeft className="size-4" />
              <span className="truncate font-medium">Back to library</span>
            </SidebarMenuButton>
          </SidebarMenuItem>
        </SidebarMenu>
      </SidebarHeader>

      <SidebarContent>
        <SidebarGroup>
          <SidebarGroupLabel>{course.name}</SidebarGroupLabel>
          <SidebarGroupContent>
            <div className="space-y-2 px-2">
              <div className="flex flex-wrap items-center gap-2 text-xs text-muted-foreground">
                <span>{course.sections.length} sections</span>
                <span>·</span>
                <span>{course.sections.reduce((s, sec) => s + sec.lessons.length, 0)} lessons</span>
              </div>
            </div>
          </SidebarGroupContent>
        </SidebarGroup>

        <SidebarGroup>
          <SidebarGroupLabel>Course outline</SidebarGroupLabel>
          <SidebarGroupContent>
            <ScrollArea className="flex-1">
              <SidebarMenu>
                {course.sections.map((section: Section, sectionIndex: number) => (
                  <SidebarMenuItem key={section.id}>
                    <Collapsible open defaultOpen>
                      <CollapsibleTrigger asChild>
                        <SidebarGroupLabel className="flex items-center gap-2 px-2 py-1.5 hover:bg-sidebar-accent hover:text-sidebar-accent-foreground cursor-pointer rounded-md transition-colors">
                          <FolderOpen className="size-4 shrink-0" />
                          <span className="truncate font-medium">Module {sectionIndex + 1}</span>
                          <span className="text-muted-foreground ml-auto text-xs">
                            {section.lessons.length} lessons
                          </span>
                          <ChevronRight className="size-4 shrink-0 transition-transform duration-200 data-[state=open]:rotate-90" />
                        </SidebarGroupLabel>
                      </CollapsibleTrigger>
                      <CollapsibleContent>
                        <SidebarGroupContent className="pl-2">
                          <SidebarMenu>
                            {section.lessons.map((lesson: Lesson) => (
                              <SidebarMenuItem key={lesson.id}>
                                <SidebarMenuButton
                                  asChild
                                  size="sm"
                                  variant={currentLessonId === lesson.id ? "default" : "outline"}
                                  isActive={currentLessonId === lesson.id}
                                  onClick={() => onSelectLesson?.(lesson)}
                                >
                                  <div className="flex w-full items-center gap-2">
                                    <div className="flex size-4 shrink-0 items-center justify-center">
                                      {lesson.completed ? (
                                        <CheckCircle2 className="size-4 text-primary" />
                                      ) : currentLessonId === lesson.id ? (
                                        <PlayCircle className="size-4 text-primary" />
                                      ) : (
                                        <Circle className="size-4 text-muted-foreground" />
                                      )}
                                    </div>
                                    <span className="truncate text-sm">{lesson.name}</span>
                                    <span className="ml-auto text-xs text-muted-foreground capitalize">
                                      {lesson.completed ? "Completed" : lesson.type}
                                    </span>
                                  </div>
                                </SidebarMenuButton>
                              </SidebarMenuItem>
                            ))}
                          </SidebarMenu>
                        </SidebarGroupContent>
                      </CollapsibleContent>
                    </Collapsible>
                  </SidebarMenuItem>
                ))}
              </SidebarMenu>
            </ScrollArea>
          </SidebarGroupContent>
        </SidebarGroup>
      </SidebarContent>

      <SidebarRail />
    </Sidebar>
  )
}