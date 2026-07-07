# Install

## Arch Linux

Use the AUR package `melearner-bin`.

```bash
yay -S melearner-bin
# or
paru -S melearner-bin
```

Optional manual install: download `melearner-bin-<version>-<pkgrel>-x86_64.pkg.tar.zst` from the [latest release](https://github.com/WhiteHades/melearner/releases/latest), then run:

```bash
sudo pacman -U melearner-bin-<version>-<pkgrel>-x86_64.pkg.tar.zst
```

## Other Linux Distros

Download the AppImage from the [latest release](https://github.com/WhiteHades/melearner/releases/latest), then run:

```bash
chmod +x melearner_<version>_amd64.AppImage
./melearner_<version>_amd64.AppImage
```

## Windows and macOS

Windows and macOS installers are not production release targets yet. The app's playback engine is embedded libmpv, and those platforms have source render hosts, but their packages still need clean-machine visual playback verification and deliberate libmpv dependency bundling before they can be advertised as supported.

Do not publish Windows MSI, NSIS, macOS DMG, or macOS app-bundle release artifacts until each platform can visibly play local media through the embedded native surface in one app window on a clean machine.

Windows portable test artifacts are available from the manually dispatched `windows portable test build` workflow. These artifacts are for end-to-end testing only, not supported production releases:

1. Open the [Actions page](https://github.com/WhiteHades/melearner/actions/workflows/windows-portable.yml).
2. Open the latest successful run for the commit you want to test.
3. Download `melearner-windows-portable-<commit-sha>`.
4. Unzip the downloaded artifact, then unzip `melearner-windows-portable.zip`.
5. Run `melearner.exe`, scan a local course folder, and open a playable lesson.

From GitHub CLI:

```bash
gh run list --repo WhiteHades/melearner --workflow windows-portable.yml --branch main
gh run download "<run-id>" --repo WhiteHades/melearner --name "melearner-windows-portable-<commit-sha>" --dir dist/download
```

Maintainer verification from a checkout:

```bash
pnpm stage:windows-runtime -- --app-bin "<installed executable>"
pnpm verify:native-playback -- --app-bin "<installed executable>" --course-id "<course-id>" --lesson-id "<lesson-id>"
```

Windows must report `render-api:wgl-opengl`. macOS must report `render-api:appkit-opengl`. Passing compile-readiness CI is not enough to publish installers.

FFmpeg is not part of ordinary playback. If Windows thumbnail generation is supported without user-installed FFmpeg, bundle FFmpeg deliberately and handle licensing.

## From Source

```bash
git clone https://github.com/WhiteHades/melearner
cd melearner
pnpm install
pnpm tauri:dev
```
