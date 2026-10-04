# Manual platform builds

The native Linux, Windows and macOS build workflows run only when dispatched
from the repository's Actions tab. Select a platform workflow and choose Run
workflow. They do not run on every push or pull request. A separate Linux
tooling and application check workflow runs on pushes and pull requests.

Each native workflow builds the app and runs its core course and document
workflow. Start the platform affected by a change and inspect its logs. The
optional `package` input creates a diagnostic installer and retains it for
three days. Packaging is off by default. A successful build does not certify
playback, graphics drivers or installer behavior on a user's machine.

The macOS UI test uses the runner's display because Qt's offscreen plugin cannot
provide window activation and a graphics context. The Windows workflow has an
optional `run_playback` input. Enable it for Windows playback changes. It checks
H.264, HEVC, multiple audio tracks, saved progress and enlarged text with Qt's
software OpenGL renderer. The runner needs enough display space for the wide
layout checks. The test has a three minute limit. Neither platform test replaces
checks on a clean machine.

## Package checks

Linux packaging runs on Ubuntu 24.04. The AppImage bundles the document
renderer and player runtime, but not glibc. It requires glibc 2.39 or newer.
The Arch package is built from the validated AppImage tree and has the same
runtime requirement.

The Windows package is a contained per user EXE. It includes the compiler
runtime, WebEngine helper and resources. It is unsigned.

The macOS package is an Apple silicon DMG for macOS 15 or newer. It includes
dependency notices and is ad hoc signed, but it is not notarized. No Intel or
universal package is produced.

Package checks collect notices and source provenance for bundled dependencies.
Do not publish a package with missing required notices or source records.
Artifacts from build workflows are diagnostic until the package checks and
dependency evidence are complete.

## Draft release delivery

The manual `Draft release delivery` workflow accepts successful Linux, Windows
and macOS build run IDs. It validates their workflow, result and source tree,
then reuses their artifacts. It does not rebuild the native packages. Artifacts
expire after three days.

The workflow checks the Windows and macOS SHA-256 files, calculates Linux and
Arch checksums, extracts the AppImage without launching it, and builds the Arch
package from its `usr` tree. It assembles the four installers, checksums,
provenance and the Windows player source archive, patch and manifest into the
fixed `v0.1.9` draft release. It refuses to modify a published release or
overwrite existing assets. Review the assets, dependency evidence and platform
limits before publishing manually. The Windows EXE is unsigned; the macOS DMG
is ad hoc signed and not notarized.
