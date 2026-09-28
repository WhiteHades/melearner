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

The first configure downloads two SHA-256-pinned source archives. Lexbor 3.0.0
supplies HTML parsing, and the pinned shadcn-cpp commit supplies every interface
component, its theme and its font. Both licenses are included in the
installation. The application itself is local only.

## Interface

Every control, the colour theme and the interface font come from shadcn-cpp,
pinned to one reviewed commit in `cpp-app/cmake/shadcn.cmake`. The application
holds no local stylesheet and no local palette: a colour mode change is an
install of the theme, and the two modes are the neutral theme's light and dark
values.

`cpp-app/src/theme.hpp` is the bridge. It reads the installed theme for the
widgets the application still paints itself, such as the list rows, and it
supplies the two answers the theme does not: the system's high contrast palette
mapped onto the theme's roles, and the platform's reduced motion signal, which
Qt 6.11 exposes as the widget animation duration.

One component came from melearner rather than from the shadcn/ui catalogue:
the Stats activity grid, added to shadcn-cpp as a `Heatmap` because the pinned
upstream registry has no equivalent. That addition is recorded in the
component library, not here.

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

### Known gap: the video settings menu does not open from the keyboard on Wayland

`main_playback_test` passes on a private X11 display and fails on a real Wayland
session. The failure is in `playsPausesAndRestoresPosition`: the video player's
settings button, focused and pressed with Space, does not open its menu.

This is not a slow compositor. The test was given a full second of polling and the
menu never appeared. A mouse click on the same button opens the menu immediately,
so the menu itself is fine and the fault is in the keyboard path. The button is
visible and enabled throughout, and the surrounding keyboard checks in the same test
pass, so it is not a general loss of keyboard focus.

Why it matters: a reader who does not use a pointer cannot reach the video settings
at all. This is a keyboard accessibility defect, not a cosmetic one, and it is the
reason the two playback suites are not part of CTest: they need a display, so this
never ran in the default check and stayed hidden until the suites were run on the
machine's own session.

Unresolved. The cause has not been isolated: the same code opens the menu under X11
and does not under Wayland, so the difference is in how the platform plugin delivers
the activation to a button that owns a menu, or in the shadcn button's own key
handling consuming Space before the button's menu logic sees it. It needs an
investigation with the two paths compared, not a change made on a guess.

The rest of the Wayland run is clean: `playback_render_test` passes 6 of 6 and
`main_window_test` passes 13 of 13 on the real session with hardware GL.

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
the selected course root after path validation. The transport is built from
shadcn components, but the decode and the surface are not: the component
library's optional media player takes a URL and owns its own transport, and this
application needs to drive a path it has already validated, seek to a saved
position, add a subtitle file and report progress.

## Measured work

The statements that run often are held to measured numbers rather than assumed
ones. On a synthetic 400-course, 48,000-lesson library:

| Path | Before | After |
| --- | --- | --- |
| Library stats, one scan instead of five | 12.8 ms | 6.5 ms |
| Resume, ranking only the page's lessons | 151 ms | 16 ms |

Stats runs every few seconds while video plays, and resume runs on every return
to the Library, which is why both are worth measuring. Other repeated work was
removed rather than measured: a PDF tile cache that survived a resize, a
position label that redrew once a second instead of once a frame, and HTML and
Markdown files that were decoded twice.

## Packaging

Installing for the current user puts the application in the session's `PATH`,
registers a desktop entry, and installs the icon into the hicolor theme at the
sizes a shell asks for. The desktop database is refreshed so the entry appears
without a logout.

```bash
bash scripts/install-cpp-linux.sh "$HOME/.local"
```

The icon set lives in `cpp-app/assets/icons/hicolor`. Below 32 pixels it carries
only the play triangle, because the book's two shapes blur together at that size
and the triangle is the part that still reads.

A folder can be passed on the command line, and that is what the desktop entry
hands over:

```bash
melearner ~/Courses
```

A file is refused rather than resolved. A library is built from the folders under
a root, so the folder holding a lesson file is a course or a section and never a
root, and any ancestor the application picked would be a guess.

The Linux archive can be staged with:

```bash
bash scripts/package-cpp-linux.sh --build-dir build/cpp-release
```

`scripts/package-cpp-appimage.sh` and `scripts/package-cpp-arch.sh` use the
same release build. Packaging requires third-party notices, an SPDX inventory,
and runtime dependency records. The scripts report missing inputs before
creating a package. No `0.1.9` binary package is currently published.
