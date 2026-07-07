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

Windows and macOS installers are not production release targets yet. The app's playback engine is embedded libmpv, and those platforms still need true in-window native render hosts before their packages can be advertised as supported.

Do not publish Windows MSI, NSIS, macOS DMG, or macOS app-bundle release artifacts until each platform can visibly play local media through the embedded native surface in one app window on a clean machine.

FFmpeg is not part of ordinary playback. If Windows thumbnail generation is supported without user-installed FFmpeg, bundle FFmpeg deliberately and handle licensing.

## From Source

```bash
git clone https://github.com/WhiteHades/melearner
cd melearner
pnpm install
pnpm tauri:dev
```
