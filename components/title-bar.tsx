"use client"

import { useEffect, useState } from "react"
import { getCurrentWindow } from "@tauri-apps/api/window"
import { isTauri } from "@/lib/tauri"
import { X, Minus, Square, Copy } from "lucide-react"
import { Button } from "@/components/ui/button"

export function TitleBar() {
  const isTauriApp = isTauri()
  const [isMaximized, setIsMaximized] = useState(false)
  const [hovering, setHovering] = useState(false)

  useEffect(() => {
    if (isTauriApp) {
      getCurrentWindow().isMaximized().then(setIsMaximized)

      const unlisten = getCurrentWindow().listen("tauri://resize", async () => {
        setIsMaximized(await getCurrentWindow().isMaximized())
      })

      return () => {
        unlisten.then((listener) => listener())
      }
    }
  }, [isTauriApp])

  if (!isTauriApp) return null

  const handleMinimize = () => getCurrentWindow().minimize()
  const handleMaximize = async () => {
    await getCurrentWindow().toggleMaximize()
    setIsMaximized(!isMaximized)
  }
  const handleClose = () => getCurrentWindow().close()

  return (
    <div
      className="fixed inset-x-0 top-0 z-50 h-10"
      onMouseEnter={() => setHovering(true)}
      onMouseLeave={() => setHovering(false)}
    >
      <div data-tauri-drag-region className="h-4" />

      <div
        data-tauri-drag-region
        className={`flex justify-end px-3 transition-all duration-200 ${
          hovering
            ? "translate-y-0 opacity-100"
            : "pointer-events-none -translate-y-3 opacity-0"
        }`}
      >
        <div className="flex items-center gap-0.5 rounded-lg border bg-background/85 px-1 py-0.5 shadow-sm backdrop-blur-sm">
          <Button variant="ghost" size="icon-sm" onClick={handleMinimize} aria-label="minimize">
            <Minus className="size-3.5" />
          </Button>
          <Button
            variant="ghost"
            size="icon-sm"
            onClick={handleMaximize}
            aria-label={isMaximized ? "restore" : "maximize"}
          >
            {isMaximized ? (
              <Copy className="size-3 rotate-180" />
            ) : (
              <Square className="size-3" />
            )}
          </Button>
          <Button
            variant="ghost"
            size="icon-sm"
            onClick={handleClose}
            className="hover:bg-destructive hover:text-destructive-foreground"
            aria-label="close"
          >
            <X className="size-3.5" />
          </Button>
        </div>
      </div>
    </div>
  )
}
