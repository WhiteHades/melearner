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

## Playback Fallback

The player is WebKitGTK HTML media playback inside Tauri, not a native Rust video player. Rust helps prepare files only when WebKit rejects a source.

Known failure mode: some files are named `.mp4` but are actually MPEG-TS containers. They may contain browser-safe H.264/AAC streams, but WebKit rejects the container. The fallback first remuxes those files into a real MP4 with `-c copy`, then only transcodes if remux fails.

Rules for this pipeline:

- Remux before transcode.
- Never run multiple playback-prep FFmpeg jobs at once.
- Cancel stale jobs when changing lessons or unmounting the player.
- Bound transcode CPU with low thread counts.
- Drop data/subtitle streams during media preparation.

See `docs/adr/0006-playback-fallback-remuxes-before-transcoding.md`.

## Scroll Performance

Course cards must not start FFmpeg or decode video while the user scrolls the library. Runtime video thumbnail extraction caused janky scrolling and competed with playback fallback.

See `docs/adr/0007-course-cards-do-not-generate-runtime-video-thumbnails.md`.

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

Public Linux releases should publish only:

- AppImage for portable Linux use
- Native Arch package asset used by AUR and optional manual `pacman -U`

Do not upload `.deb` or `.rpm` unless those channels are intentionally restored and tested.

## Windows MSI Builds

MSI builds require Windows and WiX:

```powershell
pnpm install
pnpm tauri build --target x86_64-pc-windows-msvc --bundles msi
```

The release workflow includes a Windows MSI job on `windows-latest`. It must still be validated on a clean Windows VM before a Windows release is advertised as supported.

Before publishing a Windows MSI, test on a clean Windows VM:

- Install and launch
- WebView2 availability
- Library scan
- Video/audio playback
- Playback fallback behavior when FFmpeg is missing or bundled
- Upgrade and uninstall

Windows media notes:

- WebView2 Runtime must be present or installed by the bundle mode.
- Windows N editions may require Microsoft's Media Feature Pack.
- FFmpeg is not provided by WebView2 or Tauri. If Windows fallback support is required without user-installed FFmpeg, bundle FFmpeg deliberately and handle licensing.

## Architecture Notes

- The database lives at `$HOME/.local/share/melearner/melearner.db`.
- Files load through Tauri asset URLs, not a localhost media server.
- The frontend calls Tauri commands directly.
- Logs live under `~/.melearner/`.
- ADRs live in `docs/adr/`.
- Future stats and durable course identity are documented in `docs/stats-and-identity-plan.md`.
