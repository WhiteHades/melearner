# Install

## Desktop downloads

Download version `0.1.9` for your system:

| System | Download |
| --- | --- |
| Linux | [AppImage](https://github.com/WhiteHades/melearner/releases/download/v0.1.9/melearner_0.1.9_amd64.AppImage) |
| Linux (Arch) | [Package](https://github.com/WhiteHades/melearner/releases/download/v0.1.9/melearner-bin-0.1.9-1-x86_64.pkg.tar.zst) |
| Windows | [Setup installer](https://github.com/WhiteHades/melearner/releases/download/v0.1.9/melearner-0.1.9-setup.exe) |
| macOS | [Disk image](https://github.com/WhiteHades/melearner/releases/download/v0.1.9/melearner-0.1.9-macos-arm64.dmg) |

Linux needs x86_64 and glibc 2.39 or newer. Windows needs x64. macOS needs Apple silicon and macOS 15 or newer. The Linux minimum does not guarantee support for every distribution or graphics driver.

The downloads include the browser engine, database and media runtime. No separate browser, database server or codec pack is needed.

### Windows

Open the EXE and follow the installer. It is unsigned, so Windows may show a security warning.

### macOS

Open the DMG and drag meLearner to Applications. The app is not notarized, so macOS may ask you to approve it in Settings.

### Linux

Make the AppImage executable and open it:

```bash
chmod +x melearner_0.1.9_amd64.AppImage
./melearner_0.1.9_amd64.AppImage
```

If FUSE is unavailable, run it with `--appimage-extract-and-run`.

### Linux (Arch)

Install the downloaded package:

```bash
sudo pacman -U melearner-bin-0.1.9-1-x86_64.pkg.tar.zst
```

## Linux from source

The source build needs C++23, Qt 6.11.2 and the development libraries below. On Arch, install the prerequisites:

```bash
sudo pacman -S --needed \
  base-devel cmake meson ninja patch pkgconf qt6-base qt6-webengine qt6-imageformats sqlite libzip md4c \
  ffmpeg libass libplacebo luajit alsa-lib libpipewire libpulse mesa libdrm libva libvdpau \
  libx11 libxext libxpresent libxrandr libxss wayland wayland-protocols libxkbcommon \
  ffnvcodec-headers
```

Then build and install to `$HOME/.local`:

```bash
git clone https://github.com/WhiteHades/melearner
cd melearner
bash scripts/install-cpp-linux.sh
```

The first build fetches verified source archives. The installer builds libmpv with a PipeWire fix and keeps it beside the app. For build details, including Windows, see [development](development.md) and [Windows development](windows-development.md).

To choose another install location, pass an absolute path:

```bash
bash scripts/install-cpp-linux.sh "$HOME/.local"
```
