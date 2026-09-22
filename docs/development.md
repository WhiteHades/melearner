# Development

The maintained application is the C++23/Qt Widgets executable in `cpp-app/`.
The repository no longer contains the former web, Tauri, Rust, or Zig
application lines.

## Build and test

Install the native prerequisites listed in [Install](install.md), then run:

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
search, progress, notes, documents, PDF rendering, and embedded libmpv player.
The application uses one current schema and the isolated C++ data path; it does
not import, migrate, or inspect data from the removed application lines.

The Linux player is an in-window OpenGL surface. It uses libmpv in process and
does not launch an external player or codec helper. Media files are opened from
the selected course root after path validation.

## Packaging

The diagnostic Linux archive is built with:

```bash
bash scripts/package-cpp-linux.sh --build-dir build/cpp-release
```

Arch packaging uses the C++ release build and the runtime stager. The package
and legal-input checks must pass before calling an artifact release-qualified.
GitHub Actions is disabled; run the checks locally and record the exact
commands and results for each release change.
