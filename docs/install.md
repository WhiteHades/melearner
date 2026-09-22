# Install

## Linux from source

The application needs a C++23 compiler, CMake 4.4 or newer,
Ninja, pkg-config, Qt 6.11.2 Widgets/OpenGLWidgets/Network/Pdf/Test, SQLite,
libmpv, libzip, and md4c. On Arch Linux:

```bash
sudo pacman -S --needed base-devel cmake ninja pkgconf qt6-base qt6-webengine sqlite mpv libzip md4c
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

Launch with `$HOME/.local/bin/melearner`. The desktop entry is installed with
the executable when the prefix supports it.

## Binary packages

Binary packages for version `0.1.9` are not yet available. Use the source
installer above. Older release downloads contain an earlier application.

## Other platforms

Windows and macOS packages are not available. Contributors can use the
[Windows build guide](windows-development.md) to build and test on Windows.
