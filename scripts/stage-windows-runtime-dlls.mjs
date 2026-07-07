#!/usr/bin/env node
import { copyFileSync, existsSync } from "node:fs"
import { basename, dirname, join, resolve } from "node:path"
import { spawnSync } from "node:child_process"

function usage() {
  console.error(`usage: node scripts/stage-windows-runtime-dlls.mjs --app-bin <path> [options]

options:
  --app-bin <path>      built melearner.exe to stage beside
  --msys-root <path>    MSYS2 root, default C:\\msys64
  --dry-run            list DLLs without copying
`)
}

function parseArgs(argv) {
  const options = { msysRoot: "C:\\msys64", dryRun: false }
  for (let index = 0; index < argv.length; index += 1) {
    const value = argv[index]
    if (value === "--") continue
    if (value === "--dry-run") {
      options.dryRun = true
      continue
    }
    if (!value.startsWith("--")) {
      throw new Error(`unexpected argument: ${value}`)
    }

    const [rawKey, inlineValue] = value.slice(2).split("=", 2)
    const key = rawKey.replace(/-([a-z])/g, (_, ch) => ch.toUpperCase())
    const optionValue = inlineValue ?? argv[++index]
    if (!optionValue) throw new Error(`missing value for --${rawKey}`)
    options[key] = optionValue
  }
  if (!options.appBin && process.env.MELEARNER_APP_BIN) {
    options.appBin = process.env.MELEARNER_APP_BIN
  }
  return options
}

function resolveMsysPaths(msysRoot) {
  const root = resolve(msysRoot)
  const bash = join(root, "usr", "bin", "bash.exe")
  const ucrtBin = join(root, "ucrt64", "bin")
  if (!existsSync(bash)) throw new Error(`MSYS2 bash is missing: ${bash}`)
  if (!existsSync(ucrtBin)) throw new Error(`MSYS2 UCRT bin directory is missing: ${ucrtBin}`)
  return { bash, ucrtBin }
}

function runLdd({ appBin, bash, ucrtBin }) {
  const result = spawnSync(
    bash,
    ["-lc", 'PATH=/ucrt64/bin:$PATH ldd "$(cygpath -u "$APP_BIN")"'],
    {
      encoding: "utf8",
      env: { ...process.env, APP_BIN: appBin, PATH: `${ucrtBin};${process.env.PATH ?? ""}` },
    },
  )
  if (result.status !== 0) {
    throw new Error(`ldd failed:\n${result.stderr || result.stdout}`)
  }
  return result.stdout
}

function windowsPathToMsys(path) {
  const normalized = resolve(path).replace(/\\/g, "/")
  const drive = normalized.slice(0, 1).toLowerCase()
  return `/${drive}${normalized.slice(2)}`
}

function parseUcrtDlls(lddOutput, ucrtBin, appDir) {
  const dlls = new Map()
  const appDirMsys = windowsPathToMsys(appDir).toLowerCase()
  for (const line of lddOutput.split(/\r?\n/)) {
    const match = line.match(/=>\s+([^\s]+)\s+\(/)
    if (!match) continue
    const resolvedPath = match[1].toLowerCase()
    if (!resolvedPath.startsWith("/ucrt64/bin/") && !resolvedPath.startsWith(`${appDirMsys}/`)) {
      continue
    }
    const source = join(ucrtBin, basename(match[1]))
    if (existsSync(source)) dlls.set(basename(source).toLowerCase(), source)
  }
  return [...dlls.values()].sort((a, b) => basename(a).localeCompare(basename(b)))
}

function main() {
  const options = parseArgs(process.argv.slice(2))
  if (!options.appBin) {
    usage()
    process.exitCode = 1
    return
  }

  const appBin = resolve(options.appBin)
  if (!existsSync(appBin)) throw new Error(`app executable does not exist: ${appBin}`)

  const { bash, ucrtBin } = resolveMsysPaths(options.msysRoot)
  const appDir = dirname(appBin)
  const dlls = parseUcrtDlls(runLdd({ appBin, bash, ucrtBin }), ucrtBin, appDir)
  if (dlls.length === 0) throw new Error("ldd did not report any MSYS2 UCRT DLL dependencies")

  for (const source of dlls) {
    const destination = join(appDir, basename(source))
    if (!options.dryRun) copyFileSync(source, destination)
    console.log(`${options.dryRun ? "would copy" : "copied"} ${source} -> ${destination}`)
  }
  console.log(`staged ${dlls.length} MSYS2 UCRT DLLs beside ${appBin}`)
}

main()
