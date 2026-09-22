# Install

## Linux from source

The Linux-first C++ application needs a C++23 compiler, CMake 4.4 or newer,
Ninja, pkg-config, Qt 6.11 Widgets/OpenGLWidgets/Network/Pdf/Test, SQLite,
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

## Arch package

The Arch package is produced from the C++ release build with:

```bash
cmake --preset linux-release
cmake --build --preset linux-release --parallel 4
bash scripts/package-cpp-arch.sh --build-dir build/cpp-release
```

The package path is still undergoing runtime and legal-input qualification.

## Other platforms

macOS and Windows are planned after the Linux-first implementation. No legacy
runtime or compatibility package is shipped.
