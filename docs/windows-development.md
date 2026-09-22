# Windows development

Windows release qualification is deferred until the application is built and tested on a native Windows machine. macOS is deferred separately. This handoff prepares the C++23 and Qt 6.11.2 application at version `0.1.9`; it does not claim Windows qualification or provide a release package.

## Toolchain

Use the MSYS2 UCRT64 environment. MSYS2 recommends UCRT64 for new 64-bit builds, and Qt 6.11 supports Windows 10 and Windows 11 x86_64 with MinGW-w64. The project uses CMake, Ninja, Qt Widgets, Qt OpenGL Widgets, Qt Network, Qt PDF, SQLite, libmpv, libzip, md4c, and the hash-pinned Lexbor FetchContent archive.

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

The entrypoint configures `build/cpp-windows` with the native UCRT64 compiler and Ninja, builds with CMake, and runs every test registered by CMake. The registered Qt UI tests receive `QT_QPA_PLATFORM=offscreen` from CMake. The `playback_render_test` and `main_playback_test` binaries are build targets only, not CTest registrations, so the default command does not launch them or take over the desktop. Increase or limit parallelism with `--jobs`, for example `bash scripts/build-cpp-windows.sh --jobs 8`. The `MELEARNER_BUILD_JOBS` environment variable accepts the same positive integer value.

The executable is `build/cpp-windows/melearner.exe`. Check the unified version before opening the window:

```sh
./build/cpp-windows/melearner.exe --version
```

When a visible OpenGL playback run is acceptable, use:

```sh
bash scripts/build-cpp-windows.sh --run-playback
```

After the registered CTest suite passes, that mode launches both `playback_render_test.exe` and `main_playback_test.exe` from the build directory with the normal Qt Windows platform. It is not headless, may take over the desktop, and is not required for the default build check.

The entrypoint control flow can be checked on a non-Windows machine without a compiler, Qt installation, or desktop by running:

```sh
python3 scripts/test-cpp-windows-build.py
```

This regression uses disposable command stubs. It checks environment and job validation, build failure ordering, the default no-playback path, and the explicit playback target launch.

## Confirmed boundaries and follow-up

- The Linux source installer, Linux runtime stager, Arch package, and diagnostic archive are Linux-only. They must not be used as Windows packaging commands.
- The first configure downloads Lexbor 3.0.0 from its pinned URL and verifies its SHA-256 hash, so the first configure needs network access. Runtime behavior remains local only.
- Windows packaging is still pending. Use Qt's `windeployqt` after a successful native build, then stage the MinGW UCRT runtime, libmpv, Qt PDF, and their transitive DLLs. Do not publish an installer from this handoff.
- The Linux-only secure file-handle path is guarded by `Q_OS_LINUX`; Windows currently uses Qt file reopening after validation. Review that race boundary on Windows before release qualification.
- `local_files.cpp` builds root containment with `QDir::separator()`. Validate drive roots, mixed separators, Unicode paths, and paths with spaces on Windows before release qualification.
- Marker publication uses `std::filesystem::create_hard_link`. Validate same-volume temporary paths and the warning behavior when the temporary directory and course root are on different volumes.
- The source was not compiled or executed on Windows in this environment. The native Windows machine must record compiler, CMake, Qt, package, CTest, and playback results before a release is considered.

No CI workflow or release asset is changed by this handoff.
