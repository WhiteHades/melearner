# Build from source

melearner uses C++23 and Qt Widgets. The application source is in `cpp-app/src`.

## Requirements

Install C and C++23 compilers, CMake 4.4 or newer, Ninja, pkg-config, Qt 6.11.2 or
newer with Widgets, OpenGLWidgets, Network, Pdf and Test, SQLite, libzip, and
md4c. Linux also needs Meson 1.3 or newer, patch, and the media development
libraries listed in the [Arch Linux installation commands](install.md).
Windows instructions are in [Windows builds](windows-development.md).

On Arch, `qt6-webengine` supplies Qt PDF. melearner links the native Qt PDF
library and does not embed a browser or QML runtime. PDF pages are rendered
on a worker thread into a bounded tile cache.

The first configure downloads two SHA-256-pinned source archives. Lexbor 3.0.0
supplies HTML parsing, and the pinned shadcn-cpp commit supplies every interface
component, its theme and its font. Both licenses are included in the
installation. Linux builds also download mpv 0.41.0 and an upstream PipeWire
startup patch, each verified by SHA-256. The application itself is local only.

`cpp-app/cmake/mpv.cmake` builds the Linux player runtime and installs it under
`lib/melearner`. The installed executable uses a relative runtime path to load
that copy. PipeWire, PulseAudio, OpenGL, VAAPI and VDPAU support are required
at build time; x86_64 also requires the NVDEC headers. The selected decoder
still depends on the hardware, driver and media. Windows continues to use its
native libmpv package.

The mpv build retains GPL-enabled hardware paths. Its source archive, upstream
patch, build recipe, copyright information and license texts are installed
under `share/doc/melearner/sources/mpv` and `share/licenses/melearner/mpv`.
Third-party libraries retain their own licenses. These files do not replace
the complete dependency inventory required when publishing a binary package.

## Interface

The interface uses shadcn-cpp widgets, composed into melearner's own library,
lesson, and player views. The dependency also supplies the theme and font, and
is pinned in `cpp-app/cmake/shadcn.cmake`. The application currently installs
the neutral dark theme only. The library's light theme is used in tests, not
offered as an application setting.

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
need an X11 display because the libmpv OpenGL surface cannot be created by the
offscreen plugin. Run both under private Xvfb sessions with a null PulseAudio
sink:

```bash
bash scripts/test-cpp-playback.sh
bash scripts/test-cpp-playback.sh --build-dir build/cpp-release
```

The playback tests also require the FFmpeg command-line tool. The recovery
test remuxes the checked-in clip into a longer temporary file without encoding
new media, then checks error recovery, closing and resuming a lesson.

The window tests use the application's font and check normal and doubled text.
To save their screenshots, create a destination and pass it to the test process:

```bash
mkdir -p .tmp/ui-captures
bash scripts/test-cpp-playback.sh -- env MELEARNER_TEST_SCREENSHOTS="$PWD/.tmp/ui-captures" \
  build/cpp-release/main_window_test
bash scripts/test-cpp-playback.sh -- env MELEARNER_TEST_SCREENSHOTS="$PWD/.tmp/ui-captures" \
  build/cpp-release/main_playback_test
```

The stats check waits for resized metric labels to fit before capturing them.
Inspect the images as well as the test result; geometry checks alone do not
establish visual correctness.

The default build directory is `build/cpp-dev`; set `MELEARNER_BUILD_DIR` or
pass `--build-dir` to select another. The runner requires `pulseaudio`, Xvfb,
`xvfb-run`, `xauth`, Openbox, `dbus-daemon`, `pactl`, and `timeout`. It starts a
separate Xvfb, private D-Bus, and PulseAudio null sink for each suite, redirects XDG and
temporary data into a private `.tmp/cpp-playback` run directory, runs the test
process with `LC_ALL=C`, and leaves `HOME` unchanged. It selects Mesa software
OpenGL and the Qt portal theme so desktop GTK styles and proprietary GLX
overrides do not change the test environment.
Each command has a 60-second limit, configurable with
`MELEARNER_PLAYBACK_TIMEOUT_SECONDS`; logs are retained under the run directory
and the script prints their path. The private Unix sockets live under `.tmp`,
so the checkout path must fit the platform's socket-path limit; the runner
reports a clear error if it is too long.

