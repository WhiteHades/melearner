#!/usr/bin/env node
import { writeFileSync } from "node:fs"
import readline from "node:readline"

const args = process.argv.slice(2)
const option = (name) => {
  const index = args.indexOf(name)
  return index >= 0 ? args[index + 1] : null
}
const hangPhase = option("--hang-phase")
const pidFile = option("--pid-file")

if (pidFile) writeFileSync(pidFile, `${process.pid}\n`)
console.error(`watchdog fixture started pid=${process.pid} hang=${hangPhase ?? "none"}`)

if (hangPhase !== "startup") {
  console.log(JSON.stringify({ watchdog: "ready" }))
}

const keepAlive = setInterval(() => {}, 1000)
const input = readline.createInterface({ input: process.stdin })
input.on("line", (line) => {
  const message = JSON.parse(line)
  console.error(`watchdog fixture received ${message.watchdog}`)
  if (message.watchdog === hangPhase) return

  if (message.watchdog === "command") {
    console.log(JSON.stringify({ watchdog: "command-complete" }))
  } else if (message.watchdog === "resize") {
    console.log(JSON.stringify({ watchdog: "resize-complete" }))
  } else if (message.watchdog === "shutdown") {
    clearInterval(keepAlive)
    input.close()
  }
})
input.on("close", () => {
  if (hangPhase !== "shutdown") process.exit(0)
})
