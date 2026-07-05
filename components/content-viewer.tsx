"use client"

import { convertFileSrc } from "@tauri-apps/api/core"
import { readFile, readTextFile } from "@tauri-apps/plugin-fs"
import mammoth from "mammoth"
import ReactMarkdown from "react-markdown"
import remarkGfm from "remark-gfm"
import { Button } from "@/components/ui/button"
import { Alert, AlertDescription, AlertTitle } from "@/components/ui/alert"
import { frontendLog } from "@/lib/frontend-log"
import { useAsyncResource } from "@/lib/hooks/use-async-resource"
import { isTauri, openNativeFile } from "@/lib/tauri"
import type { Lesson } from "@/types"
import { FileText, File, FileCode, ExternalLink } from "lucide-react"

interface ContentViewerProps {
  lesson: Lesson
}

type ContentResource =
  | { kind: "plain"; text: string }
  | { kind: "markdown"; text: string }
  | { kind: "html"; src: string }
  | { kind: "pdf"; src: string }
  | { kind: "docx"; html: string; warnings: string[] }
  | { kind: "unsupported"; ext: string }

function getExtension(path: string): string {
  return path.toLowerCase().split("?")[0]?.split(".").pop() ?? ""
}

function assetUrl(path: string): string {
  return isTauri() ? convertFileSrc(path) : path
}

async function loadContent(lesson: Lesson): Promise<ContentResource> {
  if (!isTauri()) throw new Error("file viewing requires the desktop app")

  const ext = getExtension(lesson.path)

  if (ext === "txt") {
    return { kind: "plain", text: await readTextFile(lesson.path) }
  }

  if (ext === "md" || ext === "markdown") {
    return { kind: "markdown", text: await readTextFile(lesson.path) }
  }

  if (ext === "html" || ext === "htm") {
    return { kind: "html", src: assetUrl(lesson.path) }
  }

  if (ext === "pdf") {
    return { kind: "pdf", src: assetUrl(lesson.path) }
  }

  if (ext === "docx") {
    const bytes = await readFile(lesson.path)
    const arrayBuffer = bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength) as ArrayBuffer
    const result = await mammoth.convertToHtml(
      { arrayBuffer },
      {
        convertImage: mammoth.images.dataUri,
        styleMap: [
          "p[style-name='Title'] => h1:fresh",
          "p[style-name='Subtitle'] => p.subtitle:fresh",
        ],
      }
    )
    return {
      kind: "docx",
      html: result.value,
      warnings: result.messages.map((message) => message.message),
    }
  }

  return { kind: "unsupported", ext }
}

export function ContentViewer({ lesson }: ContentViewerProps) {
  const ext = getExtension(lesson.path)
  const resource = useAsyncResource<ContentResource>(
    () => loadContent(lesson),
    [lesson.path],
    {
      onSuccess: (data) => {
        frontendLog("info", "content.loaded", { path: lesson.path, type: lesson.type, kind: data.kind })
      },
      onError: (error) => {
        frontendLog("error", "content.load.failed", { path: lesson.path, error })
      },
    },
  )

  return (
    <div className="flex min-h-[68vh] w-full flex-col bg-card">
      <div className="border-b border-border px-4 py-3">
        <div className="min-w-0">
          <div className="flex items-center gap-2 text-xs font-medium uppercase tracking-wide text-muted-foreground">
            {ext === "pdf" ? <FileText className="size-4" /> : ext === "html" || ext === "htm" || ext === "md" || ext === "markdown" ? <FileCode className="size-4" /> : <File className="size-4" />}
            <span>{ext || "document"}</span>
          </div>
          <h2 className="truncate text-base font-semibold">{lesson.name}</h2>
        </div>
      </div>

      {resource.status === "loading" && (
        <div className="flex flex-1 items-center justify-center bg-muted/40 p-8 text-sm text-muted-foreground">
          Loading document...
        </div>
      )}

      {resource.status === "error" && (
        <div className="flex flex-1 flex-col items-center justify-center gap-4 bg-muted/40 p-8 text-center">
          <FileText className="size-10 text-muted-foreground" />
          <div className="max-w-xl">
            <h3 className="text-lg font-semibold">Could not render this file</h3>
            <p className="mt-2 text-sm text-muted-foreground">{resource.error}</p>
          </div>
          <Button type="button" variant="outline" onClick={() => void openNativeFile(lesson.path)}>
            <ExternalLink className="size-4" /> Open externally
          </Button>
        </div>
      )}

      {resource.status === "success" && <RenderedContent resource={resource.data} lesson={lesson} />}
    </div>
  )
}

