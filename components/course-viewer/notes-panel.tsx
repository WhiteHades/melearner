"use client"

import { useCallback, useEffect, useMemo, useState } from "react"
import { z } from "zod"
import { zodResolver } from "@hookform/resolvers/zod"
import { useForm } from "react-hook-form"
import { NotebookPen } from "lucide-react"
import { Button } from "@/components/ui/button"
import { Card, CardContent, CardHeader, CardTitle } from "@/components/ui/card"
import { Label } from "@/components/ui/label"
import { Separator } from "@/components/ui/separator"
import { Textarea } from "@/components/ui/textarea"
import { formatDuration, formatTimestamp } from "@/lib/utils"
import { addNote, listNotes, removeNote } from "@/lib/operations"
import type { Lesson, Note } from "@/types"

const noteSchema = z.object({
  text: z.string().trim().min(1, "Note text is required.").max(2000),
})

type NoteFormValues = z.infer<typeof noteSchema>

interface NotesPanelProps {
  className?: string
  lesson?: Lesson | null
}

export function NotesPanel({ className, lesson }: NotesPanelProps) {
  const lessonId = lesson?.id ?? ""
  const [notes, setNotes] = useState<Note[]>([])
  const [isSaving, setIsSaving] = useState(false)
  const [isRemoving, setIsRemoving] = useState(false)
  const [loadError, setLoadError] = useState<string | null>(null)

  const reload = useCallback(async (id: string) => {
    try {
      const result = await listNotes(id)
      setNotes([...result].sort((a, b) => a.timestamp - b.timestamp))
      setLoadError(null)
    } catch (err) {
      setLoadError(err instanceof Error ? err.message : "Failed to load notes.")
    }
  }, [])

  useEffect(() => {
    if (!lessonId) {
      // Reset stale lesson-local data when the panel is detached from a lesson.
      // eslint-disable-next-line react-hooks/set-state-in-effect
      setNotes([])
      return
    }
    void reload(lessonId)
  }, [lessonId, reload])

  const form = useForm<NoteFormValues>({
    resolver: zodResolver(noteSchema),
    defaultValues: {
      text: "",
    },
  })

  useEffect(() => {
    form.reset({ text: "" })
  }, [form, lessonId])

  const handleSubmit = form.handleSubmit(async (values) => {
    if (!lesson) return

    setIsSaving(true)
    try {
      await addNote({
        lessonId: lesson.id,
        text: values.text,
        timestamp: lesson.lastPosition ?? 0,
      })
      form.reset()
      await reload(lesson.id)
    } finally {
      setIsSaving(false)
    }
  })

  const handleRemove = useCallback(
    async (noteId: string) => {
      if (!lessonId) return
      setIsRemoving(true)
      try {
        await removeNote(noteId)
        await reload(lessonId)
      } finally {
        setIsRemoving(false)
      }
    },
    [lessonId, reload]
  )

  const sortedNotes = useMemo(() => notes, [notes])

  return (
    <Card className={className}>
      <CardHeader className="gap-3">
        <div className="flex items-center gap-3">
          <div className="flex size-10 items-center justify-center rounded-full bg-secondary text-secondary-foreground">
            <NotebookPen className="size-4" />
          </div>
          <div className="space-y-1">
            <CardTitle className="text-xl font-semibold tracking-tight">Lesson notes</CardTitle>
            <p className="text-sm text-muted-foreground">
              Capture quick takeaways without leaving the lesson.
            </p>
          </div>
        </div>
      </CardHeader>

      <CardContent className="space-y-6">
        <form className="space-y-3" onSubmit={handleSubmit}>
          <div className="space-y-2">
            <Label htmlFor="session-notes" className="text-sm font-medium text-foreground">
              {lesson ? lesson.name : "Select a lesson to start taking notes"}
            </Label>
            <Textarea
              id="session-notes"
              placeholder="Write the part you want to remember. Notes save against the current lesson timestamp."
              className="min-h-28 resize-y rounded-2xl"
              {...form.register("text")}
              aria-invalid={Boolean(form.formState.errors.text)}
              disabled={!lesson || isSaving}
            />
            {form.formState.errors.text && (
              <p className="text-sm text-destructive">{form.formState.errors.text.message}</p>
            )}
          </div>

          <div className="flex flex-wrap items-center justify-between gap-3">
            <p className="text-sm text-muted-foreground">
              {lesson ? `Saved at ${formatDuration(lesson.lastPosition ?? 0)}` : "Notes unlock once a lesson is open."}
            </p>
            <Button type="submit" disabled={!lesson || isSaving}>
              {isSaving ? "Saving..." : "Save note"}
            </Button>
          </div>
        </form>

        <Separator />

        <div className="space-y-4">
          <div className="flex items-center justify-between gap-3">
            <h3 className="text-sm font-semibold text-foreground">Saved notes</h3>
            <span className="text-sm text-muted-foreground">{sortedNotes.length}</span>
          </div>

          {loadError && (
            <p className="text-sm text-destructive">{loadError}</p>
          )}

          {sortedNotes.length === 0 ? (
            <div className="rounded-2xl border border-dashed border-border/80 bg-muted/35 px-4 py-6 text-center">
              <p className="text-sm text-muted-foreground">
                {lesson ? "No notes yet for this lesson." : "Open a lesson to start saving notes."}
              </p>
            </div>
          ) : (
            <div className="space-y-3">
              {sortedNotes.map((note) => (
                <div key={note.id} className="rounded-2xl border border-border/70 bg-background/70 p-4">
                  <div className="flex flex-wrap items-center justify-between gap-3 text-xs text-muted-foreground tabular-nums">
                    <span className="font-mono">{formatDuration(note.timestamp)}</span>
                    <span>{formatTimestamp(note.createdAt)}</span>
                  </div>
                  <p className="mt-3 whitespace-pre-wrap text-sm leading-6 text-foreground">
                    {note.text}
                  </p>
                  <div className="mt-4 flex justify-end">
                    <Button
                      type="button"
                      variant="ghost"
                      size="sm"
                      onClick={() => void handleRemove(note.id)}
                      disabled={isRemoving}
                    >
                      Delete
                    </Button>
                  </div>
                </div>
              ))}
            </div>
          )}
        </div>
      </CardContent>
    </Card>
  )
}
