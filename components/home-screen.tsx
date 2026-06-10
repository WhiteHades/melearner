"use client"

import { useCallback, useEffect, useMemo } from "react"
import { parseAsString, useQueryState } from "nuqs"
import { Search } from "lucide-react"
import { CourseViewerLayout } from "@/components/course-viewer/layout"
import { CourseGrid } from "@/components/course-grid"
import { ThemeToggle } from "@/components/theme-toggle"
import { Input } from "@/components/ui/input"
import { Separator } from "@/components/ui/separator"
import { trpc } from "@/lib/trpc/client"
import type { Course } from "@/types"

type View = "library" | "viewer"

export function HomeScreen() {
  const [viewParam, setViewParam] = useQueryState(
    "view",
    parseAsString.withDefault("library")
  )
  const [courseId, setCourseId] = useQueryState("course", parseAsString)
  const [lessonId, setLessonId] = useQueryState("lesson", parseAsString)
  const [searchQuery, setSearchQuery] = useQueryState(
    "q",
    parseAsString.withDefault("")
  )

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
      <CourseViewerLayout
        course={selectedCourse}
        onBack={handleBack}
        selectedLessonId={lessonId}
        onLessonChange={setLessonId}
      />
    )
  }

  return (
    <main className="app-shell relative h-full bg-background text-foreground selection:bg-primary/15 selection:text-foreground">
      <div className="relative z-10 flex h-full flex-col">
        <header className="border-b border-border/70 bg-background/85 backdrop-blur-xl">
          <div className="mx-auto flex w-full max-w-6xl items-center justify-between gap-4 px-4 py-4 sm:px-6">
            <div className="flex min-w-0 items-center gap-3">
              <div className="flex size-10 shrink-0 items-center justify-center rounded-2xl bg-primary text-primary-foreground shadow-sm">
                <span className="text-sm font-bold tracking-tight">ml</span>
              </div>
              <div className="min-w-0">
                <p className="truncate text-sm font-semibold tracking-tight text-foreground">melearn</p>
                <p className="text-sm text-muted-foreground">Simple offline course library</p>
              </div>
            </div>

            <ThemeToggle />
          </div>
        </header>

        <div className="flex-1 overflow-y-auto">
          <section className="mx-auto flex w-full max-w-6xl flex-col gap-8 px-4 pb-10 pt-8 sm:px-6 sm:pt-10">
            <div className="overflow-hidden rounded-[32px] border border-border/70 bg-card/85 shadow-[0_32px_80px_-48px_rgba(15,23,42,0.45)]">
              <div className="flex flex-col gap-8 px-6 py-8 sm:px-8 sm:py-10 lg:flex-row lg:items-end">
                <div className="min-w-0 flex-1 space-y-4">
                  <p className="text-sm font-medium text-muted-foreground">
                    Offline-first learning, minus the clutter.
                  </p>
                  <h1 className="max-w-3xl text-3xl font-bold tracking-tight text-balance text-foreground sm:text-4xl lg:text-[2.75rem]">
                    Keep your local courses organized and resume the next lesson fast.
                  </h1>
                  <p className="max-w-2xl text-base leading-7 text-muted-foreground text-pretty">
                    Scan one folder, search by title or lesson, and jump back in without analytics dashboards,
                    extra chrome, or online accounts.
                  </p>
                </div>

                <div className="w-full shrink-0 rounded-[24px] border border-border/70 bg-background/80 p-4 shadow-[0_18px_40px_-32px_rgba(15,23,42,0.45)] lg:w-80">
                  <div className="space-y-1">
                    <p className="text-sm font-semibold text-foreground">Find a course</p>
                    <p className="text-sm text-muted-foreground">
                      Search the current library by course, lesson, or section name.
                    </p>
                  </div>
                  <div className="relative mt-4">
                    <label className="sr-only" htmlFor="library-search">
                      Search your course library
                    </label>
                    <Search className="absolute left-3 top-1/2 size-4 -translate-y-1/2 text-muted-foreground" />
                    <Input
                      id="library-search"
                      type="search"
                      placeholder="Search your library"
                      value={searchQuery}
                      onChange={(event) => setSearchQuery(event.target.value)}
                      className="h-11 rounded-2xl border-border/70 bg-card pl-10 shadow-none"
                    />
                  </div>
                </div>
              </div>

              <Separator className="bg-border/70" />

              <div className="flex flex-wrap items-center gap-3 px-6 py-4 text-sm text-muted-foreground sm:px-8">
                <span className="rounded-full bg-secondary px-3 py-1 font-medium text-secondary-foreground">
                  {courses.length} course{courses.length === 1 ? "" : "s"}
                </span>
                <span>Local folders only.</span>
                <span>Progress and notes stay on this machine.</span>
              </div>
            </div>

            <CourseGrid onCourseSelect={handleCourseSelect} searchQuery={searchQuery} />
          </section>
        </div>
      </div>
    </main>
  )
}
