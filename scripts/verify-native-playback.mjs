#!/usr/bin/env node
import { spawn, spawnSync } from "node:child_process"
import { closeSync, existsSync, mkdirSync, openSync, readFileSync } from "node:fs"
import { dirname, join } from "node:path"
import os from "node:os"

const PLATFORM = {
  linux: {
    name: "linux",
    defaultAppBin: "/usr/bin/melearner",
    expectedBackend: "render-api:gtk-opengl",
    firstFrameLog: "native gtk render-api submitted first frame",
  },
  darwin: {
    name: "macos",
    defaultAppBin: "/Applications/melearner.app/Contents/MacOS/melearner",
    expectedBackend: "render-api:appkit-opengl",
    firstFrameLog: "native macos render-api submitted first frame",
  },
  win32: {
    name: "windows",
    defaultAppBin: join(process.env.ProgramFiles || "C:\\Program Files", "melearner", "melearner.exe"),
    expectedBackend: "render-api:wgl-opengl",
    firstFrameLog: "native windows render-api submitted first frame",
  },
}

function usage() {
  console.error(`usage: node scripts/verify-native-playback.mjs [options] [course-id] [lesson-id]

options:
  --app-bin <path>                 installed or built melearner executable
  --course-id <id>                 course id to open
  --lesson-id <id>                 playable lesson id to open
  --db-path <path>                 sqlite database used only to auto-pick a lesson
  --frontend-log <path>            frontend log to watch
  --surface-log <path>             native surface log to watch
  --expected-backend <label>       expected native surface backend
  --expected-first-frame-log <text> expected first-frame surface log text
  --expect-audio-tracks <count>    expected audio track count
  --expect-subtitle-tracks <count> expected subtitle track count
  --expect-chapters <count>        expected chapter count
  --timeout-ms <ms>                wait timeout, default 90000
`)
}

function parseArgs(argv) {
  const options = {}
  const positional = []
  for (let index = 0; index < argv.length; index += 1) {
    const value = argv[index]
    if (value === "--") continue
    if (!value.startsWith("--")) {
      positional.push(value)
      continue
    }

    const [rawKey, inlineValue] = value.slice(2).split("=", 2)
    const key = rawKey.replace(/-([a-z])/g, (_, ch) => ch.toUpperCase())
    if (key === "help") {
      options.help = true
      continue
    }
    if (inlineValue !== undefined) {
      options[key] = inlineValue
      continue
    }
    index += 1
    if (index >= argv.length) {
      throw new Error(`missing value for --${rawKey}`)
    }
    options[key] = argv[index]
  }
  if (!options.courseId && positional[0]) options.courseId = positional[0]
  if (!options.lessonId && positional[1]) options.lessonId = positional[1]
  return options
}

function commandExists(command) {
  const lookup = process.platform === "win32" ? "where" : "sh"
  const args = process.platform === "win32" ? [command] : ["-lc", `command -v ${command}`]
  return spawnSync(lookup, args, { stdio: "ignore" }).status === 0
}

function lineCount(path) {
  try {
    const content = readFileSync(path, "utf8")
    if (!content) return 0
    return content.split(/\r?\n/).length - (content.endsWith("\n") ? 1 : 0)
  } catch {
    return 0
  }
}

function touch(path) {
  mkdirSync(dirname(path), { recursive: true })
  closeSync(openSync(path, "a"))
}

function readSinceLine(path, offset) {
  if (!existsSync(path)) return ""
  const content = readFileSync(path, "utf8")
  if (!content) return ""
  const lines = content.split(/\r?\n/)
  if (content.endsWith("\n")) lines.pop()
  return lines.slice(offset).join("\n")
}

function parseFrontendEvents(logText) {
  return logText
    .split(/\r?\n/)
    .map((line) => {
      const jsonStart = line.indexOf("{")
      if (jsonStart < 0) return null
      try {
        return JSON.parse(line.slice(jsonStart))
      } catch {
        return null
      }
    })
    .filter(Boolean)
}

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms))
}

