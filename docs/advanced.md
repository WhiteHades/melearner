# Advanced setup

## Install and build

[Linux source installation](install.md) · [Development guide](development.md) · [Usage](usage.md)

The source installer needs development libraries. It is not a ready made desktop download.

## Package formats

Windows gets an EXE installer. macOS gets a DMG containing the app. Linux gets an AppImage as the main download, with an Arch package for Arch and Omarchy. A Linux archive is useful for diagnostics, not as the beginner download.

AppImage is a practical default because it carries the app and required libraries in one file for supported Linux distributions. See the [AppImage documentation](https://docs.appimage.org/introduction/index.html).

Only older Linux binaries are published today. The current app still needs package qualification. Windows and macOS need native builds and tests before their installers can be offered. Do not label a planned installer as available.

## Contained runtime

Release packages must bundle the player, document renderer, required libraries, fonts and resources. Users should not need a separate browser, database server or codec pack. Course files and saved progress remain on the user's device.

Progress uses SQLite inside the app. It needs no server or separate setup.

The operating system still provides its kernel, graphics driver and desktop services. An AppImage does not make an incompatible operating system compatible. Test each advertised system on a clean machine without development packages or the build directory.

## Size without feature cuts

Use optimized release builds, remove unused symbols from packaged binaries, keep one copy of each runtime file and compress the installer. Keep codecs, document readers, language resources, accessibility and license notices. Measure the final package and retest playback and documents after each change.

The Linux packaging scripts share a dependency inventory and validator. Private diagnostic packages are not public releases. Publishing stays on hold until the installed app and its packages pass acceptance.

## Showcase images

The existing playback suite has an optional documentationShowcase case. Set MELEARNER_SHOWCASE_DIR to a private output directory and run it with the private playback runner described in the development guide. It creates sample courses and original lesson slides, then captures the real home and course windows. No personal courses or progress are included.
