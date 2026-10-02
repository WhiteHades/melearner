# Windows builds

These commands build the C++23 and Qt application on Windows. Windows support is experimental; a tested Windows package is not yet available.

## Toolchain

Use the MSYS2 UCRT64 environment. MSYS2 recommends UCRT64 for new 64-bit builds, and Qt 6.11 supports Windows 10 and Windows 11 x86_64 with MinGW-w64. The project uses CMake, Ninja, Qt Widgets, Qt OpenGL Widgets, Qt Network, Qt PDF, SQLite, libmpv, libzip, md4c, and hash-pinned Lexbor and shadcn-cpp source archives.

Windows uses the UCRT64 libmpv package. The separate pinned Linux player build
and its Meson, PipeWire and Linux graphics requirements do not apply on Windows.
The same native shadcn-cpp components, dark theme and Geist font are built on both
platforms. Windows runtime and packaging verification remain pending.

References:

- [MSYS2 environments](https://www.msys2.org/docs/environments/)
- [MSYS2 package naming](https://www.msys2.org/docs/package-naming/)
- [MSYS2 pkg-config](https://www.msys2.org/docs/pkgconfig/)
- [Qt for Windows](https://doc.qt.io/qt-6/windows.html)
- [Qt Windows deployment](https://doc.qt.io/qt-6/windows-deployment.html)

Install MSYS2 from [msys2.org](https://www.msys2.org/), open the **MSYS2 UCRT64** terminal, update it, then install the native toolchain and dependencies:

```sh
pacman -Syu
# If MSYS2 asks the terminal to close, close it, reopen MSYS2 UCRT64, and run:
pacman -Su
pacman -S --needed \
  git \
  mingw-w64-ucrt-x86_64-toolchain \
  mingw-w64-ucrt-x86_64-cmake \
  mingw-w64-ucrt-x86_64-ninja \
  mingw-w64-ucrt-x86_64-pkgconf \
  mingw-w64-ucrt-x86_64-qt6-base \
  mingw-w64-ucrt-x86_64-qt6-pdf \
  mingw-w64-ucrt-x86_64-sqlite3 \
  mingw-w64-ucrt-x86_64-mpv \
  mingw-w64-ucrt-x86_64-libzip \
  mingw-w64-ucrt-x86_64-md4c
```

The package index currently exposes the required UCRT64 packages for [Qt base](https://packages.msys2.org/packages/mingw-w64-ucrt-x86_64-qt6-base), [Qt PDF](https://packages.msys2.org/packages/mingw-w64-ucrt-x86_64-qt6-pdf), [mpv](https://packages.msys2.org/packages/mingw-w64-ucrt-x86_64-mpv), [SQLite](https://packages.msys2.org/packages/mingw-w64-ucrt-x86_64-sqlite3), [libzip](https://packages.msys2.org/packages/mingw-w64-ucrt-x86_64-libzip), and [md4c](https://packages.msys2.org/packages/mingw-w64-ucrt-x86_64-md4c). Package versions are intentionally not hard-coded here.

## Configure, build, and test

From the repository root in the same UCRT64 terminal:

```sh
git checkout main
bash scripts/build-cpp-windows.sh
```

The repository keeps shell scripts and the embedded SQLite schema in LF format
on Windows too. Git's line-ending settings will not change those files.

The entrypoint configures `build/cpp-windows` with the native UCRT64 compiler and Ninja, builds with CMake, then runs the `main_window_test` end-to-end test through CTest. CMake sets `QT_QPA_PLATFORM=offscreen` for that test. Increase or limit parallelism with `--jobs`, for example `bash scripts/build-cpp-windows.sh --jobs 8`. The `MELEARNER_BUILD_JOBS` environment variable accepts the same positive integer value.

The executable is `build/cpp-windows/melearner.exe`. Check the unified version before opening the window:

```sh
./build/cpp-windows/melearner.exe --version
```

When a visible OpenGL playback run is acceptable, use:

```sh
pacman -S --needed mingw-w64-ucrt-x86_64-ffmpeg
bash scripts/build-cpp-windows.sh --run-playback
```

After the CTest check passes, that mode launches `main_playback_test.exe` from the build directory with the normal Qt Windows platform. It is not headless, may take over the desktop, and is not required for the default build check.

The recovery check uses the FFmpeg command-line tool to prepare temporary test
media. With `--run-playback`, the script checks for that tool before configuring
or building. The default build does not require it.

## Packaging

- The Linux source installer and Linux packaging scripts do not create Windows packages.
- The first configure downloads Lexbor 3.0.0 and shadcn-cpp from pinned URLs and verifies their SHA-256 hashes, so the first configure needs network access. Runtime behavior remains local only.
- Use Qt's `windeployqt` after a successful native build, then include the MinGW UCRT runtime, libmpv, Qt PDF, and their required DLLs with the application. A Windows packaging script is not yet provided.