function defaultHome() {
  return process.env.HOME || process.env.USERPROFILE || os.homedir()
}

function defaultDbPath(home) {
  if (process.platform === "win32") {
    return join(process.env.LOCALAPPDATA || join(home, "AppData", "Local"), "melearner", "melearner.db")
  }
  if (process.platform === "darwin") {
    return join(home, "Library", "Application Support", "melearner", "melearner.db")
  }
  return join(home, ".local", "share", "melearner", "melearner.db")
}

function pickLessonFromDatabase(dbPath) {
  if (!existsSync(dbPath)) {
    throw new Error(`database is missing and no course/lesson ids were provided: ${dbPath}`)
  }
  if (!commandExists("sqlite3")) {
    throw new Error("sqlite3 is required when course/lesson ids are not provided")
  }

  const query =
    "select c.id, l.id from lessons l join courses c on c.id = l.course_id where l.type = 'video' order by c.name collate nocase, l.order_index limit 1;"
  const result = spawnSync("sqlite3", ["-separator", "\t", dbPath, query], { encoding: "utf8" })
  if (result.status !== 0) {
    throw new Error(`sqlite3 could not read playable lesson: ${result.stderr.trim()}`)
  }
  const [courseId, lessonId] = result.stdout.trim().split("\t")
  if (!courseId || !lessonId || courseId === lessonId) {
    throw new Error("could not resolve a playable course/lesson pair")
  }
  return { courseId, lessonId }
}

function listMediaToolProcesses() {
  if (process.platform === "win32") {
    const result = spawnSync("tasklist", ["/FO", "CSV", "/NH"], { encoding: "utf8" })
    if (result.status !== 0) return []
    return result.stdout
      .split(/\r?\n/)
      .filter((line) => /^"ff(?:mpeg|probe)\.exe"/i.test(line))
      .sort()
  }

  const result = spawnSync("ps", ["-axo", "pid=,command="], { encoding: "utf8" })
  if (result.status !== 0) return []
  return result.stdout
    .split(/\r?\n/)
    .map((line) => line.trim())
    .filter((line) => /(^|\s|\/)(ffmpeg|ffprobe)(\s|$)/i.test(line))
    .sort()
}

function newProcesses(before, after) {
  const previous = new Set(before)
  return after.filter((line) => !previous.has(line))
}

function listWindowsForProcess(pid) {
  if (process.platform !== "win32") return []

  const script = `
$code = @'
using System;
using System.Text;
using System.Runtime.InteropServices;

public static class MelearnerWindowList {
  public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

  [DllImport("user32.dll")]
  public static extern bool EnumWindows(EnumWindowsProc enumProc, IntPtr lParam);

  [DllImport("user32.dll")]
  public static extern bool IsWindowVisible(IntPtr hWnd);

  [DllImport("user32.dll")]
  public static extern int GetWindowTextLength(IntPtr hWnd);

  [DllImport("user32.dll", CharSet = CharSet.Unicode)]
  public static extern int GetWindowText(IntPtr hWnd, StringBuilder text, int maxCount);

  [DllImport("user32.dll")]
  public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);
}
'@
Add-Type $code
$targetPid = [uint32]${Number(pid)}
$titles = New-Object System.Collections.Generic.List[string]
[MelearnerWindowList]::EnumWindows({
  param([IntPtr]$hwnd, [IntPtr]$lparam)
  [uint32]$windowPid = 0
  [void][MelearnerWindowList]::GetWindowThreadProcessId($hwnd, [ref]$windowPid)
  if ($windowPid -eq $targetPid -and [MelearnerWindowList]::IsWindowVisible($hwnd)) {
    $length = [MelearnerWindowList]::GetWindowTextLength($hwnd)
    if ($length -gt 0) {
      $builder = New-Object System.Text.StringBuilder ($length + 1)
      [void][MelearnerWindowList]::GetWindowText($hwnd, $builder, $builder.Capacity)
      $titles.Add($builder.ToString())
    }
  }
  return $true
}, [IntPtr]::Zero) | Out-Null
$titles | ConvertTo-Json -Compress
`

  const result = spawnSync("powershell", ["-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", script], {
    encoding: "utf8",
  })
  if (result.status !== 0) {
    throw new Error(`could not inspect app windows: ${result.stderr.trim() || result.stdout.trim()}`)
  }
  const output = result.stdout.trim()
  if (!output) return []
  const parsed = JSON.parse(output)
  return Array.isArray(parsed) ? parsed : [parsed]
}

