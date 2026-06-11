"use client"

import { useState, useCallback, useEffect, useMemo } from "react"
import { useShallow } from "zustand/react/shallow"
import { parseAsString, useQueryState } from "nuqs"
import { Search, Moon, Sun, FolderOpen, RefreshCw, Loader2, BookOpen, LayoutGrid, List, Sparkles, Clock3, PlayCircle } from "lucide-react"
import { useTheme } from "next-themes"
import { CourseViewerLayout } from "@/components/course-viewer/layout"
import { CourseGrid } from "@/components/course-grid"
import { trpc } from "@/lib/trpc/client"
import { useCourseStore } from "@/lib/stores/course-store"
import { selectFolderDialog, isTauri } from "@/lib/tauri"
import { Button } from "@/components/ui/button"
import { ToggleGroup, ToggleGroupItem } from "@/components/ui/toggle-group"
import {
  CommandDialog,
  CommandEmpty,
  CommandGroup,
  CommandInput,
  CommandItem,
  CommandList,
} from "@/components/ui/command"
import { Alert, AlertDescription, AlertTitle } from "@/components/ui/alert"
import type { Course } from "@/types"
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from "@/components/ui/card"
import { formatDuration } from "@/lib/utils"

type View = "library" | "viewer"

export function HomeScreen() {
  const [viewParam, setViewParam] = useQueryState("view", parseAsString.withDefault("library"))
  const [courseId, setCourseId] = useQueryState("course", parseAsString)
  const [lessonId, setLessonId] = useQueryState("lesson", parseAsString)

  const view = viewParam === "viewer" ? ("viewer" satisfies View) : "library"
  const courses = useCourseStore(useShallow((state) => state.courses))
  const hasHydrated = useCourseStore((state) => state.hasHydrated)
  const markAccessed = trpc.courses.markAccessed.useMutation()

  const selectedCourse = useMemo(() => {
    return courses.find((course: Course) => course.id === courseId) ?? null
  }, [courses, courseId])

  useEffect(() => {
    if (view === "viewer" && hasHydrated && !selectedCourse) {
      setViewParam("library")
      setCourseId(null)
      setLessonId(null)
    }
  }, [view, hasHydrated, selectedCourse, setViewParam, setCourseId, setLessonId])

  const handleCourseSelect = useCallback(
    (course: Course) => {
      setCourseId(course.id)
      setLessonId(null)
      setViewParam("viewer")
      markAccessed.mutate({ courseId: course.id })
    },
    [setCourseId, setLessonId, setViewParam, markAccessed]
  )

  const [viewMode, setViewMode] = useState<"grid" | "list">("grid")

  const handleBack = useCallback(() => {
    setViewParam("library")
    setCourseId(null)
    setLessonId(null)
  }, [setViewParam, setCourseId, setLessonId])

  if (view === "viewer") {
    return (
      <CourseViewerLayout
        course={selectedCourse}
        onBack={handleBack}
        selectedLessonId={lessonId}
        onLessonChange={setLessonId}
      />
    )
  }

  return (
    <div className="app-shell flex h-full min-h-0 flex-col">
      <LibraryHeader
        courses={courses}
        hasHydrated={hasHydrated}
        onOpenCourse={handleCourseSelect}
        viewMode={viewMode}
        onViewModeChange={setViewMode}
      />
      <div className="min-h-0 flex-1 overflow-auto p-4 md:p-6">
        <CourseGrid
          courses={courses}
          hasHydrated={hasHydrated}
          viewMode={viewMode}
          onCourseSelect={handleCourseSelect}
        />
      </div>
    </div>
  )
}

