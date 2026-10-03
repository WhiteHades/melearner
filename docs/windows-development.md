# Windows development

The manual workflow builds the app with native MSVC. Its optional `package` input
also builds an unsigned EXE installer. A workflow artifact is not a published release.

## Required toolchain

The current app requires Qt 6.11.2 WebEngine for HTML and Markdown. Use the MSVC
Qt kit. Qt WebEngine does not support MinGW.
See [Qt WebEngine platform requirements](https://doc.qt.io/qt-6.11/qtwebengine-platform-notes.html#windows).

Prepare a native MSVC environment with a matching Qt kit and compatible builds
of SQLite, libmpv, libzip, md4c and FFmpeg. CMake also consumes the pinned Lexbor
and shadcn sources. SQLite must include FTS5 because the library schema uses it
for course and lesson search. The workflow explicitly installs `sqlite3[fts5]`;
the default vcpkg SQLite build does not include that feature.
The [manual Windows workflow](ci.md) installs a pinned native
toolchain and builds the required libraries. It downloads the official Qt 6.11.2
MSVC SDK, including PDF and WebEngine, with pinned SHA256 checks. Qt is not
compiled from source. The SDK archives are cached separately before building the
remaining libraries. The SDK archive cache has a 1 GiB cap and each build has a
one hour limit. Use that workflow for the current Windows build. It does not
publish releases automatically. Its optional `run_playback` input runs the existing video,
controls, layout and saved progress checks with software OpenGL. Leave it off
unless playback verification is needed.

The video renderer resolves OpenGL calls through the active Qt context. Qt can
select the system driver or its bundled software renderer without mixing the
two implementations. See [Qt Windows graphics](https://doc.qt.io/qt-6/windows-graphics.html).

The app and its remaining build tools use the same native Release configuration.
FFmpeg includes the file reading, video decoding and image scaling libraries
used for thumbnails and playback. The workflow builds libmpv from its verified
0.41.0 source archive and libplacebo from an immutable source revision. Their
original notices, build options and corresponding sources accompany the package
evidence. It does not use an opaque prebuilt player DLL. vcpkg does
not build Qt SQL drivers or PostgreSQL libraries that the application never uses.
The manual workflow builds the
Vulkan loader from the pinned vcpkg baseline and deploys it beside the test app,
then checks application and local DLL imports before starting the test. A missing
loader must not silently depend on an optional graphics driver installation.
The workflow also probes native DLL loading and sends Qt startup logs to stderr.
Windows system error dialogs are suppressed in CI so loader errors cannot wait
for an unseen confirmation button.
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
runtime libraries in the installer rather than asking users to install them.
The packager copies the x64 CRT DLLs from the active MSVC developer shell's
`VCToolsRedistDir`. It does not run `vc_redist.exe` or request administrator access.
GPS positioning plugins are not deployed. The local document viewer does not use
them. All remaining DLL imports must resolve within the bundle or Windows itself.
See [Qt Windows deployment](https://doc.qt.io/qt-6.11/windows-deployment.html).

`scripts/package-cpp-windows.ps1` normalizes Windows cache line endings before
validating the Release configuration and source directory. Set `MPV_DLL` to the player DLL or pass `MpvBin` as the directory
containing it. The downloaded SDK stores that DLL at its root.
Unsigned installers can show a Windows publisher warning.

Verify on a Windows machine without Qt, MSYS2, a separate player or a database
server. Test installation, playback, documents, saved progress, restart and
uninstallation before offering the EXE download. Do not copy the developer
machine's PATH into the installer or claim a Linux cross build qualifies Windows.
