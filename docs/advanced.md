# Advanced setup

[Install](install.md) · [Build from source](development.md) · [Platform builds](ci.md)

## Runtime and compatibility

Desktop downloads bundle the player, document renderer, SQLite, fonts and required libraries. Course files and saved progress stay on your computer. Source builds need the development packages listed in the installation guide.

Linux requires x86_64 and glibc 2.39 or newer. Windows requires x64. macOS requires Apple silicon and macOS 15 or newer. The operating system supplies graphics drivers and desktop services. See [platform checks](ci.md) for what has been tested. Every distribution, driver and clean machine installation has not been verified.

The Windows installer is unsigned. The macOS app has an ad hoc signature and is not notarized. Your system may ask for approval. Do not disable system security protections globally.

## Packaging

Build Linux packages against the oldest supported runtime. The AppImage packager checks every bundled ELF file against the glibc 2.39 limit. Bundling glibc is not a compatibility fix.

Use release builds, strip unused symbols from copied binaries, keep one copy of each runtime file and compress installers. Retain codecs, document readers, accessibility resources and license notices. Measure package size and check playback and documents after changing a bundle.

Release assets include checksums and source revisions. Packages include dependency notices and source records. The Windows player source bundle also contains its build patch and manifest.

For README screenshots, use the `documentationShowcase` case described in the [development guide](development.md). It captures the app with sample courses rather than personal files.
