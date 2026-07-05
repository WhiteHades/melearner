<div align="center">

<img src="src-tauri/icons/icon.svg" width="112" height="112" alt="melearner logo" />

# melearner

Local-first desktop app for learning from course files you already have on your machine.

[![platform](https://img.shields.io/badge/platform-linux-6366f1?style=flat)](https://tauri.app)
[![stack](https://img.shields.io/badge/tauri%202%20%C2%B7%20next.js%2016%20%C2%B7%20rust-0f172a?style=flat)](https://nextjs.org)
[![storage](https://img.shields.io/badge/storage-local%20sqlite-22c55e?style=flat)](docs/privacy-and-legal.md)
[![license](https://img.shields.io/badge/license-MIT-a855f7?style=flat)](LICENSE)

</div>

melearner scans a root folder, groups local videos/audio/documents into courses, and remembers progress locally. It does not download, stream, sync, or share content.

## Install

### Arch Linux

Recommended: install the AUR package.

```bash
yay -S melearner-bin
# or
paru -S melearner-bin
```

Optional manual package install: download `melearner-bin-<version>-1-x86_64.pkg.tar.zst` from the [latest release](https://github.com/WhiteHades/melearner/releases/latest), then run:

```bash
sudo pacman -U melearner-bin-<version>-1-x86_64.pkg.tar.zst
```

### Other Linux Distros

Download the AppImage from the [latest release](https://github.com/WhiteHades/melearner/releases/latest), then run:

```bash
chmod +x melearner_<version>_amd64.AppImage
./melearner_<version>_amd64.AppImage
```

More install details: [docs/install.md](docs/install.md).

## First Run

1. Open melearner.
2. Click **Scan root folder** and choose your course directory.
3. Open a course, pick a lesson, and keep learning.

Progress is saved in local SQLite and restored when you come back.

## Features

- Local course library from folders you choose
- Video/audio playback with resume position and keyboard controls
- Documents, subtitles, and section-aware course outlines
- Search across courses, sections, and lessons
- Local SQLite progress storage
- Offline by default: no accounts, telemetry, or sync

## Docs

- [Install](docs/install.md)
- [Usage and shortcuts](docs/usage.md)
- [Development and release notes](docs/development.md)
- [Stats and course identity plan](docs/stats-and-identity-plan.md)
- [Privacy and legal](docs/privacy-and-legal.md)
- [Architecture decisions](docs/adr/)

## License

MIT. See [LICENSE](LICENSE).
