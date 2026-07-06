# Development

## Run Locally

```bash
git clone https://github.com/WhiteHades/melearner
cd melearner
pnpm install
pnpm tauri:dev
```

`pnpm tauri:dev` starts the Next.js dev server and opens the Tauri desktop window.

## Verify

```bash
pnpm type-check
rtk lint
pnpm build
rtk cargo test --manifest-path src-tauri/Cargo.toml
rtk cargo check --manifest-path src-tauri/Cargo.toml
```

## Native Playback

The canonical player is an in-app native playback surface controlled by typed Tauri commands from the React UI. The engine is embedded libmpv. The app must not keep a parallel browser-media playback path, player-side compatibility conversion path, or external sidecar player path.

Current implementation state:

- `src-tauri/src/native_player.rs` owns libmpv lifecycle, local-file validation, playback commands, track selection, chapter data, and structured native state.
- `native_player_set_bounds` creates and moves the same-process native video surface used by libmpv.
- `components/video-player.tsx` owns the React/shadcn control band and surface measurement. It must not render `<video>` or `<audio>`.
- Rust refuses visible media loads until a native surface has been attached.
- Native-player tests cover internal audio/subtitle/chapter extraction and external SRT/VTT subtitle registration.
- Linux currently uses X11/XWayland for the native surface because the libmpv `wid` path needs an X11/XCB handle. A future Wayland-native path should use a verified libmpv render-API renderer.
- The native surface is hidden when its WebView placeholder leaves the viewport, then shown and moved again when the placeholder returns.

Rules for this pipeline:

- Keep React focused on layout, controls, and state display.
- Keep local-file validation and playback commands behind the native player module.
- Keep FFmpeg out of ordinary playback.
- Use FFmpeg only for queued thumbnails, metadata, or explicit future processing work.
- Remove stale player files, dependencies, docs, aliases, and generated artifacts in the same change that replaces them.
- After any launchable behavior change on this laptop, install the native Arch package so the desktop launcher runs the updated `/usr/bin/melearner` binary. Do not treat `~/.cargo/bin/melearner` as an installed app instance.
- The Arch package depends on `mpv` because Arch's `mpv` package provides `libmpv.so`; this is an embedded-library dependency, not permission to launch an external `mpv` process.

See `docs/adr/0010-embedded-libmpv-native-playback.md`.

## Scroll Performance

Course cards must not start FFmpeg or decode video while the user scrolls the library. Runtime video thumbnail extraction caused janky scrolling and competed with native playback work.

See `docs/adr/0007-course-cards-do-not-generate-runtime-video-thumbnails.md`.

## Durable Course Identity

Course identity is stored locally in SQLite. The current implementation adds:

- `courses.identity_id` as the stable identity value associated with a course row
- `courses.fingerprint` as a non-absolute content fingerprint for reconnecting moved or renamed courses
- `courses.missing_since` for courses that were absent during the latest scan
- `lessons.relative_path` for preserving lesson progress when a course folder moves
- `lesson_activity` as append-only local activity events for stats and heatmaps

Matching rules:

1. Match courses by exact path first.
2. If there is no exact path match, match by marker identity only when exactly one existing course has that identity.
3. If there is no marker match, match by fingerprint only when exactly one existing course has that fingerprint.
4. Create a new course when there is no safe match.
5. Never reuse progress for ambiguous course or lesson matches; return a scan warning instead.
6. Match lessons by exact path first, then by relative path within the resolved course, then by section/name/type/file-size metadata only when unambiguous.

Fingerprints exclude the absolute root path and the course folder name, so moving or renaming a course can preserve identity when its relative learning items are unchanged. Marker files are automatic local metadata for available course folders. Do not add a user-facing marker toggle unless a new ADR explicitly reverses this product decision.

Additional verification for identity, SQLite sync, and stats:

```bash
node --test --experimental-transform-types lib/course-identity.test.mjs lib/database-sqlite-fixture.test.mjs lib/stats.test.mjs
```

The command currently emits Node experimental-loader warnings; the tests should still pass.

## Linux Release Builds

Build the AppImage:

```bash
NO_STRIP=true pnpm tauri build --ci --bundles appimage
```

Build the native Arch package after a clean no-bundle release binary exists:

```bash
NO_STRIP=true pnpm tauri build --no-bundle --ci
makepkg -f -C
```

Run `makepkg` from `packaging/arch/`.

On the maintainer laptop, app-behavior changes must update every launcher-visible installed instance by installing the built Arch package. The launcher desktop entry must call `/usr/bin/melearner` directly instead of relying on `PATH`; do not use `cargo install` or `~/.cargo/bin/melearner` for this app's installed instance.

For routine local install checks, use:

```bash
pnpm install:arch:fast
```

This script builds the production Tauri binary with `pnpm tauri build --no-bundle --ci`, packages `melearner-bin`, and installs the package that owns `/usr/bin/melearner`. Do not replace that Tauri build step with plain `cargo build`; direct Cargo builds can produce a dev-mode binary that tries to load `http://localhost:3000` instead of bundled static assets. The script preserves ignored build caches such as `.next`, `out`, and `src-tauri/target` while the local build is still in progress and chooses the fastest available Rust cache mode. Final release or handoff cleanup still removes generated artifacts after verification.

When `sccache` is installed, the script sets `RUSTC_WRAPPER=sccache` and defaults `CARGO_INCREMENTAL=0` so release compilation can be cached across clean local builds. Without `sccache`, it defaults `CARGO_INCREMENTAL=1` to reuse Cargo's local target directory during iterative laptop builds.

Public Linux releases should publish only:

- AppImage for portable Linux use
- Native Arch package asset used by AUR and optional manual `pacman -U`

Do not upload `.deb` or `.rpm` unless those channels are intentionally restored and tested.

## Repo Hygiene

Remove stale generated artifacts, completed temporary plans, redundant docs, obsolete code, and unused settings as part of the same task that makes them unnecessary. Do not keep duplicate structures, old UI branches, or compatibility shims after the repo has a canonical replacement.

Before finishing build or release work, run a targeted artifact scan for `.next`, `out`, `dist`, `target`, package files, temp screenshots, logs, and staging directories. Keep required dependencies and release files only while they are still needed for verification, install, upload, or checksums.

See `docs/adr/0009-remove-stale-and-redundant-artifacts.md`.

## Windows MSI Builds

MSI builds require Windows and WiX:

```powershell
pnpm install
pnpm tauri build --target x86_64-pc-windows-msvc --bundles msi
```

The release workflow includes a Windows MSI job on `windows-latest`. It must still be validated on a clean Windows VM before a Windows release is advertised as supported.

Before publishing a Windows MSI, test on a clean Windows VM:

- Install and launch
- WebView2 availability for the app shell
- Library scan
- Native video/audio playback
- Thumbnail behavior when FFmpeg is missing or bundled
- Upgrade and uninstall

Windows media notes:

- WebView2 Runtime must be present or installed by the bundle mode.
- libmpv and its required runtime libraries must be bundled deliberately.
- FFmpeg is not part of ordinary playback. If thumbnail generation is supported without user-installed FFmpeg, bundle FFmpeg deliberately and handle licensing.

## Architecture Notes

- The database lives at `$HOME/.local/share/melearner/melearner.db`.
- Documents and thumbnails load through Tauri asset URLs, not a localhost media server.
- Playable lessons use the native player module.
- The frontend calls Tauri commands directly.
- Logs live under `~/.melearner/`.
- ADRs live in `docs/adr/`.
- Stats, learning activity, and course identity behavior live in `docs/stats-and-identity-plan.md`.
