# Windows development

The Windows EXE installer is planned, not available. A native Windows build and
clean machine playback and document tests are still required.

## Required toolchain

The current app requires Qt 6.11.2 WebEngine for HTML and Markdown. Qt WebEngine
does not build with MinGW, so the previous MSYS2 UCRT64 instructions do not cover
the current app. See [Qt WebEngine platform requirements](https://doc.qt.io/qt-6.11/qtwebengine-platform-notes.html#windows).

Prepare a native MSVC environment with a matching Qt kit and compatible builds
of SQLite, libmpv, libzip, md4c and FFmpeg. CMake also consumes the pinned Lexbor
and shadcn sources. The [manual Windows workflow](ci.md) installs a pinned native
toolchain and builds the required libraries. It downloads the official Qt 6.11.2
MSVC SDK, including PDF and WebEngine, with pinned SHA256 checks. Qt is not
compiled from source. The SDK archives are cached separately before building the
remaining libraries. The SDK archive cache has a 1 GiB cap and each build has a
one hour limit. It does not publish an installer. The legacy UCRT64 script checks
for the required WebEngine module before configuring; it is not a qualified
Windows build path.

The video renderer links the Windows system OpenGL import library explicitly.
The official Qt kit resolves its own OpenGL calls dynamically and does not
provide that import library for the app's direct desktop OpenGL calls.

The app and its remaining build tools use the same native Release configuration.
FFmpeg includes the file reading, video decoding and image scaling libraries
used for thumbnails. Playback uses the separate pinned libmpv SDK. vcpkg does
not build Qt SQL drivers or PostgreSQL libraries that the application never uses.
The pinned player DLL also imports vulkan-1.dll. The manual workflow builds the
Vulkan loader from the pinned vcpkg baseline and deploys it beside the test app,
then checks application and local DLL imports before starting the test. A missing
loader must not silently depend on an optional graphics driver installation.
Completed dependency archives are also saved if a later dependency fails, within
the 4 GiB cache cap. The next manual run restores them instead of rebuilding
everything. A complete dependency cache takes priority over partial caches.
The workflow uses the runner's larger work drive for vcpkg and removes completed
build trees and staging packages between dependencies. Installed libraries,
downloaded sources, build logs and binary archives remain available.

Choose a folder on a local drive. URLs, network shares and symlinked paths are
not accepted as course roots.

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