For an externally extracted PulseAudio package, set the server executable,
module directory, and (if needed) server-only library search path:

```bash
PULSEAUDIO_BIN=/path/to/pulseaudio/usr/bin/pulseaudio \
PULSEAUDIO_MODULE_DIR=/path/to/pulseaudio/usr/lib/pulseaudio/modules \
PULSEAUDIO_LIBRARY_PATH=/path/to/pulseaudio/usr/lib/pulseaudio:/path/to/pulseaudio/usr/lib/pulseaudio/modules \
bash scripts/test-cpp-playback.sh
```

An explicit command after `--` runs instead of the two default suites, using
the same private X11/audio environment. This can be used to launch an existing
nested Wayland check; the script does not provide or start a Wayland compositor.
Nested Wayland popup checks need input delivered by the compositor. Synthetic
pointer movement does not certify native compositor pointer input.

### Memory and undefined behavior checks

The Linux sanitizer preset instruments the application, native components and
Lexbor with AddressSanitizer and UndefinedBehaviorSanitizer. Qt, libmpv and the
system media libraries remain uninstrumented. Use GCC or Clang:

```bash
cmake --preset linux-sanitize
cmake --build --preset linux-sanitize --parallel 3
export MELEARNER_PLAYBACK_TIMEOUT_SECONDS=180
export ASAN_OPTIONS=halt_on_error=1
export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
bash scripts/test-cpp-playback.sh -- ctest --preset linux-sanitize --no-tests=error
bash scripts/test-cpp-playback.sh -- build/cpp-sanitize/playback_render_test \
  clearsUnloadedSurfaceToBlack repeatedlyLoadsAndClosesInEitherOrder playsWhileHidden \
  rendersSoftwareDecodedFrames:h264 rendersSoftwareDecodedFrames:hevc-main10 \
  rendersSoftwareDecodedFrames:multi-audio
bash scripts/test-cpp-playback.sh -- build/cpp-sanitize/main_playback_test
```

The private runner keeps these checks off the desktop and audio devices.
Leak detection stays enabled. Investigate reported allocations before adding
suppressions; a passing run covers only the paths exercised by the tests.
These playback commands select software decoding to separate application
reports from hardware-driver allocations. The default playback suite also
checks automatic hardware selection. Run it in the release build, then qualify
each supported GPU and driver separately on a private display.

To compare Mesa's software renderers, pass the driver to the test process:

```bash
bash scripts/test-cpp-playback.sh -- env GALLIUM_DRIVER=softpipe \
  build/cpp-sanitize/playback_render_test playsWhileHidden
```

Record the driver with the result. A pass with `softpipe` does not qualify
`llvmpipe` or a hardware driver.

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
the normal CTest run because it materializes a 100,000-lesson fixture. Run it
against a release build:

```bash
bash scripts/test-cpp-playback.sh -- build/cpp-release/library_load_test
```

It reports scan time, event-loop responsiveness, private resident memory,
startup and shutdown times. Course pages, search and the four-course resume
page each have a 200 ms response budget. The resume measurement includes three
requests and reports the slowest. These are local diagnostics, not a substitute
for testing the installed package on its supported hardware.

## Architecture

Qt owns the single application window, widgets, models, focus, keyboard input,
and accessibility. C++ modules own the current SQLite schema, course scan,
search, progress, documents, PDF rendering, and embedded libmpv player.
Tests use isolated temporary libraries. Application data paths are listed in
[Privacy](privacy-and-legal.md).

The Linux player uses libmpv in process and does not launch an external player
or codec helper. Media files are opened from the selected course root after
path validation. On `llvmpipe` or `softpipe`, libmpv uses its software render API
to render into a CPU image, which Qt presents in the `QOpenGLWidget`. This path
still needs a functioning Qt OpenGL context. Other OpenGL renderers keep the
direct libmpv OpenGL presentation path. The `--software-decoding` option
controls decoding separately: it skips hardware decoder probes, but does not
select the software render API. The transport is built from shadcn components,
but the decode and presentation are not: the component library's optional media
player takes a URL and owns its own transport, and this application needs to
drive a path it has already validated, seek to a saved position, add a subtitle
file and report progress.

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

The icon set lives in `cpp-app/assets/icons/hicolor`. Every installed size uses
the same monochrome book-and-play mark, rasterized at that size.

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
