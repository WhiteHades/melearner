<div align="center">

<img src="cpp-app/assets/melearner-logo.png" width="112" height="112" alt="melearner logo" />

# melearner

Local-only desktop learning from course files already on your machine.

[![platform](https://img.shields.io/badge/platform-Linux-8f2d25?style=flat)](docs/install.md)
[![stack](https://img.shields.io/badge/stack-C%2B%2B23%20%C2%B7%20Qt%206.11-8f2d25?style=flat)](cpp-app/README.md)
[![storage](https://img.shields.io/badge/storage-local%20SQLite-8f2d25?style=flat)](docs/privacy-and-legal.md)
[![license](https://img.shields.io/badge/license-MIT-8f2d25?style=flat)](LICENSE)

</div>

melearner scans a folder, groups local videos, audio, and documents into courses, remembers progress locally, and shows learning activity. It does not download, stream, sync, or share content.

## Install on Linux

The supported source installer builds and tests the C++23/Qt application, then installs it into `$HOME/.local` by default:

```bash
git clone https://github.com/WhiteHades/melearner
cd melearner
bash scripts/install-cpp-linux.sh
```

Run it with:

```bash
$HOME/.local/bin/melearner
```

The Arch package and portable archive use the same C++ executable and are being qualified from the Linux release build.

## First run

1. Open melearner.
2. Choose the folder that contains your courses.
3. Open a course and select a lesson.

Progress is stored in local SQLite under the C++ application data directory.

## Features

- Local course library from folders you choose
- In-window video and audio playback with resume position
- Documents, subtitles, and section-aware course outlines
- Search across courses, sections, and lessons
- Local SQLite progress, notes, and learning activity
- Durable course identity for folder moves and renames
- Offline by default: no accounts, telemetry, or sync

## Development

See [C++ development](cpp-app/README.md), [installation](docs/install.md), and [usage](docs/usage.md).

## License

MIT. See [LICENSE](LICENSE).
