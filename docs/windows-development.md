# Windows development

Build with native MSVC using the [manual Windows workflow](ci.md). Its optional
`package` input creates an unsigned EXE artifact, not a release.

## Required toolchain

Use the x64 MSVC 2022 developer environment, CMake 4.4.4 and the official Qt
6.11.2 MSVC SDK with WebEngine and PDF. Qt WebEngine does not support MinGW.
The workflow pins vcpkg at `3cbc1db4d867ec83c89fba4c461321c11f78b5e3` and
builds the `x64-windows-release` triplet. It installs `sqlite3[fts5]`, libzip,
md4c, FFmpeg, libass, LuaJIT, vulkan-loader and pkgconf. See [Qt WebEngine
requirements](https://doc.qt.io/qt-6.11/qtwebengine-platform-notes.html#windows)
and the [workflow file](../.github/workflows/windows-build.yml) for setup commands.

The workflow builds libmpv 0.41.0 from verified source with LuaJIT enabled,
using Python 3, Meson 1.9.1, Ninja 1.13.0 and the Windows SDK resource compiler.
It applies `mpv-msvc-resources.patch` to adapt mpv's resource compiler codepage
option. Package evidence includes the source archive, patch, notices and build
options, plus sources and notices for libplacebo and other bundled dependencies.

## Contained installer

The installer bundles the app, Qt plugins, WebEngine helper and resources,
media and database libraries, fonts, notices and x64 compiler runtime DLLs.
The packager copies the runtime from `VCToolsRedistDir`. Users do not need to
install a separate runtime or have administrator access. All DLL imports must
resolve from the bundle or Windows itself. See [Qt Windows deployment](https://doc.qt.io/qt-6.11/windows-deployment.html).

For local packaging, set `MPV_DLL` to the player DLL or pass `MpvBin` as its
directory. The installer is unsigned, so Windows may show a publisher warning.

Test installation, playback, documents, saved progress, restart and
uninstallation on Windows without Qt, MSYS2, a separate player or database
server. Do not copy the developer machine's PATH into the installer.
