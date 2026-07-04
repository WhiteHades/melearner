# Curated release assets

melearner releases should publish a small set of installer assets with names that say the operating system and architecture. the goal is to make the release page easy to scan, not to upload every bundle format Tauri can produce.

for linux x86_64 releases, publish these assets when available:

- `melearner-linux-x86_64.appimage` for portable linux use
- `melearner-linux-x86_64.deb` for debian and ubuntu
- `melearner-arch-x86_64.pkg.tar.zst` for arch and pacman-based systems

the manual GitHub release workflow builds appimage and deb assets. the arch asset is built from `packaging/arch/PKGBUILD` after the release binary exists.

do not upload rpm, app tarballs, updater json, msi, nsis, dmg, or duplicated architecture variants unless that platform is being intentionally supported and tested for the release.
