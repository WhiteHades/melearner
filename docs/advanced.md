# Advanced setup

## Install and build

[Linux source installation](install.md) · [Development guide](development.md) · [Usage](usage.md)

The source installer needs development libraries. It is not a ready made desktop download.

## Package formats

Windows gets an EXE installer. macOS gets a DMG containing the app. Linux gets an AppImage as the main download, with an Arch package for Arch and Omarchy. A Linux archive is useful for diagnostics, not as the beginner download.

AppImage is a practical default because it carries the app and required libraries in one file for supported Linux distributions. See the [AppImage documentation](https://docs.appimage.org/introduction/index.html).

Only older Linux binaries are published today. Native builds and core workflow tests pass on Linux, Windows and macOS. The manual workflows can also prepare diagnostic installers. See [Manual builds](ci.md) for their checks and limits. Do not label a build artifact as a public release.

## Contained runtime

Release packages must bundle the player, document renderer, required libraries, fonts and resources. Users should not need a separate browser, database server or codec pack. Course files and saved progress remain on the user's device.

Progress uses SQLite inside the app. It needs no server or separate setup.

The operating system still provides its kernel, graphics driver and desktop services. An AppImage does not make an incompatible operating system compatible. Test each advertised system on a clean machine without development packages or the build directory.

Build Linux releases against the oldest supported system runtime. Check the required runtime versions in every bundled library, not just the app executable. A bundle built on a newer system can still fail on an older desktop. Do not copy glibc into the app as a compatibility workaround.

The C++ AppImage packager rejects bundled ELF files requiring newer than glibc 2.39, matching the Ubuntu 24.04 CI baseline. This floor is necessary, not a universal portability guarantee; graphics drivers, desktop services and distribution differences still matter.

## Size without feature cuts

Use optimized release builds, remove unused symbols from packaged binaries, keep one copy of each runtime file and compress the installer. Keep codecs, document readers, language resources, accessibility and license notices. Measure the final package and retest playback and documents after each change.

The Linux packaging scripts share a dependency inventory and validator. Diagnostic packages are not public releases. Publishing is authorized, but each offered download must have its runtime dependencies, source records and required notices in place.

When a Qt provider omits its Chromium notice, the Linux notice collector retrieves
the matching Qt source archive and verifies its checksum. The runtime stager can
use that collected notice through its legal input directory.

The Windows and macOS downloads will have no trusted publisher signature. Windows may show a SmartScreen warning. macOS may require the user to approve opening the app in Privacy & Security. Do not disable system security protections globally.

## Showcase images

The existing playback suite has an optional documentationShowcase case. Set MELEARNER_SHOWCASE_DIR to a private output directory and run it with the private playback runner described in the development guide. It creates sample courses and original lesson slides, then captures the real home and course windows. No personal courses or progress are included.
