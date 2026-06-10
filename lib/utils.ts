import clsx from "clsx"
import { intervalToDuration, format } from "date-fns"
import { type ClassNameValue, twMerge } from "tailwind-merge"

export function cn(...classes: ClassNameValue[]) {
  return twMerge(clsx(classes))
}

export function formatDuration(totalSeconds: number) {
  if (!Number.isFinite(totalSeconds) || totalSeconds <= 0) {
    return "0:00"
  }

  const duration = intervalToDuration({
    start: 0,
    end: Math.floor(totalSeconds) * 1000,
  })
  const hours = duration.hours ?? 0
  const minutes = duration.minutes ?? 0
  const seconds = duration.seconds ?? 0

  if (hours > 0) {
    return `${hours}:${String(minutes).padStart(2, "0")}:${String(seconds).padStart(2, "0")}`
  }

  return `${minutes}:${String(seconds).padStart(2, "0")}`
}

export function formatTimestamp(value: Date | string) {
  const date = typeof value === "string" ? new Date(value) : value
  return format(date, "MMM d, yyyy p")
}

export function cleanSectionName(name: string): string {
  return name.replace(/^\d+[\s.-]*(?:section\s+\d+\s*)?/i, "").trim()
}
