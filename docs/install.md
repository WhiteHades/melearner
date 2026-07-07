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

Windows MSI test artifacts are available from the manually dispatched `windows msi downloader` workflow. These MSI artifacts are Authenticode-signed in CI with a per-run self-signed test certificate, then verified before upload. They prove the packaging and signing path, but they are still for end-to-end testing only, not supported production releases. Windows may still show an unknown-publisher or SmartScreen warning because the test certificate is not a trusted production code-signing certificate.

1. Open the [Actions page](https://github.com/WhiteHades/melearner/actions/workflows/windows-msi.yml).
2. Open the latest successful run for the commit you want to test.
3. Download `melearner-windows-signed-msi-<commit-sha>`.
4. Unzip the downloaded artifact.
5. Run `melearner_0.1.8_x64_en-US.msi`, scan a local course folder, and open a playable lesson.

The artifact also includes `melearner-windows-msi-test-signing.cer`, the public half of the per-run test certificate. Install it only when you specifically need to verify the self-signed signature locally; do not treat it as a production trust root.

From GitHub CLI:

```bash
gh run list --repo WhiteHades/melearner --workflow windows-msi.yml --branch main
gh run download "<run-id>" --repo WhiteHades/melearner --name "melearner-windows-signed-msi-<commit-sha>" --dir dist/download
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
