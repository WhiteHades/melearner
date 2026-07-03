"use client"

import { useCallback, useEffect, useMemo, useState } from "react"
import { useShallow } from "zustand/react/shallow"
import { parseAsString, useQueryState } from "nuqs"
import {
  BookOpen,
  CheckCircle2,
  Clock3,
  FolderOpen,
  GraduationCap,
  LayoutGrid,
  Loader2,
  PlayCircle,
  RefreshCw,
  Search,
} from "lucide-react"
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
import { Progress } from "@/components/ui/progress"
import { Skeleton } from "@/components/ui/skeleton"
import { ToggleGroup, ToggleGroupItem } from "@/components/ui/toggle-group"
import { frontendLog } from "@/lib/frontend-log"
import { markCourseAccessed, scanLibraryAt } from "@/lib/operations"
import { useCourseStore } from "@/lib/stores/course-store"
import { getBuildInfo, isTauri, selectFolderDialog, type BuildInfo } from "@/lib/tauri"
import { cleanSectionName, cn, formatDuration } from "@/lib/utils"
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
    if (view === "viewer" && hasHydrated && !selectedCourse) {
      setViewParam("library")
      setCourseId(null)
      setLessonId(null)
    }
  }, [view, hasHydrated, selectedCourse, setViewParam, setCourseId, setLessonId])

  const handleOpenCourse = useCallback(
    (course: Course, selectedLessonId: string | null = null) => {
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

  const stats = useMemo(() => summarizeLibrary(loadedCourses), [loadedCourses])
  const continueCourse = useMemo(() => selectContinueCourse(loadedCourses), [loadedCourses])
  const continueLesson = continueCourse ? selectContinueLesson(continueCourse) : null
  const recentCourses = useMemo(() => selectRecentCourses(loadedCourses), [loadedCourses])
  const visibleCourses = useMemo(() => filterCourses(loadedCourses, searchQuery), [loadedCourses, searchQuery])

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
      <header className="relative z-10 flex h-14 shrink-0 items-center gap-4 border-b border-border bg-card px-4">
        <div data-tauri-drag-region className="absolute inset-x-0 top-0 h-3" />
        <div className="flex items-center gap-3">
          <div className="text-2xl font-bold tracking-tight">melearner</div>
          <Button type="button" variant="ghost" size="sm" className="hidden text-muted-foreground md:inline-flex">
            My Learning
          </Button>
        </div>

        <button
          type="button"
          onClick={() => setCmdOpen(true)}
            className="hidden h-9 min-w-0 flex-1 items-center gap-2 rounded-lg border border-input bg-background px-4 text-left text-sm text-muted-foreground transition-colors hover:border-ring md:flex"
        >
          <Search className="size-4" />
          <span className="truncate">What do you want to learn?</span>
          <kbd className="ml-auto rounded border border-border px-1.5 py-0.5 text-[10px]">Ctrl K</kbd>
        </button>

        <div className="ml-auto flex items-center gap-2">
          <ToggleGroup
            type="single"
            value={viewMode}
            onValueChange={(value) => {
              if (value === "grid" || value === "list") setViewMode(value)
            }}
            variant="outline"
            size="sm"
            className="hidden rounded-lg border bg-background px-0.5 sm:flex"
          >
            <ToggleGroupItem value="grid" aria-label="Grid view" className="size-8 px-0">
              <LayoutGrid className="size-4" />
            </ToggleGroupItem>
            <ToggleGroupItem value="list" aria-label="List view" className="size-8 px-0">
              <BookOpen className="size-4" />
            </ToggleGroupItem>
          </ToggleGroup>

          <Button type="button" variant="ghost" size="icon" onClick={() => setCmdOpen(true)} className="size-9 md:hidden" aria-label="Search">
            <Search className="size-4" />
          </Button>
          <ThemeMenu />
          <Button type="button" variant="outline" size="sm" onClick={handleSelectFolder} disabled={scanMode !== "idle"} className="gap-2 rounded-md">
            {scanMode === "selecting" ? <Loader2 className="size-4 animate-spin" /> : <FolderOpen className="size-4" />}
            <span className="hidden sm:inline">Choose folder</span>
          </Button>
          {libraryPath && (
            <Button type="button" variant="ghost" size="icon" onClick={handleRefresh} disabled={scanMode !== "idle"} className="size-9" aria-label="Refresh library">
              <RefreshCw className={cn("size-4", scanMode === "refreshing" && "animate-spin")} />
            </Button>
          )}
        </div>
      </header>

      <main className="min-h-0 flex-1 overflow-auto">
        <div className="mx-auto flex w-full max-w-7xl flex-col gap-8 px-4 py-4 md:px-6 md:py-6">
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

          <section className="grid gap-4 xl:grid-cols-[minmax(0,1fr)_320px]">
            <div className="paper-panel-strong overflow-hidden rounded-2xl">
              <div className="hero-panel relative grid min-h-[280px] gap-6 overflow-hidden p-6 md:grid-cols-[minmax(0,1fr)_minmax(320px,0.85fr)] md:p-8">
                <div className="relative z-10 flex flex-col justify-between gap-8">
                  <div className="flex flex-col gap-4">
                    <Badge variant="secondary" className="w-fit rounded-md">Welcome back</Badge>
                    <div className="flex flex-col gap-2">
                      <p className="text-sm font-semibold text-primary">Your learning is local and ready offline.</p>
                      <h1 className="max-w-2xl text-3xl font-bold tracking-tight md:text-4xl">
                        {continueCourse ? continueCourse.name : "Build your learning library"}
                      </h1>
                      <p className="max-w-2xl text-sm leading-6 text-muted-foreground">
                        {continueLesson
                          ? `Up next: ${continueLesson.name}`
                          : "Choose a course folder to scan videos, documents, audio, subtitles, and progress into one learning workspace."}
                      </p>
                    </div>
                  </div>

                  <div className="flex flex-wrap items-center gap-3">
                    {continueCourse ? (
                      <Button type="button" onClick={() => onOpenCourse(continueCourse, continueLesson?.id ?? null)} className="rounded-md">
                        <PlayCircle className="size-4" />
                        Resume learning
                      </Button>
                    ) : (
                      <Button type="button" onClick={handleSelectFolder} disabled={scanMode !== "idle"} className="rounded-md">
                        <FolderOpen className="size-4" />
                        Scan course folder
                      </Button>
                    )}
                    {libraryPath && <p className="max-w-lg truncate text-xs text-muted-foreground">{libraryPath}</p>}
                  </div>
                </div>

                  <div className="relative z-10 hidden min-h-56 overflow-hidden rounded-xl border border-border bg-background md:block">
                  <div className="course-art absolute inset-y-0 right-0 w-1/2" />
                  <div className="relative flex h-full flex-col justify-between p-6">
                    <div className="flex items-center justify-between text-xs text-muted-foreground">
                      <span>Up next</span>
                      <span>{continueLesson?.type ?? "course"}</span>
                    </div>
                    <div className="flex flex-col gap-3">
                        <div className="flex size-14 items-center justify-center rounded-lg bg-primary text-primary-foreground shadow-[0_0_0_1px_var(--primary)]">
                        <PlayCircle className="size-7" />
                      </div>
                      <div className="flex flex-col gap-1">
                        <h2 className="text-xl font-semibold">{continueLesson?.name ?? "No lesson selected"}</h2>
                        <p className="text-sm text-muted-foreground">
                          {continueCourse ? `${summarizeCourse(continueCourse).progress}% complete` : "Scan once, learn anywhere."}
                        </p>
                      </div>
                    </div>
                  </div>
                </div>
              </div>
            </div>

            <div className="grid gap-4 sm:grid-cols-3 xl:grid-cols-1">
              <GoalCard label="Learning items" value={`${stats.completedLessons}/${stats.totalLessons}`} progress={stats.progress} />
              <MetricCard icon={GraduationCap} label="Courses" value={String(loadedCourses.length)} hint={hasHydrated ? "library ready" : "restoring"} />
              <MetricCard icon={Clock3} label="Runtime" value={stats.totalDuration > 0 ? formatDuration(stats.totalDuration) : "0:00"} hint="from scanned metadata" />
            </div>
          </section>

          <section className="flex flex-col gap-4">
            <div className="flex flex-col gap-3 md:flex-row md:items-end md:justify-between">
              <div className="flex flex-col gap-1">
                <h2 className="text-xl font-semibold tracking-tight">Resume your exploration</h2>
                <p className="text-sm text-muted-foreground">Recent and in-progress courses stay close to the top.</p>
              </div>
              <label className="flex h-10 min-w-0 items-center gap-2 rounded-lg border border-input bg-background px-3 text-sm md:w-80">
                <Search className="size-4 text-muted-foreground" />
                <input
                  value={searchQuery}
                  onChange={(event) => setSearchQuery(event.target.value)}
                  placeholder="Filter courses"
                  className="min-w-0 flex-1 bg-transparent outline-none placeholder:text-muted-foreground"
                />
              </label>
            </div>

            {!hasHydrated ? (
              <CourseSkeletonRail />
            ) : loadedCourses.length === 0 ? (
              <EmptyLibrary onSelectFolder={handleSelectFolder} disabled={scanMode !== "idle"} />
            ) : recentCourses.length > 0 ? (
              <CourseRail courses={recentCourses} onOpenCourse={onOpenCourse} />
            ) : (
              <CourseRail courses={visibleCourses.slice(0, 4)} onOpenCourse={onOpenCourse} />
            )}
          </section>

          {loadedCourses.length > 0 && (
            <section className="flex flex-col gap-4">
              <div className="flex items-center justify-between gap-4">
                <div className="flex flex-col gap-1">
                  <h2 className="text-xl font-semibold tracking-tight">Your courses</h2>
                  <p className="text-sm text-muted-foreground">{visibleCourses.length} course{visibleCourses.length === 1 ? "" : "s"} shown</p>
                </div>
              </div>
              <div className={viewMode === "list" ? "flex flex-col gap-3" : "grid gap-4 sm:grid-cols-2 xl:grid-cols-4"}>
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
              {loadedCourses.slice(0, 12).map((course) => (
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

function summarizeLibrary(courses: Course[]) {
  let totalLessons = 0
  let completedLessons = 0
  let totalDuration = 0

  for (const course of courses) {
    totalDuration += course.totalDuration
    for (const section of course.sections) {
      totalLessons += section.lessons.length
      completedLessons += section.lessons.filter((lesson) => lesson.completed).length
    }
  }

  return {
    totalLessons,
    completedLessons,
    totalDuration,
    progress: totalLessons > 0 ? Math.round((completedLessons / totalLessons) * 100) : 0,
  }
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

function selectContinueCourse(courses: Course[]) {
  return [...courses]
    .sort((a, b) => (b.lastAccessed ?? "").localeCompare(a.lastAccessed ?? ""))
    .find((course) => summarizeCourse(course).progress < 100) ?? courses[0] ?? null
}

function selectContinueLesson(course: Course) {
  const lessons = course.sections.flatMap((section) => section.lessons)
  return lessons.find((lesson) => !lesson.completed) ?? lessons[0] ?? null
}

function selectRecentCourses(courses: Course[]) {
  return [...courses]
    .filter((course) => course.lastAccessed)
    .sort((a, b) => (b.lastAccessed ?? "").localeCompare(a.lastAccessed ?? ""))
    .slice(0, 4)
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
  return courses.flatMap((course) => course.sections.flatMap((section) => section.lessons.map((lesson) => ({ course, lesson }))))
}

function GoalCard({ label, value, progress }: { label: string; value: string; progress: number }) {
  return (
    <div className="paper-panel rounded-xl p-5">
      <div className="flex flex-col gap-4">
        <div className="flex items-center justify-between gap-3">
          <span className="text-sm font-medium text-muted-foreground">{label}</span>
          <Badge variant="outline" className="rounded-md">{progress}%</Badge>
        </div>
        <div className="flex flex-col gap-2">
          <div className="text-2xl font-semibold tabular-nums">{value}</div>
          <Progress value={progress} className="h-2" />
        </div>
      </div>
    </div>
  )
}

function MetricCard({ icon: Icon, label, value, hint }: { icon: typeof BookOpen; label: string; value: string; hint: string }) {
  return (
    <div className="paper-panel rounded-xl p-5">
      <div className="flex items-start justify-between gap-4">
        <div className="flex flex-col gap-2">
          <span className="text-sm text-muted-foreground">{label}</span>
          <span className="text-2xl font-semibold tracking-tight">{value}</span>
          <span className="text-xs text-muted-foreground">{hint}</span>
        </div>
        <div className="flex size-10 items-center justify-center rounded-lg bg-secondary text-secondary-foreground">
          <Icon className="size-4" />
        </div>
      </div>
    </div>
  )
}

function CourseRail({ courses, onOpenCourse }: { courses: Course[]; onOpenCourse: (course: Course, lessonId?: string | null) => void }) {
  return (
    <div className="grid gap-4 md:grid-cols-2 xl:grid-cols-4">
      {courses.map((course) => (
        <DashboardCourseCard key={course.id} course={course} viewMode="grid" onOpenCourse={onOpenCourse} />
      ))}
    </div>
  )
}

function DashboardCourseCard({ course, viewMode, onOpenCourse }: { course: Course; viewMode: ViewMode; onOpenCourse: (course: Course, lessonId?: string | null) => void }) {
  const summary = summarizeCourse(course)
  const nextLesson = selectContinueLesson(course)
  const firstSection = course.sections[0]
  const isList = viewMode === "list"

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
      className={cn(
        "paper-panel group cursor-pointer overflow-hidden rounded-xl transition-[border-color,box-shadow] hover:border-primary/70 hover:shadow-[var(--shadow-panel)]",
        isList ? "grid gap-0 md:grid-cols-[240px_minmax(0,1fr)]" : "flex flex-col"
      )}
    >
      <CourseArtwork course={course} className={cn("min-h-36", isList && "min-h-full")} />
      <div className="flex min-w-0 flex-1 flex-col gap-4 p-4">
        <div className="flex flex-col gap-2">
          <div className="flex flex-wrap items-center gap-2">
            <Badge variant="secondary" className="rounded-md">{course.sections.length} modules</Badge>
            {summary.progress === 100 && <Badge className="rounded-md"><CheckCircle2 className="size-3" /> Complete</Badge>}
          </div>
          <h3 className="line-clamp-2 text-base font-semibold leading-snug tracking-tight">{course.name}</h3>
          <p className="line-clamp-1 text-xs text-muted-foreground">{cleanSectionName(firstSection?.name ?? "Course") || "Course"}</p>
        </div>
        <div className="mt-auto flex flex-col gap-2">
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

function EmptyLibrary({ onSelectFolder, disabled }: { onSelectFolder: () => void; disabled: boolean }) {
  return (
    <div className="paper-panel rounded-2xl border-dashed p-10 text-center">
      <div className="mx-auto flex max-w-md flex-col items-center gap-5">
        <div className="flex size-14 items-center justify-center rounded-xl bg-secondary text-secondary-foreground">
          <FolderOpen className="size-6" />
        </div>
        <div className="flex flex-col gap-2">
          <h3 className="text-lg font-semibold tracking-tight">No courses yet</h3>
          <p className="text-sm text-muted-foreground">Choose a root folder. Each subfolder becomes a course, with videos and files grouped into modules.</p>
        </div>
        <Button type="button" onClick={onSelectFolder} disabled={disabled}>
          <FolderOpen className="size-4" />
          Choose folder
        </Button>
      </div>
    </div>
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
