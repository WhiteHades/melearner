# C++ application development

The application version is `0.1.9`. The CMake project version is the source for the C++ executable and package metadata; keep all release metadata aligned with it.

The C++23/Qt application is the only implementation. Linux installer, runtime, and legal qualification remain incomplete. Windows will be built and tested next on a Windows PC; macOS is deferred under ADR 0013.

## Build and test

Required development tools are a C++23 compiler, CMake 4.4+, Ninja, pkg-config, Qt 6.11.2 or newer Widgets/OpenGLWidgets/Network/Pdf/Test, SQLite, libmpv, libzip, and md4c. On Arch, Qt PDF is supplied by `qt6-webengine`; the app links only its native `Qt6Pdf` library, not WebEngine or QML. The other package names are `base-devel cmake ninja pkgconf qt6-base sqlite mpv libzip md4c`.

PDFium is used through Qt PDF's C++ API. PDF loading and 512-pixel clipped tile rendering run on the PDF worker; the visible tile cache holds at most 64 images. The Qt PDF/PDFium source versions, notices, and binary hashes still need to be locked for release packaging. See [Qt PDF licensing](https://doc.qt.io/qt-6/qtpdf-licensing.html).

The first configure downloads the hash-pinned Lexbor 3.0.0 source archive for HTML parsing. Application code does not use its script, CSS, or network facilities. Its Apache-2.0 license and notice are included during installation. Version 3.0.0 is newer than the 2.7.0 fixes documented for the upstream [encoder](https://github.com/lexbor/lexbor/security/advisories/GHSA-mrwr-xh7f-96v3) and [fragment-parser](https://github.com/lexbor/lexbor/security/advisories/GHSA-mrpr-v36q-2vp8) advisories; this is not a complete release dependency audit.

```sh
cmake --preset linux-dev
cmake --build --preset linux-dev --parallel 4
ctest --preset linux-dev
./build/cpp-dev/melearner
```

For the full-fixture Library load check, build `linux-release`, then run:

```sh
TMPDIR="$PWD/.tmp/cpp-tests" ./build/cpp-release/library_load_test
```

This optional diagnostic creates the existing 100,000-Lesson fixture, scans its 99,802 remaining files, and measures event-loop responsiveness, paging, search, private resident memory on Linux, shutdown, and reopening the indexed Library. It removes its temporary data on exit and runs separately from routine CTest. It does not qualify an installed package or measure video rendering.

Tests create isolated temporary Libraries. The app uses the distinct Qt application identity `WhiteHades/melearner-cpp-v1` and database `library-v1.sqlite3`. It does not read old app databases.

Normal playback uses libmpv's automatic hardware-decoder selection, with its software fallback. Run `melearner --software-decoding` to disable hardware decoding for troubleshooting or qualification. Settings → About reports the active decoder. The live render test checks the active decoder as well as changing video frames. See [mpv's decoding options](https://mpv.io/manual/stable/#options-hwdec).

## Local source installation

After installing the development prerequisites, this command builds, tests, and installs into `~/.local` without sudo:

```sh
bash scripts/install-cpp-linux.sh
```

An optional absolute argument selects another installation prefix. This source installer uses the host's native libraries; it is not the self-contained release package. AppImage, Arch private-runtime packaging, signatures, and installed-package acceptance remain separate release work.

The installer rejects extra arguments, empty or relative prefixes, and a nonempty `DESTDIR`; it is a direct source installer, not a package staging command. It checks for CTest and Qt Test before configuring, treats zero discovered tests as an error, and only reports success after installation produces an executable. Use the quoted command printed on success to launch from a custom prefix; application-menu visibility depends on the desktop session's search paths. These checks do not establish installed playback or package qualification.

To exercise installer success and failure handling without building the application:

```sh
python3 scripts/test-cpp-linux-installer.py
```

This requires Linux, Bash, and Python 3.9 or newer. Build tools and install destinations are isolated fixtures; when CTest is available, three cases also exercise real empty, failing, and passing CTest suites. A pass does not compile the Qt application, exercise live playback, or qualify release packages.

## Diagnostic packaging regression tests

The diagnostic archive is not an AppImage or an Arch package. Its metadata keeps `releaseQualified` false. The packager reads the configured version directly from the CMake cache, validates the complete archive listing, rejects `DESTDIR`, and publishes without replacing an existing or concurrently created destination. Temporary staging stays on the output filesystem and is removed on exit.

```sh
python3 scripts/test-cpp-linux-archive.py
python3 scripts/test-cpp-linux-runtime.py
bash scripts/test-cpp-linux-packaging.sh
```

The Python tests require Linux, Python 3.10 or newer, CMake 3.20 or newer, a C compiler, GNU tar with zstd, and readelf. The archive tests configure a real disposable CMake project and use real tar/zstd, but substitute the application staging step. The runtime tests compile small ELF libraries and a loader using the production RPATH values, move the package directory, and load the plugin without `LD_LIBRARY_PATH`. They also run the production ELF audit function against real dependency records and use patchelf 0.19.1 or newer on an installed ELF fixture to guard against GNU hash corruption. They do not run the complete Qt runtime stager or an installed melearner app. The shell preflight test requires the production CMake 4.4+ and packaging prerequisites.

The C++ Arch package path uses `bash scripts/package-cpp-arch.sh --build-dir build/cpp-release` and the existing CMake runtime stager; its isolated `makepkg` regression is `python3 scripts/test-cpp-arch-packaging.py`. The packager refuses to run `makepkg` unless the staged tree contains the exact `0.1.9` runtime metadata, `/usr/bin/melearner` launcher, absolute `/usr/bin/melearner` desktop `Exec`, no superseded browser runtime, and `LICENSE`, `THIRD_PARTY_NOTICES`, `melearner.spdx.json`, `runtime-lock.json`, and `reference-profiles-v1.json`. The repository currently lacks the notices and SPDX inputs, so this path remains blocked and does not qualify a release.
