#!/usr/bin/env node
import { spawn } from "node:child_process"
import {
  createWriteStream,
  mkdirSync,
  readFileSync,
  readdirSync,
  writeFileSync,
} from "node:fs"
import { join, resolve } from "node:path"
import { tmpdir } from "node:os"

const DEFAULTS = {
  startupTimeoutMs: 30000,
  commandTimeoutMs: 10000,
  resizeTimeoutMs: 10000,
  shutdownTimeoutMs: 10000,
  diagnosticTimeoutMs: 5000,
  terminateGraceMs: 2000,
}

const MAX_PROTOCOL_BUFFER_BYTES = 1_000_000

function usage() {
  console.error(`usage: pnpm watchdog [options] -- <app command> [args...]

The app command must use the watchdog JSON-lines protocol on stdin/stdout:
  app -> {"watchdog":"ready"}
  watchdog -> {"watchdog":"command"}
  app -> {"watchdog":"command-complete"}
  watchdog -> {"watchdog":"resize","width":1024,"height":640}
  app -> {"watchdog":"resize-complete"}
  watchdog -> {"watchdog":"shutdown"} and the app exits with status 0

options:
  --artifact-dir <path>          diagnostic output directory (default: system temp)
  --startup-timeout-ms <ms>     ready deadline (default: 30000)
  --command-timeout-ms <ms>     command response deadline (default: 10000)
  --resize-timeout-ms <ms>      resize response deadline (default: 10000)
  --shutdown-timeout-ms <ms>    clean exit deadline (default: 10000)
  --diagnostic-timeout-ms <ms>  external diagnostic deadline (default: 5000)
  --terminate-grace-ms <ms>     grace before forced termination (default: 2000)
`)
}

function parseArgs(argv) {
  if (argv.includes("--help")) return { help: true, command: [], options: { ...DEFAULTS } }

  const separator = argv.indexOf("--")
  if (separator < 0) throw new Error("missing -- before the app command")

  const command = argv.slice(separator + 1)
  if (command.length === 0) throw new Error("missing app command")

  const options = { ...DEFAULTS }
  const optionArgs = argv.slice(0, separator)
  for (let index = 0; index < optionArgs.length; index += 1) {
    const arg = optionArgs[index]
    if (!arg.startsWith("--")) throw new Error(`unexpected argument: ${arg}`)

    const key = arg.slice(2).replace(/-([a-z])/g, (_, char) => char.toUpperCase())
    if (!(key in options) && key !== "artifactDir") throw new Error(`unknown option: ${arg}`)
    index += 1
    if (index >= optionArgs.length) throw new Error(`missing value for ${arg}`)
    options[key] = optionArgs[index]
  }

  for (const key of Object.keys(DEFAULTS)) {
    const value = Number(options[key])
    if (!Number.isInteger(value) || value <= 0) throw new Error(`invalid --${key.replace(/[A-Z]/g, (char) => `-${char.toLowerCase()}`)}: ${options[key]}`)
    options[key] = value
  }

  return { help: false, command, options }
}

function artifactDirectory(configured) {
  if (configured) return resolve(configured)
  const stamp = new Date().toISOString().replace(/[:.]/g, "-")
  return join(tmpdir(), `melearner-watchdog-${stamp}-${process.pid}`)
}

function writeJsonLine(stream, value) {
  stream.write(`${JSON.stringify(value)}\n`)
}

function createProtocolReader(stream, logStream) {
  const queued = new Map()
  const waiting = new Map()
  let buffer = ""

  function emit(event) {
    const waiter = waiting.get(event)
    if (waiter) {
      waiting.delete(event)
      waiter()
      return
    }
    const count = queued.get(event) ?? 0
    queued.set(event, count + 1)
  }

  function consume(line) {
    if (!line.trim()) return
    try {
      const event = JSON.parse(line)
      if (typeof event.watchdog === "string") emit(event.watchdog)
    } catch {
      // App logs may contain arbitrary non-protocol output.
    }
  }

  stream.on("data", (chunk) => {
    logStream.write(chunk)
    buffer += chunk.toString("utf8")
    while (buffer.includes("\n")) {
      const newline = buffer.indexOf("\n")
      consume(buffer.slice(0, newline))
      buffer = buffer.slice(newline + 1)
    }
    if (Buffer.byteLength(buffer) > MAX_PROTOCOL_BUFFER_BYTES) {
      buffer = buffer.slice(-MAX_PROTOCOL_BUFFER_BYTES)
    }
  })
  stream.on("end", () => consume(buffer))

  return {
    wait(event) {
      const count = queued.get(event) ?? 0
      if (count > 0) {
        queued.set(event, count - 1)
        return Promise.resolve()
      }
      return new Promise((resolveWait) => waiting.set(event, resolveWait))
    },
  }
}

