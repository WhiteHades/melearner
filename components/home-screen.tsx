"use client"

import { useCallback, useEffect, useMemo, useState } from "react"
import { useShallow } from "zustand/react/shallow"
import { parseAsString, useQueryState } from "nuqs"
import {
  BookOpen,
  CheckCircle2,
  FolderOpen,
  LayoutGrid,
  List,
  Loader2,
  AlertTriangle,
  PlayCircle,
  RefreshCw,
  Search,
} from "lucide-react"
import { BrandLogo } from "@/components/brand-logo"
import { CourseViewerLayout } from "@/components/course-viewer/layout"
import { CourseArtwork } from "@/components/course-artwork"
import { ThemeMenu } from "@/components/theme-menu"
import { Alert, AlertDescription, AlertTitle } from "@/components/ui/alert"
import { Badge } from "@/components/ui/badge"
import { Button } from "@/components/ui/button"
import {
  CommandDialog,
  CommandEmpty,
  CommandGroup,
  CommandInput,
  CommandItem,
  CommandList,
} from "@/components/ui/command"
import { InputGroup, InputGroupAddon, InputGroupInput } from "@/components/ui/input-group"
import { Progress } from "@/components/ui/progress"
import { Skeleton } from "@/components/ui/skeleton"
import { Tooltip, TooltipContent, TooltipTrigger } from "@/components/ui/tooltip"
import { ToggleGroup, ToggleGroupItem } from "@/components/ui/toggle-group"
import { frontendLog } from "@/lib/frontend-log"
import { markCourseAccessed, scanLibraryAt } from "@/lib/operations"
import { useCourseStore } from "@/lib/stores/course-store"
import { getBuildInfo, isTauri, selectFolderDialog, type BuildInfo } from "@/lib/tauri"
import { cleanSectionName, cn } from "@/lib/utils"
import type { Course, Lesson } from "@/types"

type View = "library" | "viewer"
type ViewMode = "grid" | "list"

export function HomeScreen() {
  const [viewParam, setViewParam] = useQueryState("view", parseAsString.withDefault("library"))
  const [courseId, setCourseId] = useQueryState("course", parseAsString)
  const [lessonId, setLessonId] = useQueryState("lesson", parseAsString)

  const view = viewParam === "viewer" ? ("viewer" satisfies View) : "library"
  const courses = useCourseStore(useShallow((state) => state.courses))
  const hasHydrated = useCourseStore((state) => state.hasHydrated)

  const selectedCourse = useMemo(() => {
    return courses.find((course: Course) => course.id === courseId) ?? null
  }, [courses, courseId])

  useEffect(() => {
    if (view === "viewer" && hasHydrated && (!selectedCourse || selectedCourse.missingSince)) {
      setViewParam("library")
      setCourseId(null)
      setLessonId(null)
    }
  }, [view, hasHydrated, selectedCourse, setViewParam, setCourseId, setLessonId])

  const handleOpenCourse = useCallback(
    (course: Course, selectedLessonId: string | null = null) => {
      if (course.missingSince) return
      setCourseId(course.id)
      setLessonId(selectedLessonId)
      setViewParam("viewer")
      void markCourseAccessed(course.id)
    },
    [setCourseId, setLessonId, setViewParam]
  )

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

  return <LibraryDashboard courses={courses} hasHydrated={hasHydrated} onOpenCourse={handleOpenCourse} />
}

