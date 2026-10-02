<div align="center">

<img src="cpp-app/assets/melearner-logo.png" width="112" height="112" alt="melearner logo" />

# melearner

Local only desktop learning from course files already on your machine.

[![platform](https://img.shields.io/badge/platform-Linux-262626?style=flat)](docs/install.md)
[![stack](https://img.shields.io/badge/stack-C%2B%2B23%20%C2%B7%20Qt%206.11-262626?style=flat)](docs/development.md)
[![storage](https://img.shields.io/badge/storage-local%20SQLite-262626?style=flat)](docs/privacy-and-legal.md)
[![source license](https://img.shields.io/badge/source%20license-MIT-262626?style=flat)](LICENSE)

</div>

melearner scans a folder, groups local videos, audio, and documents into courses, remembers progress locally, and shows learning activity. It does not download, stream, sync, or share content.

## Install on Linux

Install the [native build dependencies](docs/install.md#linux-from-source) first.
The source installer builds and tests the application, then installs it into `$HOME/.local` by default:

```bash
git clone https://github.com/WhiteHades/melearner
cd melearner
bash scripts/install-cpp-linux.sh
```

Run it with:

```bash
$HOME/.local/bin/melearner
```

The current source version is `0.1.9`. Binary packages for this version are not yet available.

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
- Local SQLite progress and learning activity
- Durable course identity for folder moves and renames
- Works entirely on your machine

## Development

See [building from source](docs/development.md), [Windows builds](docs/windows-development.md), [installation](docs/install.md), and [usage](docs/usage.md).

## License

The application source is MIT licensed. See [LICENSE](LICENSE).
Bundled dependencies retain their own licenses, including GPL-enabled libmpv.
The Linux installation includes its source, patch and license texts.