function validateAppWindows(pid) {
  const titles = listWindowsForProcess(pid)
  const separateVideoWindows = titles.filter((title) => /\bmelearner video\b/i.test(title))
  if (separateVideoWindows.length > 0) {
    throw new Error(`native playback opened a separate video window: ${separateVideoWindows.join(", ")}`)
  }
  const melearnerWindows = titles.filter((title) => /\bmelearner\b/i.test(title))
  if (process.platform === "win32" && melearnerWindows.length !== 1) {
    throw new Error(`expected exactly one visible melearner window, found ${melearnerWindows.length}: ${titles.join(", ") || "none"}`)
  }
  return titles
}

function requireNumber(value, label) {
  const parsed = Number(value)
  if (!Number.isInteger(parsed) || parsed < 0) {
    throw new Error(`${label} expectation must be a non-negative integer: ${value}`)
  }
  return parsed
}

function assertCount(context, field, expectedValue, label) {
  if (expectedValue === undefined || expectedValue === "") return
  const expected = requireNumber(expectedValue, label)
  const actual = Number(context[field])
  if (actual !== expected) {
    throw new Error(`native player ready line has unexpected ${label} count: expected ${expected}, got ${Number.isFinite(actual) ? actual : "missing"}`)
  }
}

function validateReadyContext(context, expectedBackend) {
  const required = {
    surfaceAttached: true,
    surfaceBackend: expectedBackend,
    surfaceRenderApi: true,
    surfaceRenderThreadAlive: true,
    surfaceRenderError: null,
  }

  for (const [field, expected] of Object.entries(required)) {
    if (context[field] !== expected) {
      throw new Error(`native player ready line has unexpected ${field}: expected ${JSON.stringify(expected)}, got ${JSON.stringify(context[field])}`)
    }
  }

  for (const field of ["surfaceRenderedFrames", "surfaceRenderWidth", "surfaceRenderHeight"]) {
    const value = Number(context[field])
    if (!Number.isFinite(value) || value < 1) {
      throw new Error(`native player ready line has invalid ${field}: ${JSON.stringify(context[field])}`)
    }
  }
}

async function waitForReady({ frontendLog, frontendStartLines, timeoutMs }) {
  const deadline = Date.now() + timeoutMs
  while (Date.now() < deadline) {
    const events = parseFrontendEvents(readSinceLine(frontendLog, frontendStartLines))
    const failure = events.find((event) =>
      ["native.player.load.failed", "native-player://error", "app.error", "app.unhandledRejection"].includes(event.message),
    )
    if (failure) {
      throw new Error(`native playback failed before ready: ${JSON.stringify(failure)}`)
    }

    const ready = events.filter((event) => event.message === "native.player.load.ready").at(-1)
    if (ready) return ready
    await sleep(1000)
  }

  throw new Error(`native player did not report ready within ${timeoutMs}ms`)
}

function stopApp(child) {
  if (!child || child.killed) return
  try {
    if (process.platform === "win32") {
      spawnSync("taskkill", ["/PID", String(child.pid), "/T", "/F"], { stdio: "ignore" })
    } else {
      child.kill("SIGTERM")
    }
  } catch {
    // Best effort cleanup for a verification helper.
  }
}

