import test from "node:test"
import assert from "node:assert/strict"
import { mkdtempSync, readFileSync, rmSync } from "node:fs"
import { join } from "node:path"
import { tmpdir } from "node:os"
import { spawnSync } from "node:child_process"

const repoRoot = process.cwd()
const watchdog = join(repoRoot, "scripts/responsiveness-watchdog.mjs")
const fixtureApp = join(repoRoot, "fixtures/parity/watchdog-app.mjs")

function processExists(pid) {
  try {
    process.kill(pid, 0)
    return true
  } catch {
    return false
  }
}

function runWatchdog(hangPhase = null) {
  const root = mkdtempSync(join(tmpdir(), "melearner-watchdog-test-"))
  const artifacts = join(root, "artifacts")
  const pidFile = join(root, "app.pid")
  const appArgs = [fixtureApp, "--pid-file", pidFile]
  if (hangPhase) appArgs.push("--hang-phase", hangPhase)

  const result = spawnSync(process.execPath, [
    watchdog,
    "--artifact-dir", artifacts,
    "--startup-timeout-ms", "200",
    "--command-timeout-ms", "200",
    "--resize-timeout-ms", "200",
    "--shutdown-timeout-ms", "200",
    "--diagnostic-timeout-ms", "200",
    "--terminate-grace-ms", "200",
    "--",
    process.execPath,
    ...appArgs,
  ], {
    cwd: repoRoot,
    encoding: "utf8",
    timeout: 5000,
  })

  const report = JSON.parse(readFileSync(join(artifacts, "watchdog-report.json"), "utf8"))
  return { root, artifacts, pidFile, result, report }
}

test("watchdog completes startup, command, resize, and shutdown without a GUI", (t) => {
  const run = runWatchdog()
  t.after(() => rmSync(run.root, { recursive: true, force: true }))

  assert.equal(run.result.status, 0, run.result.stderr)
  assert.equal(run.report.status, "passed")
  assert.equal(run.report.phase, "complete")
  assert.deepEqual(Object.keys(run.report.durationsMs), ["startup", "command", "resize", "shutdown"])
  assert.match(readFileSync(join(run.artifacts, "app.stdout.log"), "utf8"), /command-complete/)
  assert.match(readFileSync(join(run.artifacts, "app.stderr.log"), "utf8"), /received resize/)
})

for (const hangPhase of ["startup", "command", "resize", "shutdown"]) {
  test(`watchdog captures diagnostics and terminates a ${hangPhase} hang`, (t) => {
    const run = runWatchdog(hangPhase)
    t.after(() => rmSync(run.root, { recursive: true, force: true }))

    assert.equal(run.result.status, 1)
    assert.equal(run.report.status, "failed")
    assert.equal(run.report.phase, hangPhase)
    assert.match(run.report.error, new RegExp(`${hangPhase} phase timed out`))
    assert.match(readFileSync(join(run.artifacts, "process-tree.txt"), "utf8"), /watchdog-app\.mjs/)
    assert.ok(readFileSync(join(run.artifacts, "stacks.txt"), "utf8").length > 0)

    const pid = Number(readFileSync(run.pidFile, "utf8").trim())
    assert.equal(processExists(pid), false, `watchdog left ${hangPhase} fixture process ${pid} running`)
  })
}
