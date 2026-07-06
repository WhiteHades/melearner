"use client"

import { useCallback, useDeferredValue, useEffect, useMemo, useRef, useState } from "react"
import { useShallow } from "zustand/react/shallow"
import { parseAsString, useQueryState } from "nuqs"
import {
  BarChart3,
  BookOpen,
  CalendarDays,
  CheckCircle2,
  Clock,
  FolderOpen,
  HardDrive,
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
import {
  loadActivityDays,
  markCourseAccessed,
  scanLibraryAt,
  writeCourseMarkers,
} from "@/lib/operations"
import { search as searchLibrary, type SearchResult as LibrarySearchResult } from "@/lib/search"
import { buildLearningStats, type LearningStats } from "@/lib/stats"
import {
  buildDashboardCourseCards,
  selectCommandPaletteGroupOrder,
  selectCommandLessons,
  selectResumeCourseCards,
  selectVisibleCourseCards,
  type CourseSummary,
} from "@/lib/dashboard-selectors"
import { useCourseStore } from "@/lib/stores/course-store"
import { getBuildInfo, getStartupRoute, isTauri, selectFolderDialog, type BuildInfo } from "@/lib/tauri"
import { cleanSectionName, cn } from "@/lib/utils"
import type { ActivityDay, Course, Lesson } from "@/types"

type View = "library" | "viewer"
type ViewMode = "grid" | "list"
const EMPTY_SEARCH_RESULTS: LibrarySearchResult[] = []
const EMPTY_COMMAND_LESSONS: Array<{ course: Course; lesson: Lesson }> = []

function lessonBelongsToCourse(course: Course, lessonId: string | null): lessonId is string {
  if (!lessonId) return false
  return course.sections.some((section) => section.lessons.some((lesson) => lesson.id === lessonId))
}

export function HomeScreen() {
  const [viewParam, setViewParam] = useQueryState("view", parseAsString.withDefault("library"))
  const [courseId, setCourseId] = useQueryState("course", parseAsString)
  const [lessonId, setLessonId] = useQueryState("lesson", parseAsString)
  const startupRouteAppliedRef = useRef(false)

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

  useEffect(() => {
    if (!hasHydrated || !isTauri() || startupRouteAppliedRef.current || courses.length === 0) return
    startupRouteAppliedRef.current = true
    let cancelled = false

    getStartupRoute()
      .then((route) => {
        if (cancelled || !route) return
        const course = courses.find((course) => course.id === route.courseId && !course.missingSince)
        if (!course) {
          frontendLog("warn", "startup.route.courseMissing", { courseId: route.courseId })
          return
        }

        const selectedLessonId = lessonBelongsToCourse(course, route.lessonId) ? route.lessonId : null
        if (route.lessonId && !selectedLessonId) {
          frontendLog("warn", "startup.route.lessonMissing", {
            courseId: route.courseId,
            lessonId: route.lessonId,
          })
        }

        setCourseId(course.id)
        setLessonId(selectedLessonId)
        setViewParam("viewer")
        void markCourseAccessed(course.id)
      })
      .catch((err) => {
        frontendLog("warn", "startup.route.failed", { error: err instanceof Error ? err.message : String(err) })
      })

    return () => {
      cancelled = true
    }
  }, [courses, hasHydrated, setCourseId, setLessonId, setViewParam])

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
    if (!hasHydrated) {
      return (
        <main className="flex h-full items-center justify-center bg-background text-foreground">
          <div className="flex items-center gap-2 text-sm text-muted-foreground">
            <Loader2 className="size-4 animate-spin" />
            Loading course
          </div>
        </main>
      )
    }

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
  const deferredSearchQuery = useDeferredValue(searchQuery)
  const searchKey = deferredSearchQuery.trim()
  const [searchState, setSearchState] = useState<{ query: string; results: LibrarySearchResult[] }>({ query: "", results: [] })
  const [activityDays, setActivityDays] = useState<ActivityDay[]>([])
  const loadedCourses = useMemo(() => (hasHydrated ? courses : []), [courses, hasHydrated])
  const markerSyncCourses = useMemo(() => loadedCourses.filter((course) => !course.missingSince), [loadedCourses])
  const markerSyncCoursesRef = useRef(markerSyncCourses)
  const markerSyncKey = useMemo(() => {
    return markerSyncCourses
      .map((course) => `${course.identityId}\0${course.path}`)
      .sort()
      .join("\x01")
  }, [markerSyncCourses])

  const courseCards = useMemo(() => buildDashboardCourseCards(loadedCourses), [loadedCourses])
  const resumeCourseCards = useMemo(() => selectResumeCourseCards(courseCards), [courseCards])
  const continueCourseCard = resumeCourseCards[0] ?? null
  const continueCourse = continueCourseCard?.course ?? null
  const continueLesson = continueCourseCard?.nextLesson ?? null
  const activeSearchResults = searchState.query === searchKey ? searchState.results : EMPTY_SEARCH_RESULTS
  const visibleCourseCards = useMemo(() => selectVisibleCourseCards(courseCards, searchKey, activeSearchResults), [courseCards, searchKey, activeSearchResults])
  const commandLessons = useMemo(
    () => cmdOpen ? selectCommandLessons(loadedCourses, searchKey, activeSearchResults, 50) : EMPTY_COMMAND_LESSONS,
    [cmdOpen, loadedCourses, searchKey, activeSearchResults]
  )
  const commandGroupOrder = useMemo(
    () => selectCommandPaletteGroupOrder(searchKey, commandLessons.length),
    [searchKey, commandLessons.length]
  )
  const stats = useMemo(() => buildLearningStats(loadedCourses, activityDays), [loadedCourses, activityDays])
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
    if (!hasHydrated) return
    let cancelled = false
    loadActivityDays(84)
      .then((days) => {
        if (!cancelled) setActivityDays(days)
      })
      .catch(() => {
        if (!cancelled) setActivityDays([])
      })
    return () => {
      cancelled = true
    }
  }, [hasHydrated])

  useEffect(() => {
    const query = searchKey
    if (!query) return

    let cancelled = false
    searchLibrary(query, 200)
      .then((results) => {
        if (!cancelled) setSearchState({ query, results })
      })
      .catch((err) => {
        if (!cancelled) {
          setSearchState({ query, results: [] })
          frontendLog("warn", "library.search.failed", { error: err instanceof Error ? err.message : String(err) })
        }
      })
    return () => {
      cancelled = true
    }
  }, [searchKey, loadedCourses])

  useEffect(() => {
    markerSyncCoursesRef.current = markerSyncCourses
  }, [markerSyncCourses])

  useEffect(() => {
    if (!hasHydrated || !isTauri() || markerSyncKey.length === 0) return
    let cancelled = false
    writeCourseMarkers(markerSyncCoursesRef.current)
      .then((markerWarnings) => {
        if (cancelled) return
        if (markerWarnings.length > 0) {
          setWarnings((current) => [...current, ...markerWarnings])
        }
      })
      .catch((err) => {
        if (!cancelled) {
          setWarnings((current) => [...current, err instanceof Error ? err.message : "Could not update course identity markers."])
        }
      })
    return () => {
      cancelled = true
    }
  }, [hasHydrated, markerSyncKey])

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
              <div className={cn("hero-panel relative grid gap-7 overflow-hidden p-7 md:p-9", resumeCourseCards.length > 0 && "md:grid-cols-[minmax(22rem,0.8fr)_minmax(0,1fr)]")}>
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

                {resumeCourseCards.length > 0 && (
                  <div className={cn(
                    "relative z-10 grid gap-4",
                    resumeCourseCards.length === 1 && "grid-cols-1",
                    resumeCourseCards.length === 2 && "sm:grid-cols-2",
                    resumeCourseCards.length >= 3 && "sm:grid-cols-2 xl:grid-cols-3"
                  )}>
                    {resumeCourseCards.map(({ course, summary, nextLesson }) => (
                      <ResumeCourseCard key={course.id} course={course} summary={summary} nextLesson={nextLesson} onOpenCourse={onOpenCourse} />
                    ))}
                  </div>
                )}
              </div>
            </div>
          </section>

          {!hasHydrated && <CourseSkeletonRail />}

          {hasCourses && (
            <LibraryStatsPanel stats={stats} />
          )}

          {hasCourses && (
            <section className="flex flex-col gap-4">
              <div className="flex flex-col gap-4 md:flex-row md:items-end md:justify-between">
                <div className="flex flex-col gap-1.5">
                  <h2 className="text-2xl font-semibold tracking-tight">Your courses</h2>
                  <p className="text-sm text-muted-foreground">
                    {visibleCourseCards.length} course{visibleCourseCards.length === 1 ? "" : "s"} shown{displayLibraryPath ? ` from ${displayLibraryPath}` : ""}
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
                {visibleCourseCards.map(({ course, summary, nextLesson }) => (
                  <DashboardCourseCard key={course.id} course={course} summary={summary} nextLesson={nextLesson} viewMode={viewMode} onOpenCourse={onOpenCourse} />
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

      <CommandDialog open={cmdOpen} onOpenChange={setCmdOpen} commandProps={{ shouldFilter: false }}>
        <CommandInput placeholder="Search courses and lessons…" value={searchQuery} onValueChange={setSearchQuery} />
        <CommandList>
          <CommandEmpty>No results found.</CommandEmpty>
          {commandGroupOrder.map((group) => {
            if (group === "courses") {
              return loadedCourses.length > 0 ? (
                <CommandGroup key="courses" heading="Courses">
                  {visibleCourseCards.filter(({ course }) => !course.missingSince).slice(0, 12).map(({ course }) => (
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
              ) : null
            }

            return loadedCourses.length > 0 && commandLessons.length > 0 ? (
              <CommandGroup key="lessons" heading="Lessons">
                {commandLessons.map(({ course, lesson }) => (
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
            ) : null
          })}
        </CommandList>
      </CommandDialog>

    </div>
  )
}

function LibraryStatsPanel({
  stats,
}: {
  stats: LearningStats
}) {
  const topCourses = stats.courses.slice(0, 4)

  return (
    <section className="grid gap-5 xl:grid-cols-[minmax(0,1.4fr)_minmax(22rem,0.9fr)]">
      <div className="paper-panel rounded-2xl p-5">
        <div className="grid gap-3 sm:grid-cols-2 xl:grid-cols-4">
          <StatTile
            icon={BookOpen}
            label="Courses"
            value={`${stats.availableCourses}/${stats.totalCourses}`}
            detail={stats.missingCourses > 0 ? `${stats.missingCourses} missing` : "All available"}
          />
          <StatTile
            icon={BarChart3}
            label="Completed"
            value={`${stats.completionPercent}%`}
            detail={`${stats.completedLessons}/${stats.lessons} lessons`}
          />
          <StatTile
            icon={Clock}
            label="Watched"
            value={formatStatDuration(stats.watchedSeconds)}
            detail={stats.totalSeconds > 0 ? `${formatStatDuration(Math.max(0, stats.totalSeconds - stats.watchedSeconds))} left` : `${stats.activeDays} active days`}
          />
          <StatTile
            icon={HardDrive}
            label="Storage"
            value={formatBytes(stats.bytes)}
            detail={`${stats.sections} sections`}
          />
        </div>

        <div className="mt-5 grid gap-5 lg:grid-cols-2">
          <div className="flex min-w-0 flex-col gap-3">
            <h3 className="text-sm font-semibold">Media mix</h3>
            <div className="flex flex-col gap-2">
              {stats.mediaTypes.map((item) => (
                <MediaTypeRow key={item.type} item={item} totalBytes={stats.bytes} />
              ))}
            </div>
          </div>
          <div className="flex min-w-0 flex-col gap-3">
            <h3 className="text-sm font-semibold">Top courses</h3>
            <div className="flex flex-col gap-2">
              {topCourses.map((course) => (
                <div key={course.id} className="flex min-w-0 items-center justify-between gap-3 rounded-lg border border-border bg-background px-3 py-2">
                  <div className="min-w-0">
                    <p className="truncate text-sm font-medium">{course.name}</p>
                    <p className="text-xs text-muted-foreground">{course.completedLessons}/{course.lessons} complete</p>
                  </div>
                  <div className="shrink-0 text-right text-xs tabular-nums text-muted-foreground">
                    <div>{formatStatDuration(course.watchedSeconds)}</div>
                    <div>{formatBytes(course.bytes)}</div>
                  </div>
                </div>
              ))}
            </div>
          </div>
        </div>
      </div>

      <div className="paper-panel rounded-2xl p-5">
        <div className="mb-4 flex items-center justify-between gap-3">
          <div className="min-w-0">
            <h3 className="text-sm font-semibold">Activity</h3>
            <p className="text-xs text-muted-foreground">{stats.activeDays} active days in 12 weeks</p>
          </div>
          <CalendarDays className="hidden size-4 shrink-0 text-muted-foreground sm:block" />
        </div>
        <ActivityHeatmap days={stats.activityDays} />
      </div>
    </section>
  )
}

function StatTile({
  icon: Icon,
  label,
  value,
  detail,
}: {
  icon: typeof BookOpen
  label: string
  value: string
  detail: string
}) {
  return (
    <div className="min-w-0 rounded-xl border border-border bg-background p-4">
      <div className="mb-3 flex items-center justify-between gap-3">
        <span className="text-xs font-medium uppercase text-muted-foreground">{label}</span>
        <Icon className="size-4 shrink-0 text-primary" />
      </div>
      <div className="truncate text-2xl font-semibold tabular-nums">{value}</div>
      <div className="mt-1 truncate text-xs text-muted-foreground">{detail}</div>
    </div>
  )
}

function MediaTypeRow({ item, totalBytes }: { item: LearningStats["mediaTypes"][number]; totalBytes: number }) {
  const percent = totalBytes > 0 ? Math.round((item.bytes / totalBytes) * 100) : 0
  return (
    <div className="grid gap-1.5">
      <div className="flex items-center justify-between gap-3 text-xs">
        <span className="capitalize text-muted-foreground">{item.type}</span>
        <span className="shrink-0 tabular-nums">{formatBytes(item.bytes)}</span>
      </div>
      <div className="h-2 overflow-hidden rounded-full bg-muted">
        <div className="h-full rounded-full bg-primary" style={{ width: `${percent}%` }} />
      </div>
    </div>
  )
}

function ActivityHeatmap({ days }: { days: ActivityDay[] }) {
  const cells = buildHeatmapCells(days, 84)
  const maxWatched = Math.max(1, ...cells.map((day) => day.watchedSeconds))

  return (
    <div className="grid grid-flow-col grid-rows-7 gap-1 overflow-x-auto pb-1">
      {cells.map((day) => {
        const level = day.watchedSeconds === 0
          ? 0
          : Math.max(1, Math.ceil((day.watchedSeconds / maxWatched) * 4))
        return (
          <div
            key={day.date}
            title={`${day.date}: ${formatStatDuration(day.watchedSeconds)}`}
            aria-label={`${day.date}: ${formatStatDuration(day.watchedSeconds)}`}
            className={cn(
              "size-3 rounded-[3px] border border-border",
              level === 0 && "bg-muted",
              level === 1 && "bg-accent",
              level === 2 && "bg-primary/35",
              level === 3 && "bg-primary/65",
              level === 4 && "bg-primary"
            )}
          />
        )
      })}
    </div>
  )
}

function buildHeatmapCells(days: ActivityDay[], count: number): ActivityDay[] {
  const byDate = new Map(days.map((day) => [day.date, day]))
  const end = new Date()
  end.setUTCHours(0, 0, 0, 0)

  return Array.from({ length: count }, (_, index) => {
    const date = new Date(end)
    date.setUTCDate(end.getUTCDate() - (count - index - 1))
    const key = date.toISOString().slice(0, 10)
    return byDate.get(key) ?? { date: key, watchedSeconds: 0, lessonsTouched: 0, completions: 0 }
  })
}

function formatBytes(bytes: number): string {
  if (bytes < 1024) return `${bytes} B`
  const units = ["KB", "MB", "GB", "TB"]
  let value = bytes / 1024
  let unit = units[0]
  for (let index = 1; index < units.length && value >= 1024; index++) {
    value /= 1024
    unit = units[index]
  }
  return `${value >= 10 ? value.toFixed(0) : value.toFixed(1)} ${unit}`
}

function formatStatDuration(seconds: number): string {
  const rounded = Math.max(0, Math.round(seconds))
  if (rounded < 60) return `${rounded}s`
  const minutes = Math.floor(rounded / 60)
  if (minutes < 60) return `${minutes}m`
  const hours = Math.floor(minutes / 60)
  const remainingMinutes = minutes % 60
  return remainingMinutes > 0 ? `${hours}h ${remainingMinutes}m` : `${hours}h`
}

function formatDisplayPath(path: string): string {
  const normalized = path.replace(/\\/g, "/")
  const unixHome = normalized.match(/^\/(home|Users)\/[^/]+(?:\/(.*))?$/)
  if (unixHome) return unixHome[2] ? `~/${unixHome[2]}` : "~"

  const windowsHome = path.match(/^[A-Za-z]:\\Users\\[^\\]+(?:\\(.*))?$/)
  if (windowsHome) return windowsHome[1] ? `~\\${windowsHome[1]}` : "~"

  return path
}

function ResumeCourseCard({
  course,
  summary,
  nextLesson,
  onOpenCourse,
}: {
  course: Course
  summary: CourseSummary
  nextLesson: Lesson | null
  onOpenCourse: (course: Course, lessonId?: string | null) => void
}) {
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

function DashboardCourseCard({
  course,
  summary,
  nextLesson,
  viewMode,
  onOpenCourse,
}: {
  course: Course
  summary: CourseSummary
  nextLesson: Lesson | null
  viewMode: ViewMode
  onOpenCourse: (course: Course, lessonId?: string | null) => void
}) {
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
        "paper-panel group cursor-pointer overflow-hidden rounded-xl transition-colors hover:border-primary/70",
        isList ? "grid gap-0 md:grid-cols-[240px_minmax(0,1fr)]" : "flex min-h-[22rem] flex-col",
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
