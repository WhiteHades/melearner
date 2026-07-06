# Curated release assets

melearner releases should publish a small set of tested installer assets. the goal is to make installation clear, not to upload every bundle format Tauri can produce.

for linux x86_64 releases, publish these assets:

- `melearner_<version>_amd64.AppImage` for portable linux use
- `melearner-bin-<version>-<pkgrel>-x86_64.pkg.tar.zst` as the native arch package asset

for windows x86_64 releases, publish an MSI only after it is built on Windows and tested on a clean Windows VM.

Arch users should install through the AUR package `melearner-bin`. The native arch package asset remains available for optional manual `pacman -U` installs and as the AUR source asset.

The release workflow builds the AppImage. The arch asset is built from `packaging/arch/PKGBUILD` after a clean no-bundle release binary exists.

when building appimage on arch, set `NO_STRIP=true`; the arch PKGBUILD's `options=("!strip")` only covers pacman packaging.

## Local Installed App Updates

On the maintainer laptop, the desktop launcher uses `/usr/share/applications/io.github.whitehades.melearner.desktop`, whose `Exec=melearner` resolves to `/usr/bin/melearner`. After any completed task that changes launchable app behavior, every launcher-visible installed instance must be updated. On this laptop, that means updating the native Arch package path so the launcher runs the new build from `/usr/bin/melearner`.

The required local update path is:

1. Build the release binary with `NO_STRIP=true pnpm tauri build --no-bundle --ci`.
2. Build `packaging/arch/PKGBUILD` with `makepkg -f -C`.
3. Install the resulting `melearner-bin-<version>-<pkgrel>-x86_64.pkg.tar.zst` with `sudo pacman -U`.
4. Verify `command -v melearner` and the desktop entry resolve to `/usr/bin/melearner`.

Do not use `cargo install` or `~/.cargo/bin/melearner` as the local update path for this app; Cargo binaries are developer build artifacts, not the installed launcher target. If a task changes app behavior but the Arch-installed `/usr/bin/melearner` instance is not updated, the final summary must say that the launcher still runs the previous installed build.

do not upload deb, rpm, app tarballs, updater json, nsis, dmg, or duplicated architecture variants unless that platform is being intentionally supported and tested for the release.