function LibraryHeader({
  courses,
  hasHydrated,
  onOpenCourse,
  viewMode,
  onViewModeChange,
}: {
  courses: Course[]
  hasHydrated: boolean
  onOpenCourse: (course: Course) => void
  viewMode: "grid" | "list"
  onViewModeChange: (v: "grid" | "list") => void
}) {
  const libraryPath = useCourseStore((state) => state.libraryPath)
  const scanMode = useCourseStore((state) => state.scanMode)
  const setScanMode = useCourseStore((state) => state.setScanMode)
  const [error, setError] = useState<string | null>(null)
  const [cmdOpen, setCmdOpen] = useState(false)
  const { resolvedTheme, setTheme } = useTheme()
  const isDark = resolvedTheme === "dark"
  const scanLibrary = trpc.library.scan.useMutation()

  const totalLessons = useMemo(
    () => courses.reduce((sum, course) => sum + course.sections.reduce((count, section) => count + section.lessons.length, 0), 0),
    [courses]
  )
  const completedLessons = useMemo(
    () => courses.reduce(
      (sum, course) =>
        sum +
        course.sections.reduce(
          (count, section) => count + section.lessons.filter((lesson) => lesson.completed).length,
          0
        ),
      0
    ),
    [courses]
  )
  const totalDuration = useMemo(
    () => courses.reduce((sum, course) => sum + course.totalDuration, 0),
    [courses]
  )
  const continueCourse = useMemo(
    () =>
      [...courses]
        .filter((course) => course.lastAccessed)
        .sort((a, b) => (b.lastAccessed ?? "").localeCompare(a.lastAccessed ?? ""))[0] ?? null,
    [courses]
  )

  useEffect(() => {
    function handleKeyDown(e: KeyboardEvent) {
      if ((e.metaKey || e.ctrlKey) && e.key === "k") {
        e.preventDefault()
        setCmdOpen(true)
      }
    }
    document.addEventListener("keydown", handleKeyDown)
    return () => document.removeEventListener("keydown", handleKeyDown)
  }, [])

  async function handleSelectFolder() {
    if (!isTauri()) {
      setError("Folder selection is only available in the desktop app.")
      return
    }

    try {
      const path = await selectFolderDialog()
      if (!path) return

      setScanMode("selecting")
      setError(null)
      await scanLibrary.mutateAsync({ path })
    } catch (err) {
      setError(err instanceof Error ? err.message : "Failed to scan the selected folder.")
    } finally {
      setScanMode("idle")
    }
  }

  async function handleRefresh() {
    if (!libraryPath) return

    try {
      setScanMode("refreshing")
      setError(null)
      await scanLibrary.mutateAsync({ path: libraryPath })
    } catch (err) {
      setError(err instanceof Error ? err.message : "Failed to refresh the current library.")
    } finally {
      setScanMode("idle")
    }
  }

  return (
    <>
      <header className="relative flex h-12 shrink-0 items-center gap-2 border-b px-3">
        <div data-tauri-drag-region className="absolute inset-x-0 top-0 h-3" />
        <div className="flex items-center gap-1">
          <Button
            type="button"
            variant="ghost"
            size="sm"
            onClick={() => setCmdOpen(true)}
            className="gap-2 text-muted-foreground"
          >
            <Search className="size-4" />
            <span className="hidden sm:inline text-xs">Search</span>
          </Button>

          <ToggleGroup
            type="single"
            value={viewMode}
            onValueChange={(value) => {
              if (value === "grid" || value === "list") onViewModeChange(value)
            }}
            variant="outline"
            size="sm"
            className="rounded-lg border bg-background/80 px-0.5 backdrop-blur-sm"
          >
            <ToggleGroupItem value="grid" aria-label="Grid view" className="size-8 px-0">
              <LayoutGrid className="size-4" />
            </ToggleGroupItem>
            <ToggleGroupItem value="list" aria-label="List view" className="size-8 px-0">
              <List className="size-4" />
            </ToggleGroupItem>
          </ToggleGroup>

          <Button
            type="button"
            variant="ghost"
            size="icon"
            onClick={() => setTheme(isDark ? "light" : "dark")}
            className="size-8"
            aria-label={isDark ? "Switch to light theme" : "Switch to dark theme"}
          >
            {isDark ? <Sun className="size-4" /> : <Moon className="size-4" />}
          </Button>

          <Button
            type="button"
            variant="ghost"
            size="icon"
            onClick={handleSelectFolder}
            disabled={scanMode !== "idle"}
            className="size-8"
            aria-label={libraryPath ? "Change library folder" : "Choose library folder"}
          >
            {scanMode === "selecting" ? <Loader2 className="size-4 animate-spin" /> : <FolderOpen className="size-4" />}
          </Button>

          {libraryPath && (
            <Button
              type="button"
              variant="ghost"
              size="icon"
              onClick={handleRefresh}
              disabled={scanMode !== "idle"}
              className="size-8"
              aria-label="Refresh library"
            >
              <RefreshCw className={`size-4 ${scanMode === "refreshing" ? "animate-spin" : ""}`} />
            </Button>
          )}
        </div>

        <div data-tauri-drag-region className="ml-auto" />
      </header>

      <div className="border-b bg-muted/20 px-4 py-3">
        <div className="grid gap-3 sm:grid-cols-2 xl:grid-cols-4">
          <StatsCard icon={BookOpen} label="Courses" value={String(courses.length)} hint={hasHydrated ? "ready to learn" : "restoring library"} />
          <StatsCard icon={PlayCircle} label="Lessons" value={String(totalLessons)} hint={`${completedLessons} completed`} />
          <StatsCard icon={Sparkles} label="Progress" value={`${totalLessons > 0 ? Math.round((completedLessons / totalLessons) * 100) : 0}%`} hint="completion rate" />
          <StatsCard icon={Clock3} label="Runtime" value={totalDuration > 0 ? formatDuration(totalDuration) : "0m"} hint={continueCourse ? `continue ${continueCourse.name}` : "scan a folder to start"} />
        </div>
      </div>

      {error && (
        <div className="px-4 pt-2">
          <Alert variant="destructive" className="border-destructive/30 text-sm py-2">
            <AlertTitle className="text-xs">Error</AlertTitle>
            <AlertDescription className="text-xs">{error}</AlertDescription>
          </Alert>
        </div>
      )}

      {libraryPath && (
        <div className="px-4 pt-1.5">
          <p className="text-[10px] text-muted-foreground truncate">{libraryPath}</p>
        </div>
      )}

      <CommandDialog open={cmdOpen} onOpenChange={setCmdOpen}>
        <CommandInput placeholder="Search courses, lessons, sections\u2026" />
        <CommandList>
          <CommandEmpty>No results found.</CommandEmpty>
          {courses.length > 0 && (
            <CommandGroup heading="Courses">
              {courses.map((course) => (
                <CommandItem
                  key={course.id}
                  value={`course:${course.id}:${course.name}`}
                  onSelect={() => {
                    setCmdOpen(false)
                    onOpenCourse(course)
                  }}
                >
                  <BookOpen className="size-4" />
                  <span>{course.name}</span>
                </CommandItem>
              ))}
            </CommandGroup>
          )}
        </CommandList>
      </CommandDialog>
    </>
  )
}

function StatsCard({
  icon: Icon,
  label,
  value,
  hint,
}: {
  icon: typeof BookOpen
  label: string
  value: string
  hint: string
}) {
  return (
    <Card className="border-border/70 bg-background/85 shadow-none">
      <CardHeader className="flex flex-row items-center justify-between gap-3 space-y-0 pb-2">
        <div>
          <CardDescription>{label}</CardDescription>
          <CardTitle className="mt-1 text-xl font-semibold tracking-tight">{value}</CardTitle>
        </div>
        <div className="flex size-9 items-center justify-center rounded-full bg-secondary text-secondary-foreground">
          <Icon className="size-4" />
        </div>
      </CardHeader>
      <CardContent>
        <p className="truncate text-xs text-muted-foreground">{hint}</p>
      </CardContent>
    </Card>
  )
}
