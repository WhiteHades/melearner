# Windows development

Build with native x64 MSVC 2022 and the official Qt 6.11.2 MSVC SDK. Qt WebEngine does not support MinGW. See the [Windows workflow file](../.github/workflows/windows-build.yml) for exact SDK archives, dependency versions and setup commands.

The workflow builds libmpv from verified source and applies the generated `mpv-msvc-resources.patch`. Package evidence includes the source archive, patch, build options and notices. The optional `package` input creates an unsigned diagnostic installer, not a release.

The [0.1.1 player sources](../packaging/sources/melearner-0.1.1-windows-sources.tar.gz) include the exact archives, patch and build manifest used for the Windows download.

For local packaging, provide the player DLL with `MPV_DLL`, or pass its folder with `-MpvBin` to `scripts/package-cpp-windows.ps1`. The packager collects the app, Qt plugins, WebEngine files, media and database libraries, notices and x64 compiler runtime DLLs.

Verify installation, playback, documents, saved progress, restart and removal on a clean Windows machine. The workflow does not prove every clean machine works. Its unsigned artifact may show a publisher warning. See [Qt WebEngine requirements](https://doc.qt.io/qt-6.11/qtwebengine-platform-notes.html#windows) and [Qt Windows deployment](https://doc.qt.io/qt-6.11/windows-deployment.html).
