import type { SubtitleFile } from "@/types"

export type VideoSubtitleTrack = {
  id: string
  label: string
  language: string
  src: string
}

export function toVttContent(content: string, filePath: string): string {
  const normalized = content.replace(/\r\n/g, "\n").replace(/\r/g, "\n")

  if (filePath.toLowerCase().endsWith(".vtt")) {
    return normalized.startsWith("WEBVTT") ? normalized : `WEBVTT\n\n${normalized}`
  }

  const withoutCueNumbers = normalized.replace(/^\d+\s*$/gm, "")
  const withVttTimecodes = withoutCueNumbers.replace(
    /(\d{2}:\d{2}:\d{2}),(\d{3})/g,
    "$1.$2"
  )

  return `WEBVTT\n\n${withVttTimecodes.trim()}\n`
}

export function createSubtitleTrackId(subtitle: SubtitleFile, index: number): string {
  return `${subtitle.path}-${subtitle.language || "und"}-${index}`
}
