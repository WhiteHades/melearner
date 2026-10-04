# CI builds and draft delivery

Start the Linux, Windows or macOS native build workflows from the repository
Actions tab. The separate Linux tooling and app workflow runs on pushes and
pull requests.

Each native workflow builds the app and tests the core course and document
flow. Run the platform affected by your change and review its logs. The
optional `package` input builds a diagnostic installer, kept for three days.
It is off by default. Passing tests do not prove playback or installation on a
user's computer.

Enable Windows `run_playback` for playback changes. It checks H.264, HEVC,
multiple audio tracks, saved progress and large text with Qt software OpenGL.
It needs a wide display and stops after three minutes. The macOS UI test uses
the runner display because Qt offscreen mode cannot activate windows or create
a graphics context.

## Package limits

See [installation notes](install.md) for operating system floors and signing.
Package checks gather license notices and source records. Do not publish
packages with missing required evidence. Build artifacts are diagnostic until
those checks finish.

## Draft delivery

The manual `Draft release delivery` workflow takes successful Linux, Windows
and macOS build run IDs. It checks each run and source tree, then reuses the
artifacts without rebuilding packages. Artifacts expire after three days, so
run delivery soon after the builds.

It checks Windows and macOS SHA-256 files, calculates Linux and Arch
checksums, extracts the AppImage without launching it, then builds the Arch
package from its `usr` tree. It puts four installers, checksums, dependency
records and the Windows player source files in a `v0.1.0` draft. It will not
change a published release or replace assets. Review the draft before manual
publication. No workflow publishes a public release automatically.
