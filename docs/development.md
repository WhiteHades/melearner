# Build from source

meLearner uses C++23 and Qt Widgets. Application code lives in `cpp-app/src`.

## Requirements

Install C and C++23 compilers, CMake 4.4 or newer, Ninja, pkg-config, Qt 6.11.2
or newer with Widgets, OpenGLWidgets, Network, Pdf, WebEngineWidgets, Concurrent
and Test, SQLite, libzip, md4c with md4c-html, and FFmpeg development libraries
for libavformat, libavcodec, libavutil and libswscale. Linux also needs Meson
1.3 or newer, patch, and the [Linux media development libraries](install.md#linux-from-source).
Use LuaJIT for the pinned Linux player;
its build does not support Lua 5.5. See [Windows builds](windows-development.md)
for the Windows toolchain. macOS builds require Xcode 26 or newer and target
Apple silicon with macOS 15 or newer.

The first configure downloads source archives verified with SHA-256 for Lexbor and
shadcn-cpp. Linux also builds pinned mpv 0.41.0 with an upstream PipeWire fix.
These dependencies retain their license notices. Linux package maintainers
must also include the complete dependency inventory and required source files.

The document viewer serves files from the selected course only. It blocks
network requests, downloads, popups and permissions. A document is limited to
32 MiB, with a 128 MiB cache and 512 requests per document. Linux packaging
bundles Qt WebEngine's helper, resources and locales. Windows packages include
the helper and resources. The macOS packager stages the renderer inside the app.

## Build and test

From the repository root:

```bash
cmake --preset linux-dev
cmake --build --preset linux-dev --parallel 4
ctest --preset linux-dev --no-tests=error
```

For a release build, replace `linux-dev` with `linux-release` in each command.
CTest runs the core window test with Qt's offscreen plugin. The playback
E2E test needs a private X11 display and PulseAudio null sink, provided
by the runner script. It runs both E2E tests:

```bash
bash scripts/test-cpp-playback.sh
bash scripts/test-cpp-playback.sh --build-dir build/cpp-release
```

The playback test also needs FFmpeg. It remuxes the checked-in clip to test
recovery, closing and resuming. The fixtures are project-authored H.264 and
HEVC clips. Regenerate them with FFmpeg 8.1.2 using
`bash scripts/generate-media-corpus.sh`; this replaces the three clips.

The runner starts separate Xvfb, D-Bus and PulseAudio sessions, uses a null
audio sink and Mesa software OpenGL, and stores temporary XDG data under
`.tmp/cpp-playback`. It leaves `HOME` unchanged. Logs remain in that run
directory. The default build directory is `build/cpp-dev`; use
`MELEARNER_BUILD_DIR` or `--build-dir` to select another. Tests time out after
90 seconds by default; set `MELEARNER_PLAYBACK_TIMEOUT_SECONDS` to change it.
Required tools include `pulseaudio`, Xvfb, `xvfb-run`, `xauth`, Openbox,
`dbus-daemon`, `pactl` and `timeout`. The checkout path must fit the platform's
Unix socket path limit.

Pass a command after `--` to run it in the same private environment instead of
the two default tests. For example, use this to run an existing nested Wayland
check. The runner does not start a Wayland compositor, and synthetic pointer
movement does not verify native compositor input.

To use an extracted PulseAudio package, set its executable and module paths:

```bash
PULSEAUDIO_BIN=/path/to/pulseaudio/usr/bin/pulseaudio \
PULSEAUDIO_MODULE_DIR=/path/to/pulseaudio/usr/lib/pulseaudio/modules \
PULSEAUDIO_LIBRARY_PATH=/path/to/pulseaudio/usr/lib/pulseaudio:/path/to/pulseaudio/usr/lib/pulseaudio/modules \
bash scripts/test-cpp-playback.sh
```

To save test screenshots, create a destination and set
`MELEARNER_TEST_SCREENSHOTS` for the test process:

```bash
mkdir -p .tmp/ui-captures
bash scripts/test-cpp-playback.sh -- env MELEARNER_TEST_SCREENSHOTS="$PWD/.tmp/ui-captures" \
  build/cpp-release/main_window_test
bash scripts/test-cpp-playback.sh -- env MELEARNER_TEST_SCREENSHOTS="$PWD/.tmp/ui-captures" \
  build/cpp-release/main_playback_test
```

Inspect screenshots; geometry checks do not prove visual correctness. The
optional `documentationShowcase` case creates sample courses and lesson slides
for README screenshots. Set `MELEARNER_SHOWCASE_DIR` to a private output
directory and run that case with the playback runner. It writes `home.png` and
`course.png` from the app and skips when the variable is unset.

### Sanitizers

The Linux sanitizer preset instruments the app, native components and Lexbor
with AddressSanitizer and UndefinedBehaviorSanitizer. Qt, libmpv and system
media libraries are not instrumented. Use GCC or Clang:

```bash
cmake --preset linux-sanitize
cmake --build --preset linux-sanitize --parallel 3
export MELEARNER_PLAYBACK_TIMEOUT_SECONDS=180
export ASAN_OPTIONS=halt_on_error=1
export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
bash scripts/test-cpp-playback.sh -- ctest --preset linux-sanitize --no-tests=error
bash scripts/test-cpp-playback.sh -- build/cpp-sanitize/main_playback_test
```

Leak detection stays enabled. A passing run covers only the paths exercised by
the tests.

## Architecture and compatibility

Qt owns the application window, widgets, input and accessibility. C++ modules
handle the SQLite library, course scanning, search, progress, documents, PDF
rendering and embedded libmpv playback. Application data paths are listed in
[Privacy](privacy-and-legal.md).

Linux playback uses the bundled in-process libmpv. It opens media only after
validating its path under the selected course root. On `llvmpipe` or
`softpipe`, it renders to a CPU image that Qt presents in a `QOpenGLWidget`;
this still requires a working Qt OpenGL context. Other renderers use direct
libmpv OpenGL presentation. `--software-decoding` selects software decoding,
not software rendering.

Release 0.1.9 is public. The native Linux, Windows and macOS core workflow
passes, and Windows playback passes. Linux AppImage and Arch packages require
glibc 2.39 or newer. The Windows EXE is unsigned. The Apple silicon macOS 15+
DMG is ad hoc signed, not notarized. These checks do not establish compatibility
with every distribution, graphics driver or clean machine.

## Install for the current user

The Linux source installer adds the app to `PATH`, registers a desktop entry
and installs its icons. It accepts an absolute prefix:

```bash
bash scripts/install-cpp-linux.sh "$HOME/.local"
```

The desktop entry passes the selected course folder to the app. To launch from
a terminal:

```bash
melearner ~/Courses
```
