"use client"

import { useState, useCallback, useEffect, useMemo } from "react"
import { parseAsString, useQueryState } from "nuqs"
import { Search, Moon, Sun, FolderOpen, RefreshCw, Loader2, BookOpen, LayoutGrid, List } from "lucide-react"
import { useTheme } from "next-themes"
import { CourseViewerLayout } from "@/components/course-viewer/layout"
import { CourseGrid } from "@/components/course-grid"
import { trpc } from "@/lib/trpc/client"
import { useCourseStore } from "@/lib/stores/course-store"
import { selectFolderDialog, isTauri } from "@/lib/tauri"
import { Button } from "@/components/ui/button"
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
import { SidebarProvider } from "@/components/ui/sidebar"

type View = "library" | "viewer"

export function HomeScreen() {
  const [viewParam, setViewParam] = useQueryState("view", parseAsString.withDefault("library"))
  const [courseId, setCourseId] = useQueryState("course", parseAsString)
  const [lessonId, setLessonId] = useQueryState("lesson", parseAsString)

  const view = viewParam === "viewer" ? ("viewer" satisfies View) : "library"
  const { data: courses = [] } = trpc.courses.list.useQuery()
  const markAccessed = trpc.courses.markAccessed.useMutation()

  const selectedCourse = useMemo(() => {
    return courses.find((course: Course) => course.id === courseId) ?? null
  }, [courses, courseId])

  useEffect(() => {
    if (view === "viewer" && !selectedCourse) {
      setViewParam("library")
      setCourseId(null)
      setLessonId(null)
    }
  }, [view, selectedCourse, setViewParam, setCourseId, setLessonId])

  const handleCourseSelect = useCallback(
    (course: Course) => {
      setCourseId(course.id)
      setLessonId(null)
      setViewParam("viewer")
      markAccessed.mutate({ courseId: course.id })
    },
    [setCourseId, setLessonId, setViewParam, markAccessed]
  )

  const handleBack = useCallback(() => {
    setViewParam("library")
    setCourseId(null)
    setLessonId(null)
  }, [setViewParam, setCourseId, setLessonId])

  if (view === "viewer") {
    return (
      <SidebarProvider>
        <CourseViewerLayout
          course={selectedCourse}
          onBack={handleBack}
          selectedLessonId={lessonId}
          onLessonChange={setLessonId}
        />
      </SidebarProvider>
    )
  }

  const [viewMode, setViewMode] = useState<"grid" | "list">("grid")

  return (
    <div className="flex h-full flex-col">
      <LibraryHeader viewMode={viewMode} onViewModeChange={setViewMode} />
      <div className="min-h-0 flex-1 overflow-auto p-4 md:p-6">
        <CourseGrid viewMode={viewMode} onCourseSelect={handleCourseSelect} />
      </div>
    </div>
  )
}

function LibraryHeader({ viewMode, onViewModeChange }: { viewMode: "grid" | "list"; onViewModeChange: (v: "grid" | "list") => void }) {
  const isScanning = useCourseStore((state) => state.isScanning)
  const libraryPath = useCourseStore((state) => state.libraryPath)
  const setIsScanning = useCourseStore((state) => state.setIsScanning)
  const [error, setError] = useState<string | null>(null)
  const [cmdOpen, setCmdOpen] = useState(false)
  const { resolvedTheme, setTheme } = useTheme()
  const isDark = resolvedTheme === "dark"
  const utils = trpc.useUtils()
  const { data: courses = [] } = trpc.courses.list.useQuery()
  const scanLibrary = trpc.library.scan.useMutation({
    onSuccess: async () => {
      await utils.courses.list.invalidate()
    },
  })

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

      setIsScanning(true)
      setError(null)
      await scanLibrary.mutateAsync({ path })
    } catch (err) {
      setError(err instanceof Error ? err.message : "Failed to scan the selected folder.")
    } finally {
      setIsScanning(false)
    }
  }

  async function handleRefresh() {
    if (!libraryPath) return

    try {
      setIsScanning(true)
      setError(null)
      await scanLibrary.mutateAsync({ path: libraryPath })
    } catch (err) {
      setError(err instanceof Error ? err.message : "Failed to refresh the current library.")
    } finally {
      setIsScanning(false)
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

          <Button
            type="button"
            variant="ghost"
            size="icon"
            onClick={() => onViewModeChange(viewMode === "grid" ? "list" : "grid")}
            className="size-8"
          >
            {viewMode === "grid" ? <List className="size-4" /> : <LayoutGrid className="size-4" />}
          </Button>

          <Button
            type="button"
            variant="ghost"
            size="icon"
            onClick={() => setTheme(isDark ? "light" : "dark")}
            className="size-8"
          >
            {isDark ? <Sun className="size-4" /> : <Moon className="size-4" />}
          </Button>

          <Button
            type="button"
            variant="ghost"
            size="icon"
            onClick={handleSelectFolder}
            disabled={isScanning}
            className="size-8"
          >
            {isScanning ? <Loader2 className="size-4 animate-spin" /> : <FolderOpen className="size-4" />}
          </Button>

          {libraryPath && (
            <Button
              type="button"
              variant="ghost"
              size="icon"
              onClick={handleRefresh}
              disabled={isScanning}
              className="size-8"
            >
              <RefreshCw className={`size-4 ${isScanning ? "animate-spin" : ""}`} />
            </Button>
          )}
        </div>

        <div data-tauri-drag-region className="ml-auto" />
      </header>

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
                    window.location.search = `?view=viewer&course=${course.id}`
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
