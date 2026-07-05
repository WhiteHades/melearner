# Install

## Arch Linux

Use the AUR package `melearner-bin`.

```bash
yay -S melearner-bin
# or
paru -S melearner-bin
```

Optional manual install: download `melearner-bin-<version>-1-x86_64.pkg.tar.zst` from the [latest release](https://github.com/WhiteHades/melearner/releases/latest), then run:

```bash
sudo pacman -U melearner-bin-<version>-1-x86_64.pkg.tar.zst
```

## Other Linux Distros

Download the AppImage from the [latest release](https://github.com/WhiteHades/melearner/releases/latest), then run:

```bash
chmod +x melearner_<version>_amd64.AppImage
./melearner_<version>_amd64.AppImage
```

## Windows

Windows MSI support requires a Windows build machine because Tauri's MSI bundler uses WiX on Windows. Published Windows installers should be treated as supported only after clean-VM testing.

For local Windows builds:

```powershell
pnpm install
pnpm tauri build --target x86_64-pc-windows-msvc --bundles msi
```

Required tools:

- Rust MSVC toolchain
- Node.js and pnpm
- Microsoft C++ Build Tools
- WiX Toolset for MSI bundling
- WebView2 Runtime on the target machine, or a bundled/offline WebView2 installer mode
- Media Feature Pack on Windows N editions if media playback support is missing

Windows release artifacts must be built and tested on Windows before publishing.

FFmpeg is not bundled by default. Playback fallback features require FFmpeg to be available, or a future Windows package must bundle and invoke FFmpeg intentionally.

## From Source

```bash
git clone https://github.com/WhiteHades/melearner
cd melearner
pnpm install
pnpm tauri:dev
```
