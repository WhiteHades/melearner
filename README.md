<div align="center">

<img src="cpp-app/assets/melearner-logo.png" width="112" height="112" alt="meLearner logo" />

# meLearner

[![platform](https://img.shields.io/badge/platform-Linux%20%7C%20Windows%20%7C%20macOS-262626?style=flat)](docs/install.md)
[![stack](https://img.shields.io/badge/stack-C%2B%2B23%20%C2%B7%20Qt%206.11-262626?style=flat)](docs/development.md)
[![storage](https://img.shields.io/badge/storage-local%20SQLite-262626?style=flat)](docs/privacy-and-legal.md)
[![source license](https://img.shields.io/badge/source%20license-MIT-262626?style=flat)](LICENSE)

</div>

Organise, watch and complete your downloaded courses from one place without having to manually figure out what to watch next. Browse videos, audio and readings, then pick up where you left off.

https://github.com/user-attachments/assets/672a65f5-9c69-45f6-bcbd-17ecec261f7e

## Install or download

Choose your system for version 0.1.2. On Linux, paste the command into a terminal to download, verify and install the app.

| Platform | Download | Install |
| --- | --- | --- |
| Linux | [AppImage](https://github.com/WhiteHades/melearner/releases/download/v0.1.2/melearner_0.1.2_amd64.AppImage) | `curl -fsSL https://raw.githubusercontent.com/WhiteHades/melearner/main/scripts/install-linux.sh \| bash` |
| Linux (Arch) | [Package](https://github.com/WhiteHades/melearner/releases/download/v0.1.2/melearner-bin-0.1.2-1-x86_64.pkg.tar.zst) | `bash -c "$(curl -fsSL https://raw.githubusercontent.com/WhiteHades/melearner/main/scripts/install-linux.sh)" -- --arch` |
| Windows | [Setup installer](https://github.com/WhiteHades/melearner/releases/download/v0.1.2/melearner-0.1.2-setup.exe) | Open the installer. |
| macOS | [Disk image](https://github.com/WhiteHades/melearner/releases/download/v0.1.2/melearner-0.1.2-macos-arm64.dmg) | Open the disk image and drag the app to Applications. |

The AppImage command installs for your user and adds an app menu entry. The Arch command uses `pacman` and asks for administrator access. [Requirements and manual installation](docs/install.md).

## Get started

For easier setup, keep all course folders in one directory, with a separate folder for each course.

1. Open meLearner.
2. Choose that parent directory.
3. Pick a lesson.

For technical details and more ways to install, see [Advanced setup](docs/advanced.md).

[Install](docs/install.md) · [Help](docs/usage.md) · [Contribute](CONTRIBUTING.md) · [License](LICENSE)
