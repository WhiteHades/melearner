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

## Experimental Windows and macOS

Windows and macOS downloads are experimental and may not work on every machine yet. The app's playback engine is embedded libmpv, and those platforms have source render hosts, but their packages still need clean-machine visual playback verification and deliberate libmpv dependency bundling before they can be advertised as production-ready.

Keep experimental Windows and macOS downloads clearly labeled. Do not describe them as stable until each platform can visibly play local media through the embedded native surface in one app window on a clean machine.

Windows MSI artifacts are available from the manually dispatched `windows msi downloader` workflow. These MSI artifacts are Authenticode-signed in CI, then checked before upload. If production code-signing secrets are configured, the workflow signs with that trusted certificate. Otherwise it falls back to a per-run self-signed test certificate. The fallback proves the packaging and signing path, but it is still for end-to-end testing only, not a supported production release. Windows may still show an unknown-publisher or SmartScreen warning when the artifact is signed by the fallback test certificate.

1. Open the [Actions page](https://github.com/WhiteHades/melearner/actions/workflows/windows-msi.yml).
2. Open the latest successful run for the commit you want to test.
3. Download `melearner-windows-signed-msi-<commit-sha>`.
4. Unzip the downloaded artifact.
5. Run `melearner_0.1.8_x64_en-US.msi`, scan a local course folder, and open a playable lesson.

The artifact includes `melearner-windows-msi-signing.txt`, which says whether the MSI was signed with `mode=production` or `mode=test`. In test mode, the artifact also includes `melearner-windows-msi-test-signing.cer`, the public half of the per-run test certificate. Install that certificate only when you specifically need to verify the self-signed signature locally; do not treat it as a production trust root.

To produce a Windows-trusted production signature, maintainers must obtain a code-signing certificate from a certificate authority trusted by Windows and add these repository settings:

- `WINDOWS_CODE_SIGNING_PFX_BASE64` secret: base64 text for the `.pfx` code-signing certificate and private key.
- `WINDOWS_CODE_SIGNING_PFX_PASSWORD` secret: password for that `.pfx`.
- `WINDOWS_CODE_SIGNING_TIMESTAMP_URL` repository variable: timestamp server URL from the certificate authority. This is strongly recommended so installed artifacts remain verifiable after the certificate expires.

Create the base64 secret from PowerShell:

```powershell
[Convert]::ToBase64String([IO.File]::ReadAllBytes("C:\path\to\certificate.pfx")) | Set-Clipboard
```

If the certificate is hardware-backed, cloud-HSM-backed, or locked behind a signing service instead of exportable as a `.pfx`, the workflow needs that provider's signing integration instead of these PFX secrets.

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
