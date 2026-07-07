import test from "node:test"
import assert from "node:assert/strict"
import { readFileSync } from "node:fs"
import { join } from "node:path"

const repoRoot = process.cwd()

test("fast Arch install builds the production Tauri binary", () => {
  const script = readFileSync(join(repoRoot, "scripts/install-arch-fast.sh"), "utf8")

  assert.equal(script.includes("pnpm tauri build --no-bundle --ci"), true)
  assert.equal(script.includes("cargo build --manifest-path src-tauri/Cargo.toml --release"), false)
})

test("Arch desktop launcher starts the installed system binary directly", () => {
  const desktopEntry = readFileSync(join(repoRoot, "packaging/arch/io.github.whitehades.melearner.desktop"), "utf8")

  assert.equal(desktopEntry.includes("Exec=/usr/bin/melearner"), true)
  assert.equal(desktopEntry.includes("Exec=melearner"), false)
})

test("Arch package description is concise", () => {
  const pkgbuild = readFileSync(join(repoRoot, "packaging/arch/PKGBUILD"), "utf8")
  const srcinfo = readFileSync(join(repoRoot, "packaging/arch/.SRCINFO"), "utf8")

  assert.equal(pkgbuild.includes('pkgdesc="local course learner"'), true)
  assert.equal(srcinfo.includes("pkgdesc = local course learner"), true)
})

test("release workflow does not publish unsupported Windows native-player artifacts", () => {
  const workflow = readFileSync(join(repoRoot, ".github/workflows/release.yml"), "utf8")
  const packageJson = readFileSync(join(repoRoot, "package.json"), "utf8")

  assert.equal(workflow.includes("windows-latest"), false)
  assert.equal(workflow.includes("--bundles msi"), false)
  assert.equal(workflow.includes("*.msi"), false)
  assert.equal(packageJson.includes("tauri:build:windows"), false)
})

test("release workflow publishes both supported Linux assets", () => {
  const workflow = readFileSync(join(repoRoot, ".github/workflows/release.yml"), "utf8")

  assert.equal(workflow.includes("linux appimage"), true)
  assert.equal(workflow.includes("linux arch package"), true)
  assert.equal(workflow.includes("archlinux:base-devel"), true)
  assert.equal(workflow.includes("pnpm tauri build --no-bundle --ci"), true)
  assert.equal(workflow.includes("makepkg -f --noconfirm"), true)
  assert.equal(workflow.includes("*.pkg.tar.zst"), true)
})

test("ci workflow covers supported Linux native-player checks", () => {
  const workflow = readFileSync(join(repoRoot, ".github/workflows/ci.yml"), "utf8")

  assert.equal(workflow.includes("ubuntu-22.04"), true)
  assert.equal(workflow.includes("archlinux:base-devel"), true)
  assert.equal(workflow.includes("webkit2gtk-4.1"), true)
  assert.equal(workflow.includes(" mpv "), true)
  assert.equal(workflow.includes("cargo ffmpeg"), true)
  assert.equal(workflow.includes("ffmpeg"), true)
  assert.equal(workflow.includes("npm run type-check"), true)
  assert.equal(workflow.includes("npm run lint"), true)
  assert.equal(workflow.includes("node --test lib/*boundary.test.mjs lib/dashboard-selectors.test.mjs"), true)
  assert.equal(workflow.includes("node --test --experimental-transform-types"), true)
  assert.equal(workflow.includes("pnpm build"), true)
  assert.equal(workflow.includes("cargo test --manifest-path src-tauri/Cargo.toml"), true)
  assert.equal(workflow.includes("cargo check --manifest-path src-tauri/Cargo.toml"), true)
  assert.equal(workflow.includes("windows-latest"), false)
  assert.equal(workflow.includes("macos-latest"), false)
})

test("release Arch package job uses the runner uid without chowning the checkout", () => {
  const workflow = readFileSync(join(repoRoot, ".github/workflows/release.yml"), "utf8")

  assert.equal(workflow.includes("archlinux:base-devel"), true)
  assert.equal(workflow.includes('RUNNER_UID="$(id -u)"'), true)
  assert.equal(workflow.includes('RUNNER_GID="$(id -g)"'), true)
  assert.equal(workflow.includes("cargo ffmpeg"), true)
  assert.equal(workflow.includes(" chown -R builder:builder /workspace"), false)
})
