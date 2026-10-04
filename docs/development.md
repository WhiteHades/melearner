# Build from source

meLearner uses C++23 and Qt Widgets. Code is in `cpp-app/src`.
See [installation notes](install.md) for OS support and signing. Windows needs
its own toolchain; see [Windows development](windows-development.md).

## Requirements

Install a C and C++ compiler, CMake 4.4 or newer, Ninja, pkg-config, Qt 6.11.2
or newer, SQLite with FTS5, libzip, md4c with md4c-html, and FFmpeg development
libraries for libavformat, libavcodec, libavutil and libswscale. Qt needs
Widgets, OpenGLWidgets, Network, Pdf, WebEngineWidgets, Concurrent and Test.
Linux also needs Meson 1.3 or newer, patch, LuaJIT and the
[Linux media development libraries](install.md#linux-from-source). The pinned
Linux player does not build with Lua 5.5. macOS builds need Xcode 26 or newer
and target Apple silicon with macOS 15 or newer.

Configure downloads Lexbor and shadcn-cpp source archives, then checks their
SHA-256 hashes. Linux builds mpv 0.41.0 with an upstream PipeWire fix. Keep
license notices and source files when packaging.

## Build and test

Run these commands from the repository root:

```bash
cmake --preset linux-dev
cmake --build --preset linux-dev --parallel 4
ctest --preset linux-dev --no-tests=error
```

Use `linux-release` instead of `linux-dev` for a release build. CTest runs the
core window test with Qt's offscreen plugin. To run both app E2E tests, use
the private environment script:

```bash
bash scripts/test-cpp-playback.sh
bash scripts/test-cpp-playback.sh --build-dir build/cpp-release
```

The script starts private Xvfb, D-Bus and PulseAudio sessions with a null audio
sink and Mesa software OpenGL. It stores temporary XDG data and logs in
`.tmp/cpp-playback`, and leaves `HOME` unchanged. It needs `pulseaudio`, Xvfb,
`xvfb-run`, `xauth`, Openbox, `dbus-daemon`, `pactl` and `timeout`. The checkout
path must fit the Unix socket path limit. The default build directory is
`build/cpp-dev`; set `MELEARNER_BUILD_DIR` or pass `--build-dir` to change it.
Each test has a 90 second limit, configurable with
`MELEARNER_PLAYBACK_TIMEOUT_SECONDS`. Playback tests need FFmpeg and use the
checked in H.264 and HEVC clips. The script accepts a command after `--` to
run it in this environment instead of its two default tests.

The runner does not start a Wayland compositor. Synthetic pointer movement
does not test native compositor input. To save screenshots, create a directory
and set `MELEARNER_TEST_SCREENSHOTS` for the test process. For example:

```bash
mkdir -p .tmp/ui-captures
bash scripts/test-cpp-playback.sh -- env MELEARNER_TEST_SCREENSHOTS="$PWD/.tmp/ui-captures" build/cpp-release/main_playback_test
```

The optional `documentationShowcase` test creates sample courses and lesson
slides. Set `MELEARNER_SHOWCASE_DIR` to a private directory and pass
`documentationShowcase` after the test executable to save `home.png` and
`course.png`. For example:

```bash
MELEARNER_SHOWCASE_DIR="$PWD/.tmp/showcase" bash scripts/test-cpp-playback.sh -- build/cpp-release/main_playback_test documentationShowcase
```

Geometry checks do not confirm visual quality.

## Sanitizers

The Linux sanitizer preset uses AddressSanitizer and UndefinedBehaviorSanitizer
for the app, native components and Lexbor. Qt, libmpv and system media
libraries are not instrumented. Use GCC or Clang:

```bash
cmake --preset linux-sanitize
cmake --build --preset linux-sanitize --parallel 3
export MELEARNER_PLAYBACK_TIMEOUT_SECONDS=180
export ASAN_OPTIONS=halt_on_error=1
export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
bash scripts/test-cpp-playback.sh -- ctest --preset linux-sanitize --no-tests=error
bash scripts/test-cpp-playback.sh -- build/cpp-sanitize/main_playback_test
```

Leak checks stay enabled. A passing run only covers code reached by its tests.

## Install for your account

The Linux source installer adds the app to `PATH`, registers a desktop entry
and installs icons. Give it an absolute prefix:

```bash
bash scripts/install-cpp-linux.sh "$HOME/.local"
melearner ~/Courses
```

The desktop entry opens the selected course folder. The document viewer only
serves files from that course. It blocks network access, downloads, popups and
permissions. Each document is limited to 32 MiB, with a 128 MiB cache and 512
requests.
