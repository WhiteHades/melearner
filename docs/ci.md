# Manual builds

Open the repository's Actions tab, choose a platform workflow and select Run workflow.

Builds run on native GitHub machines for Linux, Windows and macOS. They do not run on pushes or pull requests. Start only the platform affected by a change, then inspect its build and test logs. Fix a failure before starting another run.

The workflows compile the app and its existing test executables, then run the core course library and document workflow. Enable the optional `package` input to create an installer and retain its diagnostic artifact for three days. Packaging is off by default. Workflows never publish a release automatically. A passing build does not certify video playback, graphics drivers or installer compatibility on users' machines.
The macOS UI test uses the runner's native display. The offscreen Qt plugin cannot provide its window activation and graphics context.
UI tests focus a field before typing and wait for queued commands to complete, rather than assuming that closing a dialog also completes its action.
Document fixtures use `.lnk` shortcuts on Windows and symbolic links on Unix.
The viewer must reject either type when it points outside the course folder.

The Windows workflow has an optional `run_playback` input, disabled by default.
Enable it when Windows video changes need verification. It runs the existing
playback and saved progress checks for H.264, HEVC, multiple audio tracks and
enlarged text, using Qt's software OpenGL renderer. The runner display must fit
the wide layout checks. The display preflight fails before dependency setup if
the runner cannot provide that space. Playback has a three minute process limit.
This check does not replace clean machine installer or graphics driver testing.

Dependency caches reduce repeated downloads and compilation. Save them before the application build so an application failure does not discard the dependencies. Use standard runners and keep cache storage within the included allowance. Do not enable paid runners or raise storage limits without approval.

Linux builds target an Ubuntu baseline. macOS builds target Apple silicon and use Xcode 26 for standard C++ thread support. Windows builds use the native Microsoft toolchain. Packaging checks runtime deployment on each platform. Clean machine installer lifecycle and broad graphics driver compatibility are not covered by these workflows.

## Installer builds

Linux packaging uses Ubuntu 24.04 and checks every bundled ELF file against the glibc 2.39 ceiling. The AppImage includes the document renderer and private player runtime. It does not bundle glibc.

macOS packaging creates an Apple silicon DMG with an Applications shortcut. It checks the bundle's library paths, includes original dependency notices, and records each Homebrew receipt and formula recipe. Source provenance is identified by an archive or standalone PEM SHA-256, or an immutable 40-character Git commit; source-fallback files are copied only after verifying that identity. The pinned curl CA bundle is retained as its original Mozilla-attributed PEM, not unpacked as an archive. Notice collection recognizes both `LICENSE` and `LICENCE` spellings. Missing notices for core runtime dependencies fail packaging, and source-format failures report the affected formula and URL. The DMG has an ad hoc signature only, with no trusted publisher signature or notarization. The current workflow does not produce an Intel or universal app.

Windows packaging uses Inno Setup for a contained, per user EXE. It checks runtime DLL imports and includes the compiler runtime, WebEngine helper and resources. It has no publisher signature.

Artifacts remain diagnostic until the package checks and corresponding dependency source and notice records are complete. Their presence in Actions is not a public release announcement. Review the generated evidence and missing source records before publishing; do not replace missing records with empty manifests.

## Draft release delivery

The manual `Draft release delivery` workflow accepts the successful Linux, Windows, and macOS build run IDs. It only downloads the named diagnostic artifacts; it does not rebuild platform packages. It rejects runs from another repository, the wrong workflow, unsuccessful runs, or commits whose C++ app source tree differs from the delivery workflow commit. Run artifacts expire after three days, so deliver promptly.

The workflow verifies the Windows and macOS supplied SHA-256 files, computes Linux and Arch checksums, extracts the AppImage without launching the app, and builds the Arch package from its `usr` tree using the repository PKGBUILD. It retains all four installers, per-asset checksums, a combined `SHA256SUMS`, source-head provenance, and the Windows source archive/patch/manifest bundle. It creates or adds assets only to the fixed `v0.1.9` draft release. Published releases and name collisions are refused; assets are never overwritten. Review the draft assets, provenance, checksums, and platform limitations before publishing it manually. The DMG is ad hoc signed but not notarized, the EXE is unsigned, and no trusted publisher signature is provided.

The Arch container uses an unprivileged builder with the extracted bundle's
owner UID so private extraction permissions remain readable without opening
the bundle to other users. Windows notice evidence gaps are printed in the
delivery log for review before publication.
