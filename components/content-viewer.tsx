"use client"

import { useState, useEffect } from "react"
import { readTextFile } from "@tauri-apps/plugin-fs"
import { convertFileSrc } from "@tauri-apps/api/core"
import ReactMarkdown from "react-markdown"
import remarkGfm from "remark-gfm"
import { ScrollArea } from "@/components/ui/scroll-area"
import { Button } from "@/components/ui/button"
import { isTauri } from "@/lib/tauri"
import type { Lesson } from "@/types"
import { SkipBack, SkipForward, FileText, File, FileCode, ExternalLink } from "lucide-react"

interface ContentViewerProps {
  lesson: Lesson
  onPrevious?: () => void
  onNext?: () => void
}

export function ContentViewer({ lesson, onPrevious, onNext }: ContentViewerProps) {
  const [content, setContent] = useState<string | null>(null)
  const [assetSrc, setAssetSrc] = useState<string | null>(null)
  const [loading, setLoading] = useState(true)
  const [error, setError] = useState<string | null>(null)

  const ext = lesson.path.toLowerCase().split(".").pop() || ""
  const isPdf = ext === "pdf"
  const isHtml = ext === "html" || ext === "htm"
  const isMarkdown = ext === "md" || ext === "markdown"
  const isTextFile = ["document", "subtitle"].includes(lesson.type) && !isPdf

  useEffect(() => {
    async function loadContent() {
      setLoading(true)
      setError(null)
      setContent(null)
      setAssetSrc(null)

      try {
        if (!isTauri()) {
          setError("file viewing requires desktop app")
          return
        }

        if (isPdf || isHtml) {
          const src = convertFileSrc(lesson.path)
          setAssetSrc(src)
        } else if (isTextFile || isMarkdown) {
          const text = await readTextFile(lesson.path)
          setContent(text)
        }
      } catch (err) {
        setError(`failed to load file: ${err}`)
      } finally {
        setLoading(false)
      }
    }

    loadContent()
  }, [lesson.path, isPdf, isHtml, isTextFile, isMarkdown, ext])

  const getIcon = () => {
    if (isPdf) return <FileText className="size-8 text-muted-foreground" />
    if (isHtml) return <FileCode className="size-8 text-muted-foreground" />
    if (isMarkdown) return <FileCode className="size-8 text-muted-foreground" />
    if (lesson.type === "document") return <FileText className="size-8 text-muted-foreground" />
    return <File className="size-8 text-muted-foreground" />
  }

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

  if (loading) {
    return (
      <div className="flex aspect-video w-full items-center justify-center bg-muted">
        <p className="text-muted-foreground">loading...</p>
      </div>
    )
  }

  if (error) {
    return (
      <div className="flex aspect-video w-full flex-col items-center justify-center gap-4 bg-muted">
        <p className="text-destructive">{error}</p>
        {navButtons}
      </div>
    )
  }

  if ((isPdf || isHtml) && assetSrc) {
    return (
      <div className="flex flex-col bg-background">
        <div className="flex flex-wrap items-center justify-between gap-3 border-b px-4 py-2">
          <div className="flex items-center gap-2">
            {getIcon()}
            <span className="font-medium">{lesson.name}</span>
          </div>
          <div className="flex items-center gap-2">
            <Button variant="ghost" size="sm" asChild>
              <a href={assetSrc} target="_blank" rel="noopener noreferrer">
                <ExternalLink className="mr-1 size-4" /> open externally
              </a>
            </Button>
            {navButtons}
          </div>
        </div>
        <iframe
          src={assetSrc}
          className="min-h-[50vh] w-full flex-1 border-0 bg-white"
          title={lesson.name}
          sandbox="allow-same-origin allow-scripts"
        />
      </div>
    )
  }

  if (isMarkdown && content) {
    return (
      <div className="flex flex-col bg-background">
        <div className="flex flex-wrap items-center justify-between gap-3 border-b px-4 py-2">
          <div className="flex items-center gap-2">
            {getIcon()}
            <span className="font-medium">{lesson.name}</span>
          </div>
          {navButtons}
        </div>
        <ScrollArea className="min-h-[40vh] flex-1">
          <div className="prose max-w-none p-4 dark:prose-invert">
            <ReactMarkdown remarkPlugins={[remarkGfm]}>{content}</ReactMarkdown>
          </div>
        </ScrollArea>
      </div>
    )
  }

  if (isTextFile && content) {
    return (
      <div className="flex flex-col bg-background">
        <div className="flex flex-wrap items-center justify-between gap-3 border-b px-4 py-2">
          <div className="flex items-center gap-2">
            {getIcon()}
            <span className="font-medium">{lesson.name}</span>
          </div>
          {navButtons}
        </div>
        <ScrollArea className="min-h-[40vh] flex-1">
          <pre className="whitespace-pre-wrap break-all p-4 font-mono text-sm">{content}</pre>
        </ScrollArea>
      </div>
    )
  }

  return (
    <div className="flex aspect-video w-full flex-col items-center justify-center gap-4 bg-muted">
      {getIcon()}
      <div className="text-center">
        <p className="text-lg font-medium">{lesson.name}</p>
        <p className="mt-1 text-sm text-muted-foreground">{lesson.type} file</p>
        <p className="mt-2 max-w-md truncate text-xs text-muted-foreground">{lesson.path}</p>
      </div>
      {navButtons}
    </div>
  )
}
