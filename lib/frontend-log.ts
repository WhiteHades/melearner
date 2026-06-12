"use client"

import { isTauri } from "@/lib/tauri"

export function frontendLog(level: "info" | "warn" | "error" | "debug", message: string, context?: Record<string, unknown>) {
  if (!isTauri()) return
  const full = JSON.stringify({ level, message, context, ts: Date.now() })
  void import("@tauri-apps/api/core").then(({ invoke }) => {
    invoke("log_frontend", { message: full }).catch(() => {})
  })
}