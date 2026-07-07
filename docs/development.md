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
- `src-tauri/src/native_player/surface.rs` owns the native video surface boundary. On Linux, the default and only shipped backend is the same-window GTK `GLArea`/libmpv render path in `src-tauri/src/native_player/surface/linux_gtk.rs`, which reports `render-api:gtk-opengl` with `surfaceRenderApi=true` after attachment. Native state also reports `surfaceRenderThreadAlive`, `surfaceRenderedFrames`, `surfaceRenderWidth`, `surfaceRenderHeight`, `surfaceRenderUpdateFlags`, and `surfaceRenderError` so runtime verification can distinguish an attached surface from a surface that is actively rendering frames at non-zero dimensions. `MELEARNER_SURFACE_BACKEND=window-handle` is invalid on Linux because normal playback must never open a separate `melearner video` window.
- `native_player_set_bounds` creates and moves the same-process native video surface used by libmpv through the surface backend.
- `components/video-player.tsx` owns the React/shadcn control band and surface measurement. It must not render `<video>` or `<audio>`.
- Rust refuses visible media loads until a native surface has been attached.
- File-loaded, end-file, and libmpv playback errors come from a dedicated libmpv event client, not from optimistic load responses or position-duration guessing.
- Native-player tests cover internal audio/subtitle/chapter extraction and external SRT/VTT subtitle registration.
- Linux uses a GTK overlay inside the existing Tauri/WebKit window. The WebView remains the UI layer, while libmpv renders into a GTK `GLArea` positioned over the measured video placeholder. If this path cannot attach, playback must fail clearly instead of opening a second compositor-visible app window.
- The native surface is hidden when its WebView placeholder leaves the viewport, then shown and moved again when the placeholder returns.
- Packaged native-surface attach and render failures are written to `~/.melearner/native-surface.log` by default. Set `MELEARNER_NATIVE_SURFACE_LOG=/path/to/log` when running focused render diagnostics.
- Packaged render verification can open a known lesson at launch with `--open-course <course-id> --open-lesson <lesson-id>`, or with `MELEARNER_OPEN_COURSE_ID` and `MELEARNER_OPEN_LESSON_ID`. The packaged app must keep the static Tauri `main` window from `tauri.conf.json`; the frontend hydrates the library first, then reads the startup route with a short timeout and applies the viewer route asynchronously. Startup routing must never block library hydration.
- Packaged scan/sync diagnostics can explicitly run a startup scan with `--auto-scan <library-path>` or `MELEARNER_AUTO_SCAN_PATH=<library-path>`. This hook exists for verification and repair; normal app startup must not rescan automatically because large local libraries can make startup feel locked.
- On the maintainer laptop under Hyprland, launch visual checks silently on workspace 2 so the user's active workspace is not stolen: `hyprctl dispatch exec "[workspace 2 silent] /usr/bin/melearner"`. Use the same wrapper with startup-route environment variables for playback diagnostics.
- For a repeatable installed-player render check, run `scripts/verify-installed-native-playback.sh`. It opens `/usr/bin/melearner` silently on workspace 2 when Hyprland is available, waits for `native.player.load.ready`, requires the `render-api:gtk-opengl` in-window backend with nonzero submitted frames/dimensions, checks the native-surface first-frame log, rejects a separate video window, fails if normal playback starts a new `ffmpeg`/`ffprobe` process, and then closes the app.

Rules for this pipeline:

- Keep React focused on layout, controls, and state display.
- Keep local-file validation and playback commands behind the native player module.
- Keep FFmpeg out of ordinary playback.
- Use FFmpeg only for queued thumbnails, metadata, or explicit future processing work.
- Remove stale player files, dependencies, docs, aliases, and generated artifacts in the same change that replaces them.
- After any launchable behavior change on this laptop, install the native Arch package so the desktop launcher runs the updated `/usr/bin/melearner` binary. Do not treat `~/.cargo/bin/melearner` as an installed app instance.
- The Arch package depends on `mpv` because Arch's `mpv` package provides `libmpv.so`; this is an embedded-library dependency, not permission to launch an external `mpv` process. It also depends on `libglvnd` for the OpenGL render-api backend.
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

SQLite write rule: frontend writes must go through the serialized write queue and ordinary autocommit statements. Do not add manual `BEGIN`, `COMMIT`, or `ROLLBACK` calls through `tauri-plugin-sql`; the plugin uses a `sqlx` pool, so separate frontend commands are not a pinned transaction and can lock the database during large scans. Move any future true transaction to Rust where one connection can be held for the whole operation.

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

This script builds the production Tauri binary with `pnpm tauri build --no-bundle --ci`, packages `melearner-bin`, and installs the package that owns `/usr/bin/melearner`. Do not replace that Tauri build step with plain `cargo build`; direct Cargo builds can produce a dev-mode binary that tries to load `http://localhost:3000` instead of bundled static assets. The script preserves ignored build caches such as `.next`, `out`, and `src-tauri/target` and chooses the fastest available Rust cache mode.

Do not run `pnpm install:arch:fast` after every edit. Use focused tests, `npm run type-check`, and targeted Rust tests first. Run the Arch install only after the source change is already checked and the launcher-visible `/usr/bin/melearner` app must be verified. Repeating full Tauri package builds for unverified intermediate edits wastes time and can make the app feel stalled even when the actual code change is small.

When package contents change but `pkgver` remains the same, increment Arch `pkgrel`. Keep `pkgver=0.1.8` while the upstream app version is still `0.1.8`; use `pkgrel` for local/AUR rebuilds of that same version. Reset `pkgrel` to `1` only when `pkgver` changes to a new upstream version. Do not churn `pkgrel` for source-only changes that are not packaged or installed.

During iterative local development, do not delete `.next`, `out`, `src-tauri/target`, or `tsconfig.tsbuildinfo` after every package install. Those ignored paths are build caches/outputs, not stale source artifacts. Removing `src-tauri/target` forces the next Tauri package install to compile the GTK/WebKit/Tauri/Rust dependency graph again, which can take minutes. Clean them only when intentionally doing a cold rebuild, validating release reproducibility, recovering from a poisoned cache, or preparing a final source-only handoff where the slower next build is acceptable.

When `sccache` is installed, the script sets `RUSTC_WRAPPER=sccache` and defaults `CARGO_INCREMENTAL=0` so release compilation can be cached across clean local builds. Without `sccache`, it defaults `CARGO_INCREMENTAL=1` to reuse Cargo's local target directory during iterative laptop builds.

Public Linux releases should publish only:

- AppImage for portable Linux use
- Native Arch package asset used by AUR and optional manual `pacman -U`

Do not upload `.deb` or `.rpm` unless those channels are intentionally restored and tested.

## Repo Hygiene

Remove stale generated artifacts, completed temporary plans, redundant docs, obsolete code, and unused settings as part of the same task that makes them unnecessary. Do not keep duplicate structures, old UI branches, or compatibility shims after the repo has a canonical replacement.

Before finishing iterative build or install work, remove package tarballs, package staging directories, temp screenshots, logs, and one-off verification files. Keep ignored build caches such as `.next`, `out`, `src-tauri/target`, and `tsconfig.tsbuildinfo` unless the task explicitly calls for a clean rebuild or source-only cleanup.

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
