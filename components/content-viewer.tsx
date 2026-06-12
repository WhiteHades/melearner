"use client"

import { invoke } from "@tauri-apps/api/core"
import { Button } from "@/components/ui/button"
import { frontendLog } from "@/lib/frontend-log"
import { useAsyncResource } from "@/lib/hooks/use-async-resource"
import { isTauri } from "@/lib/tauri"
import type { Lesson } from "@/types"
import { SkipBack, SkipForward, FileText, File, FileCode } from "lucide-react"

interface ContentViewerProps {
  lesson: Lesson
  onPrevious?: () => void
  onNext?: () => void
}

export function ContentViewer({ lesson, onPrevious, onNext }: ContentViewerProps) {
  const ext = lesson.path.toLowerCase().split(".").pop() || ""
  const isPdf = ext === "pdf"
  const isHtml = ext === "html" || ext === "htm"
  const isMarkdown = ext === "md" || ext === "markdown"
  const isDocument = lesson.type === "document"

  const getIcon = () => {
    if (isPdf || isDocument) return <FileText className="size-8 text-muted-foreground" />
    if (isHtml || isMarkdown) return <FileCode className="size-8 text-muted-foreground" />
    return <File className="size-8 text-muted-foreground" />
  }

  const resource = useAsyncResource<true>(
    async () => {
      if (!isTauri()) throw new Error("file viewing requires desktop app")
      await invoke("open_native", { path: lesson.path })
      return true
    },
    [lesson.path, lesson.type],
    {
      onSuccess: () => {
        frontendLog("info", "content.openNative", { path: lesson.path, type: lesson.type })
      },
      onError: (error) => {
        frontendLog("error", "content.openNative.failed", {
          path: lesson.path,
          error,
        })
      },
    },
  )

  const navButtons = (
    <div className="flex gap-2">
      {onPrevious && (
        <Button variant="outline" size="sm" onClick={onPrevious}>
          <SkipBack className="mr-1 size-4" /> previous
        </Button>
      )}
      {onNext && (
        <Button size="sm" onClick={onNext}>
          next <SkipForward className="ml-1 size-4" />
        </Button>
      )}
    </div>
  )

  if (resource.status === "loading") {
    return (
      <div className="flex aspect-video w-full items-center justify-center bg-muted">
        <p className="text-muted-foreground">opening in default app...</p>
      </div>
    )
  }

  if (resource.status === "success") {
    return (
      <div className="flex aspect-video w-full flex-col items-center justify-center gap-4 bg-muted">
        {getIcon()}
        <div className="text-center">
          <p className="text-lg font-medium">{lesson.name}</p>
          <p className="mt-1 text-sm text-muted-foreground">
            {"opened in your default app \u2014 return here when you are done"}
          </p>
          <p className="mt-2 max-w-md truncate text-xs text-muted-foreground">{lesson.path}</p>
        </div>
        {navButtons}
      </div>
    )
  }

  if (resource.status === "error") {
    return (
      <div className="flex aspect-video w-full flex-col items-center justify-center gap-4 bg-muted">
        <p className="text-destructive">{`failed to open file: ${resource.error}`}</p>
        {navButtons}
      </div>
    )
  }

  return null
}
