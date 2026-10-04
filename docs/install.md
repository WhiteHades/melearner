# Install

## Desktop downloads

Download version `0.1.9` for your system:

| System | Download |
| --- | --- |
| Linux | [AppImage](https://github.com/WhiteHades/melearner/releases/download/v0.1.9/melearner_0.1.9_amd64.AppImage) |
| Linux (Arch) | [Package](https://github.com/WhiteHades/melearner/releases/download/v0.1.9/melearner-bin-0.1.9-1-x86_64.pkg.tar.zst) |
| Windows | [Setup installer](https://github.com/WhiteHades/melearner/releases/download/v0.1.9/melearner-0.1.9-setup.exe) |
| macOS | [Disk image](https://github.com/WhiteHades/melearner/releases/download/v0.1.9/melearner-0.1.9-macos-arm64.dmg) |

Linux requires x86_64 and glibc 2.39 or newer. Windows requires x64. macOS
requires Apple silicon and macOS 15 or newer.

The installers include the browser engine, database and media runtime. Windows
users do not need a separate development SDK or compiler runtime. No separate
browser, database server or codec pack is needed. The Windows installer is
unsigned and may show a security warning. The macOS app is not notarized; you
may need to approve it in macOS settings.

On Windows, open the EXE and follow the installer. On macOS, open the DMG and
drag meLearner to Applications.

On Linux, keep the AppImage in a folder such as `Applications`, mark it
executable and open it. No administrator access is needed. If your desktop lacks
FUSE, run it with `--appimage-extract-and-run`. The glibc minimum does not
guarantee support for every distribution or graphics driver.

On Arch or Omarchy, install the downloaded package:

```bash
sudo pacman -U melearner-bin-0.1.9-1-x86_64.pkg.tar.zst
```

## Linux from source

The app uses C++23 and Qt 6.11.2. On Arch, install these build prerequisites:

```bash
sudo pacman -S --needed \
  base-devel cmake meson ninja patch pkgconf qt6-base qt6-webengine qt6-imageformats sqlite libzip md4c \
  ffmpeg libass libplacebo luajit alsa-lib libpipewire libpulse mesa libdrm libva libvdpau \
  libx11 libxext libxpresent libxrandr libxss wayland wayland-protocols libxkbcommon \
  ffnvcodec-headers
```

Build and install into `$HOME/.local`:

```bash
git clone https://github.com/WhiteHades/melearner
cd melearner
bash scripts/install-cpp-linux.sh
```

Pass an absolute prefix to choose another location:

```bash
bash scripts/install-cpp-linux.sh "$HOME/.local"
```

The first build downloads verified source archives. It builds pinned libmpv
with a PipeWire startup fix and installs it beside meLearner without replacing
the system player. The source installer bundles Qt WebEngine's helper, resources,
locales and Chromium notices, but uses Qt libraries from the development
packages. The document viewer starts on demand, blocks network access and
limits files to the open course.

Launch with `$HOME/.local/bin/melearner`. The installer also adds a desktop entry
when the prefix supports it.

For Windows build requirements, see [Windows development](windows-development.md).
