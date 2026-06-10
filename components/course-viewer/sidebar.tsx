"use client"

import * as React from "react"
import { ChevronLeft, ChevronRight, CheckCircle2, Circle, PlayCircle, FolderOpen } from "lucide-react"
import { Collapsible, CollapsibleContent, CollapsibleTrigger } from "@/components/ui/collapsible"
import { ScrollArea } from "@/components/ui/scroll-area"
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
            <SidebarMenuButton variant="outline" size="sm" onClick={onBack} className="w-full justify-start gap-1.5">
              <ChevronLeft className="size-3.5 shrink-0" />
              <span className="truncate text-xs font-medium">Back</span>
            </SidebarMenuButton>
          </SidebarMenuItem>
        </SidebarMenu>
      </SidebarHeader>

      <SidebarContent>
        <SidebarGroup>
          <SidebarGroupLabel className="text-xs">{course.name}</SidebarGroupLabel>
          <SidebarGroupContent>
            <div className="px-1.5">
              <p className="text-[10px] leading-relaxed text-muted-foreground">
                {course.sections.length} section{course.sections.length !== 1 ? "s" : ""}
                {" · "}
                {course.sections.reduce((s, sec) => s + sec.lessons.length, 0)} lesson
                {course.sections.reduce((s, sec) => s + sec.lessons.length, 0) !== 1 ? "s" : ""}
              </p>
            </div>
          </SidebarGroupContent>
        </SidebarGroup>

        <SidebarGroup>
          <SidebarGroupLabel className="text-xs">Outline</SidebarGroupLabel>
          <SidebarGroupContent>
            <ScrollArea className="flex-1">
              <SidebarMenu>
                {course.sections.map((section: Section, sectionIndex: number) => (
                  <SidebarMenuItem key={section.id}>
                    <Collapsible open defaultOpen>
                      <CollapsibleTrigger asChild>
                        <SidebarGroupLabel className="flex items-center gap-1.5 px-1.5 py-1 hover:bg-sidebar-accent hover:text-sidebar-accent-foreground cursor-pointer rounded-md transition-colors">
                          <FolderOpen className="size-3.5 shrink-0" />
                          <span className="truncate text-xs font-medium">Module {sectionIndex + 1}</span>
                          <span className="text-muted-foreground ml-auto text-[10px]">
                            {section.lessons.length}
                          </span>
                          <ChevronRight className="size-3 shrink-0 transition-transform duration-200 data-[state=open]:rotate-90" />
                        </SidebarGroupLabel>
                      </CollapsibleTrigger>
                      <CollapsibleContent>
                        <SidebarGroupContent>
                          <SidebarMenu>
                            {section.lessons.map((lesson: Lesson) => (
                              <SidebarMenuItem key={lesson.id}>
                                <SidebarMenuButton
                                  size="sm"
                                  variant={currentLessonId === lesson.id ? "default" : "outline"}
                                  isActive={currentLessonId === lesson.id}
                                  onClick={() => onSelectLesson?.(lesson)}
                                  className="h-auto px-1.5 py-1.5"
                                >
                                  <div className="flex w-full items-start gap-1.5">
                                    <div className="mt-0.5 flex size-3.5 shrink-0 items-center justify-center">
                                      {lesson.completed ? (
                                        <CheckCircle2 className="size-3.5 text-primary" />
                                      ) : currentLessonId === lesson.id ? (
                                        <PlayCircle className="size-3.5 text-primary" />
                                      ) : (
                                        <Circle className="size-3.5 text-muted-foreground" />
                                      )}
                                    </div>
                                    <span className="whitespace-normal text-left text-[11px] leading-snug break-words">{lesson.name}</span>
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
