# Install

## Linux from source

The application needs C and C++23 compilers, CMake 4.4 or newer, Meson 1.3
or newer, Ninja, patch, pkg-config, Qt 6.11.2 Widgets/OpenGLWidgets/Network/Pdf/WebEngineWidgets/Concurrent/Test,
SQLite, libzip, md4c with md4c-html, and the media development libraries below. On Arch Linux:

```bash
sudo pacman -S --needed \
  base-devel cmake meson ninja patch pkgconf qt6-base qt6-webengine sqlite libzip md4c \
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
the development packages for a source installation. The document viewer starts
only when needed, blocks network access, and restricts files to the open course.
No separately installed browser or database server is needed.

Launch with `$HOME/.local/bin/melearner`. The desktop entry is installed with
the executable when the prefix supports it.

## Binary packages

Binary packages for version `0.1.9` are not yet available. Use the source
installer above. Older release downloads contain an earlier application.

## Other platforms

Windows and macOS packages are not available. Contributors can use the
[Windows build guide](windows-development.md) to build and test on Windows.