function LibraryDashboard({
  courses,
  hasHydrated,
  onOpenCourse,
}: {
  courses: Course[]
  hasHydrated: boolean
  onOpenCourse: (course: Course, lessonId?: string | null) => void
}) {
  const libraryPath = useCourseStore((state) => state.libraryPath)
  const scanMode = useCourseStore((state) => state.scanMode)
  const setScanMode = useCourseStore((state) => state.setScanMode)
  const [error, setError] = useState<string | null>(null)
  const [warnings, setWarnings] = useState<string[]>([])
  const [cmdOpen, setCmdOpen] = useState(false)
  const [buildInfo, setBuildInfo] = useState<BuildInfo | null>(null)
  const [viewMode, setViewMode] = useState<ViewMode>("grid")
  const [searchQuery, setSearchQuery] = useState("")
  const loadedCourses = useMemo(() => (hasHydrated ? courses : []), [courses, hasHydrated])

  const resumeCourses = useMemo(() => selectResumeCourses(loadedCourses), [loadedCourses])
  const continueCourse = resumeCourses[0] ?? null
  const continueLesson = continueCourse ? selectContinueLesson(continueCourse) : null
  const visibleCourses = useMemo(() => filterCourses(loadedCourses, searchQuery), [loadedCourses, searchQuery])
  const hasCourses = loadedCourses.length > 0
  const displayLibraryPath = libraryPath ? formatDisplayPath(libraryPath) : null
  const isBootstrapping = !hasHydrated

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

  useEffect(() => {
    if (!isTauri()) return
    getBuildInfo()
      .then(setBuildInfo)
      .catch(() => setBuildInfo(null))
  }, [])

  useEffect(() => {
    if (!isTauri()) return
    if (typeof window === "undefined") return
    if (!window.location.search.includes("autoScan=")) return

    const url = new URL(window.location.href)
    const path = url.searchParams.get("autoScan")
    if (!path) return

    setScanMode("selecting")
    scanLibraryAt(path)
      .then((result) => {
        setError(null)
        setWarnings(result.warnings)
      })
      .catch((err) => {
        const detail = err instanceof Error
          ? `${err.name}: ${err.message}\n${err.stack ?? ""}`
          : `non-Error throw: ${JSON.stringify(err)}`
        frontendLog("error", `autoScan failed: ${detail}`)
        setError(`autoScan failed: ${detail}`)
      })
      .finally(() => setScanMode("idle"))
    // eslint-disable-next-line react-hooks/exhaustive-deps
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
      const result = await scanLibraryAt(path)
      setWarnings(result.warnings)
    } catch (err) {
      const detail = err instanceof Error
        ? `${err.name}: ${err.message}`
        : `non-Error throw: ${JSON.stringify(err)}`
      frontendLog("error", `scan failed at path: ${detail}`)
      setError(err instanceof Error ? err.message : `Failed to scan the selected folder (${detail}).`)
    } finally {
      setScanMode("idle")
    }
  }

  async function handleRefresh() {
    if (!libraryPath) return

    try {
      setScanMode("refreshing")
      setError(null)
      const result = await scanLibraryAt(libraryPath)
      setWarnings(result.warnings)
    } catch (err) {
      setError(err instanceof Error ? err.message : "Failed to refresh the current library.")
    } finally {
      setScanMode("idle")
    }
  }

  return (
    <div className="app-shell flex h-full min-h-0 flex-col bg-background text-foreground">
      <header className="relative z-10 flex h-14 shrink-0 items-center gap-4 border-b border-border bg-card px-4 md:pr-32">
        <div data-tauri-drag-region className="absolute inset-x-0 top-0 h-3" />
        <BrandLogo className="shrink-0" />

        <button
          type="button"
          onClick={() => setCmdOpen(true)}
          className="hidden h-9 w-80 shrink-0 items-center gap-2 rounded-lg border border-input bg-background px-4 text-left text-sm text-muted-foreground transition-colors hover:border-ring lg:flex xl:w-96"
        >
          <Search className="size-4" />
          <span className="truncate">What do you want to learn?</span>
          <kbd className="ml-auto rounded border border-border px-1.5 py-0.5 text-[10px]">Ctrl K</kbd>
        </button>

        <div className="ml-auto flex items-center gap-2">
          <Tooltip>
            <TooltipTrigger asChild>
              <Button type="button" variant="ghost" size="icon" onClick={() => setCmdOpen(true)} className="size-9 md:hidden" aria-label="Search">
                <Search className="size-4" />
              </Button>
            </TooltipTrigger>
            <TooltipContent>Search</TooltipContent>
          </Tooltip>
          <ThemeMenu />
          {hasCourses && (
            <Button type="button" variant="outline" size="sm" onClick={handleSelectFolder} disabled={scanMode !== "idle"} className="gap-2 rounded-md">
              {scanMode === "selecting" ? <Loader2 className="size-4 animate-spin" /> : <FolderOpen className="size-4" />}
              <span className="hidden sm:inline">Change root folder</span>
            </Button>
          )}
          {libraryPath && (
            <Tooltip>
              <TooltipTrigger asChild>
                <Button type="button" variant="ghost" size="icon" onClick={handleRefresh} disabled={scanMode !== "idle"} className="size-9" aria-label="Refresh library">
                  <RefreshCw className={cn("size-4", scanMode === "refreshing" && "animate-spin")} />
                </Button>
              </TooltipTrigger>
              <TooltipContent>Refresh library</TooltipContent>
            </Tooltip>
          )}
        </div>
      </header>

      <main className="min-h-0 flex-1 overflow-auto">
        <div className="mx-auto flex w-full max-w-[92rem] flex-col gap-9 px-4 py-5 md:px-7 md:py-7">
          {error && (
            <Alert variant="destructive" className="border-destructive/30 bg-destructive/10">
              <AlertTitle>Error</AlertTitle>
              <AlertDescription>{error}</AlertDescription>
            </Alert>
          )}

          {warnings.length > 0 && (
            <Alert className="border-primary/40 bg-primary/10 text-foreground">
              <AlertTitle>Scan warnings</AlertTitle>
              <AlertDescription>{warnings.join(" | ")}</AlertDescription>
            </Alert>
          )}

          <section>
            <div className="paper-panel-strong overflow-hidden rounded-2xl">
              <div className={cn("hero-panel relative grid gap-7 overflow-hidden p-7 md:p-9", resumeCourses.length > 0 && "md:grid-cols-[minmax(22rem,0.8fr)_minmax(0,1fr)]")}>
                <div className="relative z-10 flex flex-col justify-between gap-8">
                  <div className="flex flex-col gap-4">
                    <Badge variant="secondary" className="w-fit rounded-md">{isBootstrapping ? "Loading library" : "Welcome back"}</Badge>
                    <div className="flex flex-col gap-2">
                      <h1 className="max-w-2xl text-4xl font-bold tracking-tight md:text-5xl">
                        {isBootstrapping ? "Loading your library" : continueCourse ? continueCourse.name : "Build your learning library"}
                      </h1>
                      <p className="max-w-2xl text-base leading-7 text-muted-foreground">
                        {isBootstrapping
                          ? "Restoring your courses and progress from the local database."
                          : continueLesson
                          ? `Up next: ${continueLesson.name}`
                          : "Select a root folder to scan videos, documents, audio, subtitles, and progress into one learning workspace."}
                      </p>
                    </div>
                  </div>

                  <div className="flex flex-wrap items-center gap-3">
                    {isBootstrapping ? (
                      <Button type="button" size="lg" disabled className="rounded-md">
                        <Loader2 data-icon="inline-start" className="animate-spin" />
                        Loading library
                      </Button>
                    ) : continueCourse ? (
                      <Button type="button" size="lg" onClick={() => onOpenCourse(continueCourse, continueLesson?.id ?? null)} className="rounded-md">
                        <PlayCircle data-icon="inline-start" />
                        Resume learning
                      </Button>
                    ) : (
                      <Button type="button" size="lg" onClick={handleSelectFolder} disabled={scanMode !== "idle"} className="rounded-md">
                        <FolderOpen data-icon="inline-start" />
                        Scan root folder
                      </Button>
                    )}
                  </div>
                </div>

                {resumeCourses.length > 0 && (
                  <div className={cn(
                    "relative z-10 grid gap-4",
                    resumeCourses.length === 1 && "grid-cols-1",
                    resumeCourses.length === 2 && "sm:grid-cols-2",
                    resumeCourses.length >= 3 && "sm:grid-cols-2 xl:grid-cols-3"
                  )}>
                    {resumeCourses.map((course) => (
                      <ResumeCourseCard key={course.id} course={course} onOpenCourse={onOpenCourse} />
                    ))}
                  </div>
                )}
              </div>
            </div>
          </section>

          {!hasHydrated && <CourseSkeletonRail />}

          {hasCourses && (
            <section className="flex flex-col gap-4">
              <div className="flex flex-col gap-4 md:flex-row md:items-end md:justify-between">
                <div className="flex flex-col gap-1.5">
                  <h2 className="text-2xl font-semibold tracking-tight">Your courses</h2>
                  <p className="text-sm text-muted-foreground">
                    {visibleCourses.length} course{visibleCourses.length === 1 ? "" : "s"} shown{displayLibraryPath ? ` from ${displayLibraryPath}` : ""}
                  </p>
                </div>
                <div className="flex flex-col gap-3 sm:flex-row sm:items-center">
                  <InputGroup className="h-10 bg-background sm:w-80">
                    <InputGroupAddon>
                      <Search />
                    </InputGroupAddon>
                    <InputGroupInput
                      value={searchQuery}
                      onChange={(event) => setSearchQuery(event.target.value)}
                      placeholder="Filter courses"
                    />
                  </InputGroup>
                  <ToggleGroup
                    type="single"
                    value={viewMode}
                    onValueChange={(value) => {
                      if (value === "grid" || value === "list") setViewMode(value)
                    }}
                    size="lg"
                    className="hidden gap-1 sm:flex"
                  >
                    <Tooltip>
                      <TooltipTrigger asChild>
                        <ToggleGroupItem value="grid" aria-label="Grid view" className="size-10 px-0">
                          <LayoutGrid />
                        </ToggleGroupItem>
                      </TooltipTrigger>
                      <TooltipContent>Grid view</TooltipContent>
                    </Tooltip>
                    <Tooltip>
                      <TooltipTrigger asChild>
                        <ToggleGroupItem value="list" aria-label="List view" className="size-10 px-0">
                          <List />
                        </ToggleGroupItem>
                      </TooltipTrigger>
                      <TooltipContent>List view</TooltipContent>
                    </Tooltip>
                  </ToggleGroup>
                </div>
              </div>
              <div className={viewMode === "list" ? "flex flex-col gap-3" : "grid items-start gap-5 sm:grid-cols-2 xl:grid-cols-4"}>
                {visibleCourses.map((course) => (
                  <DashboardCourseCard key={course.id} course={course} viewMode={viewMode} onOpenCourse={onOpenCourse} />
                ))}
              </div>
            </section>
          )}

          <footer className="flex flex-col gap-2 border-t border-border py-8 text-xs text-muted-foreground md:flex-row md:items-center md:justify-between">
            <span>melearner keeps courses and progress on this machine.</span>
            {buildInfo && (
              <span className="tabular-nums">
                v{buildInfo.version} · {buildInfo.git_sha} · built {new Date(Number(buildInfo.build_timestamp) * 1000).toISOString().slice(0, 10)}
              </span>
            )}
          </footer>
        </div>
      </main>

      <CommandDialog open={cmdOpen} onOpenChange={setCmdOpen}>
        <CommandInput placeholder="Search courses and lessons…" />
        <CommandList>
          <CommandEmpty>No results found.</CommandEmpty>
          {loadedCourses.length > 0 && (
            <CommandGroup heading="Courses">
              {loadedCourses.filter((course) => !course.missingSince).slice(0, 12).map((course) => (
                <CommandItem
                  key={course.id}
                  value={`course ${course.name}`}
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
          {loadedCourses.length > 0 && (
            <CommandGroup heading="Lessons">
              {allLessons(loadedCourses).slice(0, 50).map(({ course, lesson }) => (
                  <CommandItem
                    key={lesson.id}
                    value={`lesson ${lesson.name} ${course.name} ${lesson.sectionName}`}
                    onSelect={() => {
                      setCmdOpen(false)
                      onOpenCourse(course, lesson.id)
                    }}
                  >
                    <PlayCircle className="size-4" />
                    <span className="truncate">{lesson.name}</span>
                  </CommandItem>
                ))}
            </CommandGroup>
          )}
        </CommandList>
      </CommandDialog>
    </div>
  )
}

function summarizeCourse(course: Course) {
  const lessons = course.sections.flatMap((section) => section.lessons)
  const completed = lessons.filter((lesson) => lesson.completed).length
  return {
    totalLessons: lessons.length,
    completedLessons: completed,
    progress: lessons.length > 0 ? Math.round((completed / lessons.length) * 100) : 0,
  }
}

function selectResumeCourses(courses: Course[]) {
  const sorted = courses
    .filter((course) => !course.missingSince)
    .sort((a, b) => {
      const accessOrder = (b.lastAccessed ?? "").localeCompare(a.lastAccessed ?? "")
      if (accessOrder !== 0) return accessOrder
      return summarizeCourse(b).progress - summarizeCourse(a).progress
    })

  const activeOrRecent = sorted.filter((course) => course.lastAccessed || summarizeCourse(course).progress > 0)
  return (activeOrRecent.length > 0 ? activeOrRecent : sorted.filter((course) => summarizeCourse(course).progress < 100))
    .slice(0, 3)
}

function selectContinueLesson(course: Course) {
  const lessons = course.sections.flatMap((section) => section.lessons)
  return lessons.find((lesson) => !lesson.completed) ?? lessons[0] ?? null
}

function filterCourses(courses: Course[], query: string) {
  const normalized = query.trim().toLowerCase()
  if (!normalized) return courses
  return courses.filter((course) => {
    if (course.name.toLowerCase().includes(normalized)) return true
    return course.sections.some((section) => {
      if (section.name.toLowerCase().includes(normalized)) return true
      return section.lessons.some((lesson) => lesson.name.toLowerCase().includes(normalized))
    })
  })
}

function allLessons(courses: Course[]): Array<{ course: Course; lesson: Lesson }> {
  return courses
    .filter((course) => !course.missingSince)
    .flatMap((course) => course.sections.flatMap((section) => section.lessons.map((lesson) => ({ course, lesson }))))
}

function formatDisplayPath(path: string): string {
  const normalized = path.replace(/\\/g, "/")
  const unixHome = normalized.match(/^\/(home|Users)\/[^/]+(?:\/(.*))?$/)
  if (unixHome) return unixHome[2] ? `~/${unixHome[2]}` : "~"

  const windowsHome = path.match(/^[A-Za-z]:\\Users\\[^\\]+(?:\\(.*))?$/)
  if (windowsHome) return windowsHome[1] ? `~\\${windowsHome[1]}` : "~"

  return path
}

function ResumeCourseCard({ course, onOpenCourse }: { course: Course; onOpenCourse: (course: Course, lessonId?: string | null) => void }) {
  const summary = summarizeCourse(course)
  const nextLesson = selectContinueLesson(course)

  return (
    <article
      role="button"
      tabIndex={0}
      onClick={() => onOpenCourse(course, nextLesson?.id ?? null)}
      onKeyDown={(event) => {
        if (event.key === "Enter" || event.key === " ") {
          event.preventDefault()
          onOpenCourse(course, nextLesson?.id ?? null)
        }
      }}
      className="group flex min-h-60 cursor-pointer flex-col overflow-hidden rounded-xl border border-border bg-background/90 transition-[border-color,box-shadow] hover:border-primary/70 hover:shadow-[var(--shadow-panel)]"
    >
      <CourseArtwork course={course} className="h-32 min-h-32 shrink-0" />
      <div className="flex min-h-0 flex-1 flex-col justify-between gap-4 p-5">
        <div className="flex min-h-0 flex-col gap-1.5">
          <div className="flex items-center justify-between gap-3 text-xs text-muted-foreground">
            <span className="truncate">Resume</span>
            <span className="shrink-0 font-medium text-foreground tabular-nums">{summary.progress}%</span>
          </div>
          <h2 className="line-clamp-2 text-base font-semibold leading-snug tracking-tight">{course.name}</h2>
          <p className="line-clamp-1 text-sm text-muted-foreground">{nextLesson?.name ?? "No lesson selected"}</p>
        </div>
        <div className="flex flex-col gap-2">
          <Progress value={summary.progress} className="h-1.5" />
          <div className="flex items-center gap-1.5 text-sm font-medium text-primary">
            <PlayCircle className="size-4" />
            Continue
          </div>
        </div>
      </div>
    </article>
  )
}

function DashboardCourseCard({ course, viewMode, onOpenCourse }: { course: Course; viewMode: ViewMode; onOpenCourse: (course: Course, lessonId?: string | null) => void }) {
  const summary = summarizeCourse(course)
  const nextLesson = selectContinueLesson(course)
  const firstSection = course.sections[0]
  const isList = viewMode === "list"
  const isMissing = Boolean(course.missingSince)

  return (
    <article
      role={isMissing ? undefined : "button"}
      tabIndex={isMissing ? -1 : 0}
      aria-disabled={isMissing || undefined}
      onClick={() => {
        if (!isMissing) onOpenCourse(course, nextLesson?.id ?? null)
      }}
      onKeyDown={(event) => {
        if (isMissing) return
        if (event.key === "Enter" || event.key === " ") {
          event.preventDefault()
          onOpenCourse(course, nextLesson?.id ?? null)
        }
      }}
      className={cn(
        "paper-panel group cursor-pointer overflow-hidden rounded-xl transition-[border-color,box-shadow] hover:border-primary/70 hover:shadow-[var(--shadow-panel)] [contain-intrinsic-size:280px] [content-visibility:auto]",
        isList ? "grid gap-0 md:grid-cols-[240px_minmax(0,1fr)]" : "flex flex-col",
        isMissing && "cursor-default opacity-75 hover:border-border hover:shadow-none"
      )}
    >
      <CourseArtwork course={course} className={cn("h-40 min-h-40 shrink-0", isList && "h-full min-h-40")} />
      <div className={cn("flex min-w-0 flex-col gap-4 p-5", isList && "flex-1")}>
        <div className="flex flex-col gap-2">
          <div className="flex flex-wrap items-center gap-2">
            <Badge variant="secondary" className="rounded-md">{course.sections.length} sections</Badge>
            {isMissing && <Badge variant="outline" className="rounded-md"><AlertTriangle className="size-3" /> Missing folder</Badge>}
            {summary.progress === 100 && <Badge className="rounded-md"><CheckCircle2 className="size-3" /> Complete</Badge>}
          </div>
          <h3 className="line-clamp-2 text-lg font-semibold leading-snug tracking-tight">{course.name}</h3>
          <p className="line-clamp-1 text-sm text-muted-foreground">{cleanSectionName(firstSection?.name ?? "Course") || "Course"}</p>
        </div>
        <div className="flex flex-col gap-2">
          <div className="flex items-center justify-between text-xs text-muted-foreground">
            <span>{summary.completedLessons}/{summary.totalLessons} lessons</span>
            <span className="font-medium text-foreground tabular-nums">{summary.progress}%</span>
          </div>
          <Progress value={summary.progress} className="h-1.5" />
        </div>
      </div>
    </article>
  )
}

function CourseSkeletonRail() {
  return (
    <div className="grid gap-4 md:grid-cols-2 xl:grid-cols-4">
      {Array.from({ length: 4 }).map((_, index) => (
        <div key={index} className="paper-panel rounded-xl p-4">
          <div className="flex flex-col gap-4">
            <Skeleton className="h-32 w-full" />
            <Skeleton className="h-4 w-24" />
            <Skeleton className="h-5 w-3/4" />
            <Skeleton className="h-2 w-full" />
          </div>
        </div>
      ))}
    </div>
  )
}
