# Curated release assets

melearner releases should publish a small set of tested installer assets. the goal is to make installation clear, not to upload every bundle format Tauri can produce.

for linux x86_64 releases, publish these assets:

- `melearner_<version>_amd64.AppImage` for portable linux use
- `melearner-bin-<version>-<pkgrel>-x86_64.pkg.tar.zst` as the native arch package asset

for windows x86_64 releases, publish an MSI only after it is built on Windows and tested on a clean Windows VM.

Arch users should install through the AUR package `melearner-bin`. The native arch package asset remains available for optional manual `pacman -U` installs and as the AUR source asset.

The release workflow builds the AppImage. The arch asset is built from `packaging/arch/PKGBUILD` after a clean no-bundle release binary exists.

when building appimage on arch, set `NO_STRIP=true`; the arch PKGBUILD's `options=("!strip")` only covers pacman packaging.

do not upload deb, rpm, app tarballs, updater json, nsis, dmg, or duplicated architecture variants unless that platform is being intentionally supported and tested for the release.