function RenderedContent({ resource, lesson }: { resource: ContentResource; lesson: Lesson }) {
  if (resource.kind === "plain") {
    return (
      <ScrollDocument>
        <pre className="whitespace-pre-wrap break-words font-mono text-sm leading-7 text-foreground">{resource.text}</pre>
      </ScrollDocument>
    )
  }

  if (resource.kind === "markdown") {
    return (
      <ScrollDocument>
        <div className="document-prose">
          <ReactMarkdown remarkPlugins={[remarkGfm]}>{resource.text}</ReactMarkdown>
        </div>
      </ScrollDocument>
    )
  }

  if (resource.kind === "docx") {
    return (
      <ScrollDocument>
        {resource.warnings.length > 0 && (
          <Alert className="mb-4">
            <AlertTitle>Document conversion notes</AlertTitle>
            <AlertDescription>{resource.warnings.slice(0, 3).join(" | ")}</AlertDescription>
          </Alert>
        )}
        <iframe
          title={lesson.name}
          sandbox=""
          srcDoc={`<!doctype html><html><head><meta charset="utf-8"><style>${documentFrameCss}</style></head><body>${resource.html}</body></html>`}
          className="h-[66vh] w-full rounded-xl border border-border bg-card shadow-[var(--shadow-whisper)]"
        />
      </ScrollDocument>
    )
  }

  if (resource.kind === "html") {
    return (
      <div className="min-h-0 flex-1 bg-muted/30 p-3">
        <iframe title={lesson.name} sandbox="" src={resource.src} className="h-[70vh] w-full rounded-xl border border-border bg-card shadow-[var(--shadow-whisper)]" />
      </div>
    )
  }

  if (resource.kind === "pdf") {
    return (
      <div className="min-h-0 flex-1 bg-muted/30 p-3">
        <iframe title={lesson.name} src={resource.src} className="h-[70vh] w-full rounded-xl border border-border bg-card shadow-[var(--shadow-whisper)]" />
      </div>
    )
  }

  return (
    <div className="flex flex-1 flex-col items-center justify-center gap-4 bg-muted/40 p-8 text-center">
      <FileText className="size-10 text-muted-foreground" />
      <div className="max-w-xl">
        <h3 className="text-lg font-semibold">Preview is not available for this file type</h3>
        <p className="mt-2 text-sm text-muted-foreground">
          Learning items ending in .{resource.ext || "unknown"} can stay in the course outline, but this version can render txt, markdown, html, pdf, and docx directly.
        </p>
      </div>
      <Button type="button" variant="outline" onClick={() => void openNativeFile(lesson.path)}>
        <ExternalLink className="size-4" /> Open externally
      </Button>
    </div>
  )
}

function ScrollDocument({ children }: { children: React.ReactNode }) {
  return (
    <div className="min-h-0 flex-1 overflow-auto bg-muted/30 p-4 md:p-6">
      <div className="paper-panel mx-auto max-w-4xl rounded-2xl p-5 md:p-8">
        {children}
      </div>
    </div>
  )
}

const documentFrameCss = `
  :root { color-scheme: light dark; }
  body {
    margin: 0;
    padding: 32px;
    font: 15px/1.55 system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
    color: #141413;
    background: #faf9f5;
  }
  h1, h2, h3 { line-height: 1.25; margin: 1.5em 0 0.5em; }
  h1 { font-size: 28px; }
  h2 { font-size: 22px; }
  h3 { font-size: 18px; }
  p { margin: 0 0 1em; }
  table { width: 100%; border-collapse: collapse; margin: 1rem 0; }
  td, th { border: 1px solid #e3e0d3; padding: 8px; vertical-align: top; }
  img { max-width: 100%; height: auto; }
`
