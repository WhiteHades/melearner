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

  assert.equal(desktopEntry.includes("Name=melearner"), true)
  assert.equal(desktopEntry.includes("GenericName=local course learner"), true)
  assert.equal(desktopEntry.includes("Exec=/usr/bin/melearner"), true)
  assert.equal(desktopEntry.includes("Exec=melearner"), false)
  assert.equal(desktopEntry.includes("Keywords=melearner;local;course;learner;education;video;"), true)
})

test("Arch package description is concise", () => {
  const pkgbuild = readFileSync(join(repoRoot, "packaging/arch/PKGBUILD"), "utf8")
  const srcinfo = readFileSync(join(repoRoot, "packaging/arch/.SRCINFO"), "utf8")

  assert.equal(pkgbuild.includes('pkgdesc="local course learner"'), true)
  assert.equal(srcinfo.includes("pkgdesc = local course learner"), true)
})

test("release workflow does not publish unverified Windows native-player artifacts", () => {
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

test("release AppImage job builds in the Arch native-player environment", () => {
  const workflow = readFileSync(join(repoRoot, ".github/workflows/release.yml"), "utf8")

  assert.equal(workflow.includes("NO_STRIP=true pnpm tauri build --bundles appimage"), true)
  assert.equal(workflow.includes("archlinux:base-devel"), true)
  assert.equal(workflow.includes("mpv webkit2gtk-4.1"), true)
  assert.equal(workflow.includes("libglvnd"), true)
  assert.equal(workflow.includes("libwebkit2gtk-4.1-dev"), false)
  assert.equal(workflow.includes("sudo apt-get install -y"), false)
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
  assert.equal(workflow.includes("arch rust cache"), true)
  assert.equal(workflow.includes(".cache/arch-cargo"), true)
  assert.equal(workflow.includes("src-tauri/target"), true)
  assert.equal(workflow.includes("-e CARGO_HOME=/cargo-home"), true)
  assert.equal(workflow.includes("cargo test --manifest-path src-tauri/Cargo.toml"), true)
  assert.equal(workflow.includes("cargo check --manifest-path src-tauri/Cargo.toml"), true)
  assert.equal(workflow.includes("macos-latest"), false)
})

test("ci workflow checks macos compilation without publishing macos releases", () => {
  const workflow = readFileSync(join(repoRoot, ".github/workflows/ci.yml"), "utf8")
  const releaseWorkflow = readFileSync(join(repoRoot, ".github/workflows/release.yml"), "utf8")

  assert.equal(workflow.includes("macos compile readiness"), true)
  assert.equal(workflow.includes("macos-14"), true)
  assert.equal(workflow.includes("brew install mpv"), true)
  assert.equal(workflow.includes("pnpm build"), true)
  assert.equal(workflow.includes("cargo check --manifest-path src-tauri/Cargo.toml"), true)
  assert.equal(releaseWorkflow.includes("macos-"), false)
  assert.equal(releaseWorkflow.includes("--bundles dmg"), false)
})

test("ci workflow checks windows compilation without publishing windows releases", () => {
  const workflow = readFileSync(join(repoRoot, ".github/workflows/ci.yml"), "utf8")
  const releaseWorkflow = readFileSync(join(repoRoot, ".github/workflows/release.yml"), "utf8")

  assert.equal(workflow.includes("windows compile readiness"), true)
  assert.equal(workflow.includes("windows-latest"), true)
  assert.equal(workflow.includes("msys2/setup-msys2@v2"), true)
  assert.equal(workflow.includes("msystem: UCRT64"), true)
  assert.equal(workflow.includes("mingw-w64-ucrt-x86_64-gcc"), true)
  assert.equal(workflow.includes("mingw-w64-ucrt-x86_64-mpv"), true)
  assert.equal(workflow.includes("mingw-w64-ucrt-x86_64-pkgconf"), true)
  assert.equal(workflow.includes("x86_64-pc-windows-gnu"), true)
  assert.equal(workflow.includes("CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER"), true)
  assert.equal(workflow.includes("cargo check --manifest-path src-tauri/Cargo.toml --target x86_64-pc-windows-gnu"), true)
  assert.equal(releaseWorkflow.includes("windows-latest"), false)
  assert.equal(releaseWorkflow.includes("--bundles msi"), false)
  assert.equal(releaseWorkflow.includes("*.msi"), false)
})

test("windows no-bundle verification stages MSYS2 runtime DLLs explicitly", () => {
  const packageJson = readFileSync(join(repoRoot, "package.json"), "utf8")
  const developmentDoc = readFileSync(join(repoRoot, "docs/development.md"), "utf8")
  const installDoc = readFileSync(join(repoRoot, "docs/install.md"), "utf8")
  const stageScript = readFileSync(join(repoRoot, "scripts/stage-windows-runtime-dlls.mjs"), "utf8")

  assert.equal(packageJson.includes('"stage:windows-runtime": "node scripts/stage-windows-runtime-dlls.mjs"'), true)
  assert.equal(developmentDoc.includes("pnpm stage:windows-runtime -- --app-bin"), true)
  assert.equal(installDoc.includes("pnpm stage:windows-runtime -- --app-bin"), true)
  assert.equal(stageScript.includes("ldd"), true)
  assert.equal(stageScript.includes("/ucrt64/bin"), true)
  assert.equal(stageScript.includes("copyFileSync"), true)
  assert.equal(stageScript.includes("ffmpeg.exe"), false)
  assert.equal(stageScript.includes("ffprobe.exe"), false)
})

test("windows msi workflow uploads a signed test artifact without publishing a release", () => {
  const workflow = readFileSync(join(repoRoot, ".github/workflows/windows-msi.yml"), "utf8")
  const releaseWorkflow = readFileSync(join(repoRoot, ".github/workflows/release.yml"), "utf8")
  const cargoToml = readFileSync(join(repoRoot, "src-tauri/Cargo.toml"), "utf8")
  const installDoc = readFileSync(join(repoRoot, "docs/install.md"), "utf8")
  const signingScript = readFileSync(join(repoRoot, "scripts/sign-windows-msi-test.ps1"), "utf8")

  assert.equal(workflow.includes("workflow_dispatch"), true)
  assert.equal(workflow.includes("windows msi downloader"), true)
  assert.equal(workflow.includes("windows-latest"), true)
  assert.equal(workflow.includes("RUSTUP_TOOLCHAIN=stable-x86_64-pc-windows-gnu"), true)
  assert.equal(workflow.includes("MSYS2_ROOT"), true)
  assert.equal(workflow.includes("UCRT64_BIN"), true)
  assert.equal(workflow.includes("CARGO_PROFILE_RELEASE_OPT_LEVEL=1"), true)
  assert.equal(workflow.includes("pnpm tauri build --no-bundle --ci --target x86_64-pc-windows-gnu"), true)
  assert.equal(workflow.includes('pnpm stage:windows-runtime -- --msys-root "$env:MSYS2_ROOT" --app-bin'), true)
  assert.equal(workflow.includes("pnpm tauri bundle --bundles msi --ci --target x86_64-pc-windows-gnu"), true)
  assert.equal(workflow.includes("libmpv-2.dll"), true)
  assert.equal(workflow.includes("avcodec-62.dll"), true)
  assert.equal(workflow.includes("create test signing certificate"), true)
  assert.equal(workflow.includes("shell: bash"), true)
  assert.equal(workflow.includes("openssl req -x509 -newkey rsa:2048"), true)
  assert.equal(workflow.includes("openssl pkcs12 -export"), true)
  assert.equal(workflow.includes('workspace="$(cygpath -u "$GITHUB_WORKSPACE")"'), true)
  assert.equal(workflow.includes("WINDOWS_TEST_SIGNING_PFX"), true)
  assert.equal(workflow.includes("sign msi with test certificate"), true)
  assert.equal(workflow.includes("timeout-minutes: 3"), true)
  assert.equal(workflow.includes("scripts\\sign-windows-msi-test.ps1"), true)
  assert.equal(workflow.includes("melearner-windows-signed-msi-${{ github.sha }}"), true)
  assert.equal(workflow.includes("melearner-windows-msi-test-signing.cer"), true)
  assert.equal(workflow.includes("actions/upload-artifact@v4"), true)
  assert.equal(workflow.includes("gh release upload"), false)
  assert.equal(workflow.includes("*.msi"), true)
  assert.equal(releaseWorkflow.includes("gh release upload") && releaseWorkflow.includes("*.msi"), false)
  assert.equal(cargoToml.includes('crate-type = ["rlib"]'), true)
  assert.equal(cargoToml.includes("cdylib"), false)
  assert.equal(signingScript.includes("signtool.exe"), true)
  assert.equal(signingScript.includes('@("sign", "/f", $PfxPath, "/p", $PfxPassword'), true)
  assert.equal(signingScript.includes('@("verify", "/pa", "/v", $Path)'), true)
  assert.equal(signingScript.includes("Write-Host $Description"), true)
  assert.equal(signingScript.includes("Write-Host $_"), true)
  assert.equal(signingScript.includes("No signature found"), true)
  assert.equal(signingScript.includes("self-signed test certificate is expected to be untrusted"), true)
  assert.equal(signingScript.includes("$global:LASTEXITCODE = 0"), true)
  assert.equal(installDoc.includes("windows msi downloader"), true)
  assert.equal(installDoc.includes("self-signed test certificate"), true)
  assert.equal(installDoc.includes("melearner-windows-signed-msi-<commit-sha>"), true)
  assert.equal(installDoc.includes("gh run download"), true)
})

test("release Arch package job uses the runner uid without chowning the checkout", () => {
  const workflow = readFileSync(join(repoRoot, ".github/workflows/release.yml"), "utf8")

  assert.equal(workflow.includes("archlinux:base-devel"), true)
  assert.equal(workflow.includes('RUNNER_UID="$(id -u)"'), true)
  assert.equal(workflow.includes('RUNNER_GID="$(id -g)"'), true)
  assert.equal(workflow.includes("cargo ffmpeg"), true)
  assert.equal(workflow.includes(" chown -R builder:builder /workspace"), false)
})