async function main() {
  const options = parseArgs(process.argv.slice(2))
  if (options.help) {
    usage()
    return
  }

  const platform = PLATFORM[process.platform]
  if (!platform) throw new Error(`unsupported verification platform: ${process.platform}`)

  const home = defaultHome()
  const appBin = options.appBin || process.env.MELEARNER_APP_BIN || platform.defaultAppBin
  const frontendLog = options.frontendLog || process.env.MELEARNER_FRONTEND_LOG || join(home, ".melearner", "frontend.log")
  const surfaceLog = options.surfaceLog || process.env.MELEARNER_NATIVE_SURFACE_LOG || join(home, ".melearner", "native-surface.log")
  const dbPath = options.dbPath || process.env.MELEARNER_DB_PATH || defaultDbPath(home)
  const expectedBackend = options.expectedBackend || platform.expectedBackend
  const firstFrameLog = options.expectedFirstFrameLog || platform.firstFrameLog
  const timeoutMs = Number(options.timeoutMs || process.env.MELEARNER_VERIFY_TIMEOUT_MS || 90000)

  if (!existsSync(appBin)) throw new Error(`app executable does not exist: ${appBin}`)
  if (!Number.isFinite(timeoutMs) || timeoutMs <= 0) throw new Error(`invalid timeout: ${timeoutMs}`)

  let courseId = options.courseId || process.env.MELEARNER_OPEN_COURSE_ID || ""
  let lessonId = options.lessonId || process.env.MELEARNER_OPEN_LESSON_ID || ""
  if (!courseId || !lessonId) {
    const picked = pickLessonFromDatabase(dbPath)
    courseId = picked.courseId
    lessonId = picked.lessonId
  }

  touch(frontendLog)
  touch(surfaceLog)
  const frontendStartLines = lineCount(frontendLog)
  const surfaceStartLines = lineCount(surfaceLog)
  const mediaBefore = listMediaToolProcesses()

  const child = spawn(appBin, [], {
    env: {
      ...process.env,
      HOME: home,
      MELEARNER_OPEN_COURSE_ID: courseId,
      MELEARNER_OPEN_LESSON_ID: lessonId,
      MELEARNER_DB_PATH: dbPath,
      MELEARNER_FRONTEND_LOG: frontendLog,
      MELEARNER_NATIVE_SURFACE_LOG: surfaceLog,
    },
    stdio: "ignore",
  })

  try {
    const childFailure = new Promise((_, reject) => {
      child.once("error", reject)
      child.once("exit", (code, signal) => {
        reject(new Error(`app exited before native playback was ready: code=${code ?? "null"} signal=${signal ?? "null"}`))
      })
    })
    const ready = await Promise.race([waitForReady({ frontendLog, frontendStartLines, timeoutMs }), childFailure])
    const context = ready.context || {}
    validateReadyContext(context, expectedBackend)
    const appWindowTitles = validateAppWindows(child.pid)
    assertCount(context, "audioTracks", options.expectAudioTracks || process.env.MELEARNER_EXPECT_AUDIO_TRACKS, "audio track")
    assertCount(context, "subtitleTracks", options.expectSubtitleTracks || process.env.MELEARNER_EXPECT_SUBTITLE_TRACKS, "subtitle track")
    assertCount(context, "chapters", options.expectChapters || process.env.MELEARNER_EXPECT_CHAPTERS, "chapter")

    const surfaceText = readSinceLine(surfaceLog, surfaceStartLines)
    if (!surfaceText.includes(firstFrameLog)) {
      throw new Error(`native surface log did not include first-frame marker: ${firstFrameLog}`)
    }

    const mediaAfter = listMediaToolProcesses()
    const startedMediaTools = newProcesses(mediaBefore, mediaAfter)
    if (startedMediaTools.length > 0) {
      throw new Error(`normal native playback started ffmpeg/ffprobe, which is not allowed:\n${startedMediaTools.join("\n")}`)
    }

    console.log(
      `native playback verified: platform=${platform.name} course=${courseId} lesson=${lessonId} backend=${context.surfaceBackend} frames=${context.surfaceRenderedFrames} surface=${context.surfaceRenderWidth}x${context.surfaceRenderHeight} ffmpeg=none windows=${appWindowTitles.join("|") || "none"}`,
    )
  } finally {
    stopApp(child)
  }
}

main().catch((error) => {
  console.error(error.message || String(error))
  process.exit(1)
})
