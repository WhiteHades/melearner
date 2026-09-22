# Build from source

melearner uses C++23 and Qt Widgets. The application source is in `cpp-app/src`.

## Requirements

Install a C++23 compiler, CMake 4.4 or newer, Ninja, pkg-config, Qt 6.11.2 or
newer with Widgets, OpenGLWidgets, Network, Pdf and Test, SQLite, libmpv,
libzip, and md4c. Arch Linux package commands are in [Install](install.md).
Windows instructions are in [Windows builds](windows-development.md).

On Arch, `qt6-webengine` supplies Qt PDF. melearner links the native Qt PDF
library and does not embed a browser or QML runtime. PDF pages are rendered
on a worker thread into a bounded tile cache.

The first configure downloads the SHA-256-pinned Lexbor 3.0.0 source archive
for HTML parsing. Its license and notice are included in the installation.
The application itself is local only.

## Build and test

From the repository root:

```bash
cmake --preset linux-dev
cmake --build --preset linux-dev --parallel 4
ctest --preset linux-dev --no-tests=error
```

For a release build:

```bash
cmake --preset linux-release
cmake --build --preset linux-release --parallel 4
ctest --preset linux-release --no-tests=error
```

The regular CTest suites use Qt's offscreen platform. The two playback suites
need a private X11 display because the libmpv OpenGL surface cannot be created
by the offscreen plugin:

```bash
Xvfb :99 -screen 0 1920x1080x24 -nolisten tcp -noreset &
DISPLAY=:99 QT_QPA_PLATFORM=xcb LIBGL_ALWAYS_SOFTWARE=1 \
  ./build/cpp-release/playback_render_test
DISPLAY=:99 QT_QPA_PLATFORM=xcb LIBGL_ALWAYS_SOFTWARE=1 \
  ./build/cpp-release/main_playback_test
```

Use the source installer for the complete local check and installation:

```bash
bash scripts/install-cpp-linux.sh "$HOME/.local"
```

The deterministic fixture and packaging checks are separate from CTest:

```bash
cmake -DCPP_PARITY_FULL_MATERIALIZATION=ON -P scripts/test-cpp-parity-fixtures.cmake
python3 scripts/test-cpp-linux-installer.py
python3 scripts/test-cpp-linux-archive.py
python3 scripts/test-cpp-linux-runtime.py
bash scripts/test-cpp-linux-packaging.sh
```

`library_load_test` is an explicit large-library diagnostic; it is not part of
the normal CTest run because it materializes a 100,000-lesson fixture.

## Architecture

Qt owns the single application window, widgets, models, focus, keyboard input,
and accessibility. C++ modules own the current SQLite schema, course scan,
search, progress, documents, PDF rendering, and embedded libmpv player.
Tests use isolated temporary libraries. Application data paths are listed in
[Privacy](privacy-and-legal.md).

The Linux player is an in-window OpenGL surface. It uses libmpv in process and
does not launch an external player or codec helper. Media files are opened from
the selected course root after path validation.

## Packaging

The Linux archive can be staged with:

```bash
bash scripts/package-cpp-linux.sh --build-dir build/cpp-release
```

`scripts/package-cpp-appimage.sh` and `scripts/package-cpp-arch.sh` use the
same release build. Packaging requires third-party notices, an SPDX inventory,
and runtime dependency records. The scripts report missing inputs before
creating a package. No `0.1.9` binary package is currently published.