function withTimeout(promise, timeoutMs, phase) {
  let timer
  return Promise.race([
    promise,
    new Promise((_, reject) => {
      timer = setTimeout(() => reject(new Error(`${phase} phase timed out after ${timeoutMs}ms`)), timeoutMs)
    }),
  ]).finally(() => clearTimeout(timer))
}

function completionFor(child) {
  return new Promise((resolveCompletion) => {
    let settled = false
    const settle = (result) => {
      if (settled) return
      settled = true
      resolveCompletion(result)
    }
    child.once("error", (error) => settle({ code: null, signal: null, error: error.message }))
    child.once("exit", (code, signal) => settle({ code, signal, error: null }))
  })
}

async function waitForPhase(protocol, event, completion, timeoutMs, phase) {
  return withTimeout(
    Promise.race([
      protocol.wait(event),
      completion.then((exit) => {
        throw new Error(`app exited during ${phase}: code=${exit.code ?? "null"} signal=${exit.signal ?? "null"}${exit.error ? ` error=${exit.error}` : ""}`)
      }),
    ]),
    timeoutMs,
    phase,
  )
}

function runDiagnostic(command, args, timeoutMs) {
  return new Promise((resolveDiagnostic) => {
    let stdout = ""
    let stderr = ""
    let settled = false
    const child = spawn(command, args, { stdio: ["ignore", "pipe", "pipe"], windowsHide: true })
    const finish = (result) => {
      if (settled) return
      settled = true
      clearTimeout(timer)
      resolveDiagnostic(`${result}\n${stdout}${stderr ? `\nstderr:\n${stderr}` : ""}`)
    }
    child.stdout.on("data", (chunk) => {
      if (stdout.length < 2_000_000) stdout += chunk.toString("utf8")
    })
    child.stderr.on("data", (chunk) => {
      if (stderr.length < 2_000_000) stderr += chunk.toString("utf8")
    })
    child.once("error", (error) => finish(`diagnostic command failed: ${error.message}`))
    child.once("exit", (code, signal) => finish(`diagnostic command exited: code=${code ?? "null"} signal=${signal ?? "null"}`))
    const timer = setTimeout(() => {
      child.kill("SIGKILL")
      finish(`diagnostic command timed out after ${timeoutMs}ms`)
    }, timeoutMs)
  })
}

function linuxStacks(pid) {
  const lines = []
  for (const file of ["status", "wchan"]) {
    try {
      lines.push(`== /proc/${pid}/${file} ==\n${readFileSync(`/proc/${pid}/${file}`, "utf8")}`)
    } catch (error) {
      lines.push(`== /proc/${pid}/${file} unavailable ==\n${error.message}`)
    }
  }

  let taskIds = []
  try {
    taskIds = readdirSync(`/proc/${pid}/task`).sort((a, b) => Number(a) - Number(b))
  } catch (error) {
    lines.push(`cannot enumerate tasks: ${error.message}`)
  }
  for (const taskId of taskIds) {
    lines.push(`== thread ${taskId} ==`)
    for (const file of ["comm", "wchan", "stack"]) {
      try {
        lines.push(`${file}:\n${readFileSync(`/proc/${pid}/task/${taskId}/${file}`, "utf8")}`)
      } catch (error) {
        lines.push(`${file}: unavailable (${error.message})`)
      }
    }
  }
  return lines.join("\n")
}

async function captureDiagnostics(pid, directory, timeoutMs) {
  let processTree
  let stacks
  if (!pid) {
    processTree = "app process did not start"
    stacks = "app process did not start"
  } else if (process.platform === "win32") {
    processTree = await runDiagnostic("tasklist", ["/V", "/FO", "CSV"], timeoutMs)
    stacks = await runDiagnostic(
      "powershell",
      ["-NoProfile", "-Command", `Get-Process -Id ${pid} | Format-List *`],
      timeoutMs,
    )
  } else {
    processTree = await runDiagnostic("ps", ["-eo", "pid=,ppid=,stat=,etime=,command="], timeoutMs)
    stacks = process.platform === "linux"
      ? linuxStacks(pid)
      : await runDiagnostic("sample", [String(pid), "1", "1"], timeoutMs)
  }

  writeFileSync(join(directory, "process-tree.txt"), processTree)
  writeFileSync(join(directory, "stacks.txt"), stacks)
}

function isRunning(child, exit) {
  return Boolean(child.pid) && exit === null
}

