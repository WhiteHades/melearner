# Windows development

The Windows EXE installer is planned, not available. A native Windows build and
clean machine playback and document tests are still required.

## Required toolchain

The current app requires Qt 6.11.2 WebEngine for HTML and Markdown. Qt WebEngine
does not build with MinGW, so the previous MSYS2 UCRT64 instructions do not cover
the current app. See [Qt WebEngine platform requirements](https://doc.qt.io/qt-6.11/qtwebengine-platform-notes.html#windows).

Prepare a native MSVC environment with a matching Qt kit and compatible builds
of SQLite, libmpv, libzip, md4c and FFmpeg. CMake also consumes the pinned Lexbor
and shadcn sources. The MSVC build entrypoint and dependency setup remain to be
implemented and verified on Windows. The legacy UCRT64 script checks for the
required WebEngine module before configuring; it is not a qualified Windows
build path.

## Contained installer

The EXE installer must include the application, Qt libraries and plugins,
WebEngine helper and resources, required media and database libraries, fonts
and license notices. Use windeployqt to collect Qt dependencies, then inspect
the remaining DLL dependencies separately. Include the official compiler
redistributable in the installer rather than asking users to install it.
See [Qt Windows deployment](https://doc.qt.io/qt-6.11/windows-deployment.html).

Verify on a Windows machine without Qt, MSYS2, a separate player or a database
server. Test installation, playback, documents, saved progress, restart and
uninstallation before offering the EXE download. Do not copy the developer
machine's PATH into the installer or claim a Linux cross build qualifies Windows.
