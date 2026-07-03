"use client"

import { useEffect, useState, useMemo } from "react"
import { getCurrentWindow } from "@tauri-apps/api/window"
import { isTauri } from "@/lib/tauri"
import { X, Minus, Copy, Square } from "lucide-react"

type Platform = "macos" | "windows" | "linux"

function detectPlatform(): Platform {
  if (typeof navigator === "undefined") return "linux"
  const ua = navigator.userAgent
  if (/Mac/i.test(ua)) return "macos"
  if (/Win/i.test(ua)) return "windows"
  return "linux"
}

function MacButtons({
  onMinimize,
  onMaximize,
  onClose,
  isMaximized,
}: {
  onMinimize: () => void
  onMaximize: () => void
  onClose: () => void
  isMaximized: boolean
}) {
  const [hovered, setHovered] = useState<string | null>(null)

  return (
    <div className="flex items-center gap-1.5 px-2.5 py-2">
      <button
        onClick={onClose}
        onMouseEnter={() => setHovered("close")}
        onMouseLeave={() => setHovered(null)}
        className="relative flex size-3 items-center justify-center rounded-full bg-[#ff5f57] transition-opacity"
        aria-label="close"
      >
        {hovered === "close" && <X className="size-2 text-[#4d0000]" />}
      </button>
      <button
        onClick={onMinimize}
        onMouseEnter={() => setHovered("minimize")}
        onMouseLeave={() => setHovered(null)}
        className="relative flex size-3 items-center justify-center rounded-full bg-[#febc2e] transition-opacity"
        aria-label="minimize"
      >
        {hovered === "minimize" && <Minus className="size-2 text-[#593c00]" />}
      </button>
      <button
        onClick={onMaximize}
        onMouseEnter={() => setHovered("maximize")}
        onMouseLeave={() => setHovered(null)}
        className="relative flex size-3 items-center justify-center rounded-full bg-[#28c840] transition-opacity"
        aria-label={isMaximized ? "restore" : "maximize"}
      >
        {hovered === "maximize" && (
          isMaximized ? <Copy className="size-2 text-[#003d00]" /> : <Square className="size-2 text-[#003d00]" />
        )}
      </button>
    </div>
  )
}

function WinLinuxButtons({
  onMinimize,
  onMaximize,
  onClose,
  isMaximized,
}: {
  onMinimize: () => void
  onMaximize: () => void
  onClose: () => void
  isMaximized: boolean
}) {
  return (
    <div className="flex items-center">
      <button
        onClick={onMinimize}
        className="flex size-9 items-center justify-center rounded-lg text-muted-foreground/60 transition-colors hover:bg-muted hover:text-foreground"
        aria-label="minimize"
      >
        <Minus className="size-3.5" />
      </button>
      <button
        onClick={onMaximize}
        className="flex size-9 items-center justify-center rounded-lg text-muted-foreground/60 transition-colors hover:bg-muted hover:text-foreground"
        aria-label={isMaximized ? "restore" : "maximize"}
      >
        {isMaximized ? <Copy className="size-3 rotate-180" /> : <Square className="size-3" />}
      </button>
      <button
        onClick={onClose}
        className="flex size-9 items-center justify-center rounded-lg text-muted-foreground/60 transition-colors hover:bg-destructive hover:text-destructive-foreground"
        aria-label="close"
      >
        <X className="size-3.5" />
      </button>
    </div>
  )
}

export function TitleBar() {
  const [mounted, setMounted] = useState(false)
  const [isMaximized, setIsMaximized] = useState(false)
  const [hovering, setHovering] = useState(false)

  const isTauriApp = useMemo(() => isTauri(), [])
  const platform = useMemo(() => detectPlatform(), [])

  useEffect(() => { setMounted(true) }, []) // eslint-disable-line react-hooks/set-state-in-effect

  useEffect(() => {
    if (!isTauriApp) return

    getCurrentWindow().isMaximized().then(setIsMaximized)

    const unlisten = getCurrentWindow().listen("tauri://resize", async () => {
      setIsMaximized(await getCurrentWindow().isMaximized())
    })

    return () => {
      unlisten.then((listener) => listener())
    }
  }, [isTauriApp])

  if (!isTauriApp || !mounted) return null

  const handleMinimize = () => getCurrentWindow().minimize()
  const handleMaximize = async () => {
    await getCurrentWindow().toggleMaximize()
    setIsMaximized(!isMaximized)
  }
  const handleClose = () => getCurrentWindow().close()

  return (
    <div
      className="fixed right-2 top-2 z-50"
      onMouseEnter={() => setHovering(true)}
      onMouseLeave={() => setHovering(false)}
    >
      <div
        className={`rounded-xl border bg-background shadow-[var(--shadow-whisper)] transition-opacity duration-150 ${
          hovering
            ? "translate-y-0 opacity-100 pointer-events-auto"
            : "translate-y-0 opacity-0 pointer-events-none"
        }`}
      >
        {platform === "macos" ? (
          <MacButtons
            onMinimize={handleMinimize}
            onMaximize={handleMaximize}
            onClose={handleClose}
            isMaximized={isMaximized}
          />
        ) : (
          <WinLinuxButtons
            onMinimize={handleMinimize}
            onMaximize={handleMaximize}
            onClose={handleClose}
            isMaximized={isMaximized}
          />
        )}
      </div>
    </div>
  )
}