async function terminate(child, completion, graceMs, exit) {
  if (!isRunning(child, exit)) return exit

  if (process.platform === "win32") {
    await runDiagnostic("taskkill", ["/PID", String(child.pid), "/T"], graceMs)
  } else {
    try {
      process.kill(-child.pid, "SIGTERM")
    } catch {
      child.kill("SIGTERM")
    }
  }

  const graceful = await Promise.race([
    completion,
    new Promise((resolveGrace) => setTimeout(() => resolveGrace(null), graceMs)),
  ])
  if (graceful) return graceful

  if (process.platform === "win32") {
    await runDiagnostic("taskkill", ["/PID", String(child.pid), "/T", "/F"], graceMs)
  } else {
    try {
      process.kill(-child.pid, "SIGKILL")
    } catch {
      child.kill("SIGKILL")
    }
  }
  return withTimeout(completion, graceMs, "forced termination").catch(() => ({ code: null, signal: "SIGKILL", error: "process did not report exit" }))
}

function closeStream(stream) {
  return new Promise((resolveClose) => stream.end(resolveClose))
}

async function main() {
  const parsed = parseArgs(process.argv.slice(2))
  if (parsed.help) {
    usage()
    return
  }

  const { command, options } = parsed
  const directory = artifactDirectory(options.artifactDir)
  mkdirSync(directory, { recursive: true })
  const stdoutLog = createWriteStream(join(directory, "app.stdout.log"), { flags: "w" })
  const stderrLog = createWriteStream(join(directory, "app.stderr.log"), { flags: "w" })
  const startedAt = new Date()
  const report = {
    version: 1,
    status: "running",
    phase: "startup",
    command,
    pid: null,
    startedAt: startedAt.toISOString(),
    finishedAt: null,
    durationsMs: {},
    exit: null,
    error: null,
    artifacts: {
      stdout: "app.stdout.log",
      stderr: "app.stderr.log",
      processTree: "process-tree.txt",
      stacks: "stacks.txt",
    },
  }

  const child = spawn(command[0], command.slice(1), {
    cwd: process.cwd(),
    env: {
      ...process.env,
      MELEARNER_WATCHDOG: "1",
      MELEARNER_WATCHDOG_ARTIFACT_DIR: directory,
    },
    detached: process.platform !== "win32",
    stdio: ["pipe", "pipe", "pipe"],
    windowsHide: true,
  })
  report.pid = child.pid ?? null
  child.stdin.on("error", () => {})
  const protocol = createProtocolReader(child.stdout, stdoutLog)
  child.stderr.on("data", (chunk) => stderrLog.write(chunk))
  const completion = completionFor(child)
  let exit = null

  async function phase(name, work) {
    report.phase = name
    const phaseStarted = Date.now()
    await work()
    report.durationsMs[name] = Date.now() - phaseStarted
  }

  try {
    await phase("startup", () =>
      waitForPhase(protocol, "ready", completion, options.startupTimeoutMs, "startup"))
    await phase("command", async () => {
      writeJsonLine(child.stdin, { watchdog: "command" })
      await waitForPhase(protocol, "command-complete", completion, options.commandTimeoutMs, "command")
    })
    await phase("resize", async () => {
      writeJsonLine(child.stdin, { watchdog: "resize", width: 1024, height: 640 })
      await waitForPhase(protocol, "resize-complete", completion, options.resizeTimeoutMs, "resize")
    })
    await phase("shutdown", async () => {
      writeJsonLine(child.stdin, { watchdog: "shutdown" })
      exit = await withTimeout(completion, options.shutdownTimeoutMs, "shutdown")
      if (exit.error || exit.code !== 0) {
        throw new Error(`app did not shut down cleanly: code=${exit.code ?? "null"} signal=${exit.signal ?? "null"}${exit.error ? ` error=${exit.error}` : ""}`)
      }
    })
    report.status = "passed"
    report.phase = "complete"
  } catch (error) {
    report.status = "failed"
    report.error = error.message || String(error)
    try {
      await captureDiagnostics(child.pid, directory, options.diagnosticTimeoutMs)
    } catch (diagnosticError) {
      report.error += `; diagnostic capture failed: ${diagnosticError.message || String(diagnosticError)}`
    } finally {
      exit = await terminate(child, completion, options.terminateGraceMs, exit)
    }
  } finally {
    if (report.status === "passed") {
      writeFileSync(join(directory, "process-tree.txt"), "watchdog completed without a hang\n")
      writeFileSync(join(directory, "stacks.txt"), "watchdog completed without a hang\n")
    }
    report.exit = exit
    report.finishedAt = new Date().toISOString()
    await Promise.all([closeStream(stdoutLog), closeStream(stderrLog)])
    writeFileSync(join(directory, "watchdog-report.json"), `${JSON.stringify(report, null, 2)}\n`)
  }

  if (report.status === "failed") {
    throw new Error(`${report.error}; diagnostics: ${directory}`)
  }
  console.log(`responsiveness watchdog passed; diagnostics: ${directory}`)
}

main().catch((error) => {
  console.error(error.message || String(error))
  process.exitCode = 1
})
