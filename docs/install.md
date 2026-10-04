# Install

## Desktop downloads

Download version `0.1.9` for your system:

| System | Download |
| --- | --- |
| Linux x86_64 with glibc 2.39 or newer | [AppImage](https://github.com/WhiteHades/melearner/releases/download/v0.1.9/melearner_0.1.9_amd64.AppImage) |
| Arch Linux x86_64 | [Package](https://github.com/WhiteHades/melearner/releases/download/v0.1.9/melearner-bin-0.1.9-1-x86_64.pkg.tar.zst) |
| Windows x64 | [Setup installer](https://github.com/WhiteHades/melearner/releases/download/v0.1.9/melearner-0.1.9-setup.exe) |
| macOS Apple silicon with macOS 15 or newer | [Disk image](https://github.com/WhiteHades/melearner/releases/download/v0.1.9/melearner-0.1.9-macos-arm64.dmg) |

The installers include the app runtime, including its browser engine, database
and media playback support. You do not need to install a separate browser,
database server or codec pack.

The Windows installer is unsigned, so Windows may show a security warning. The
macOS app is not notarized. You may need to approve it in macOS settings before
opening it.

On Windows, open the EXE and follow the installer. On macOS, open the DMG and
drag meLearner to Applications.

On Linux, keep the AppImage in a folder such as `Applications`, mark it
executable in your file manager and open it. Installing it in your user account
does not need administrator privileges. If
your desktop lacks FUSE, run it from a terminal with `--appimage-extract-and-run`.
The glibc requirement does not guarantee compatibility with every distribution
or graphics driver.

On Arch or Omarchy, install the downloaded package with:

```bash
sudo pacman -U melearner-bin-0.1.9-1-x86_64.pkg.tar.zst
```

## Linux from source

The application needs C and C++23 compilers, CMake 4.4 or newer, Meson 1.3
or newer, Ninja, patch, pkg-config, Qt 6.11.2 Widgets/OpenGLWidgets/Network/Pdf/WebEngineWidgets/Concurrent/Test,
SQLite, libzip, md4c with md4c-html, and the media development libraries below. On Arch Linux:

```bash
sudo pacman -S --needed \
  base-devel cmake meson ninja patch pkgconf qt6-base qt6-webengine qt6-imageformats sqlite libzip md4c \
  ffmpeg libass libplacebo luajit alsa-lib libpipewire libpulse mesa libdrm libva libvdpau \
  libx11 libxext libxpresent libxrandr libxss wayland wayland-protocols libxkbcommon \
  ffnvcodec-headers
```

Build, test, and install into `$HOME/.local`:

```bash
git clone https://github.com/WhiteHades/melearner
cd melearner
bash scripts/install-cpp-linux.sh
```

The installer accepts an absolute prefix when needed:

```bash
bash scripts/install-cpp-linux.sh "$HOME/.local"
```

The first build needs network access to download verified source archives.
Linux builds include a pinned libmpv with an upstream PipeWire startup fix.
It is installed privately beside melearner and does not replace the system player.
The running application is local only. The interface uses shadcn-cpp's native
dark theme and bundled Geist font.

HTML and Markdown documents use Qt WebEngine inside the application. The Linux
source installer copies its sandboxed helper, resource packs, locales and
Chromium notices into app-specific directories. Qt libraries still come from
the development packages for a source installation. The installer locates
`qtpaths` through Qt's pkg-config binary directory when it is not on `PATH`.
When QtWebEngine is installed in a separate module prefix, packaging resolves
its helper, resources and locales together from that provider rather than
assuming every Qt module shares the `qtpaths` prefix.
The document viewer starts
only when needed, blocks network access, and restricts files to the open course.
No separately installed browser or database server is needed.

Launch with `$HOME/.local/bin/melearner`. The desktop entry is installed with
the executable when the prefix supports it.

For Windows build requirements, see the [Windows development notes](windows-development.md).
