<div align="center">

<img src="src-tauri/icons/icon.svg" width="120" height="120" alt="melearn logo" />

# melearn

a native desktop app for learning from your own course files. point it at a folder, open a lesson, take notes. everything stays on your machine.

melearn is a viewer. it doesn't download, stream, or share anything. see the [legal disclaimer](#legal-disclaimer).

<br />

[![platform](https://img.shields.io/badge/platform-linux%20%7C%20macos%20%7C%20windows-6366f1?style=for-the-badge)](https://tauri.app)
[![stack](https://img.shields.io/badge/tauri%202%20%C2%B7%20next.js%2016%20%C2%B7%20rust-0f172a?style=for-the-badge)](https://nextjs.org)
[![storage](https://img.shields.io/badge/storage-local%20sqlite-22c55e?style=for-the-badge)](#privacy)
[![license](https://img.shields.io/badge/license-MIT-a855f7?style=for-the-badge)](LICENSE)

</div>

---

## install

grab the latest release for your platform from the [releases page](https://github.com/WhiteHades/melearn/releases). download the file, open it, done.

| platform    | what to download          |
| ----------- | ------------------------- |
| linux       | `.deb` or `.AppImage`     |
| macos       | `.dmg` (intel or apple silicon) |
| windows     | `.msi` or `.exe`          |

on linux, after downloading the deb:

```bash
sudo dpkg -i melearn_*.deb
```

or, if you built it yourself from source:

```bash
sudo cp src-tauri/target/release/melearn /usr/local/bin/melearn
```

## first run

1. launch melearn from your app drawer (linux), launchpad (mac), or start menu (windows)
2. click **choose folder** and pick a directory that holds your course files
3. melearn walks the folder, groups files into courses, sections, and lessons, and drops you in the library
4. click a course to open it, click a lesson to play or read
5. press `space` to play, `f` for fullscreen, `n` to jump to the next lesson

notes go in the card under the player. progress saves automatically. close the app, come back tomorrow, and you're exactly where you left off.

## what it does

you pick a folder. melearn walks it, groups files into courses, sections, and lessons, and gives you a clean player plus a place to write notes against timestamps. progress is saved locally so you can close the app, come back tomorrow, and pick up where you stopped.

that's the whole product.

## features

- scans a folder and builds a library on the spot
- video player with resume position, keyboard shortcuts, and fullscreen
- notes tied to a timestamp on the lesson
- progress tracking in sqlite
- full-text search across courses, sections, and lessons
- light and dark themes
- frameless window, native title bar drag regions
- works offline. no accounts, no telemetry, no network

## keyboard shortcuts

| key       | action           |
| --------- | ---------------- |
| space / k | play / pause     |
| m         | mute / unmute    |
| f         | fullscreen       |
| j / ←     | seek back 10s    |
| l / →     | seek forward 10s |
| ↑ / ↓     | volume up / down |
| n         | next lesson      |
| p         | previous lesson  |

## for developers

if you want to build melearn yourself, hack on it, or send a patch, keep reading.

### prerequisites

- node 20+
- pnpm 10+
- rust toolchain (`rustup`)
- tauri 2 system deps for your os ([guide](https://tauri.app/start/prerequisites/))

### develop

```bash
pnpm install
pnpm tauri:dev
```

this boots the next dev server and opens the native desktop window in one step. you don't need a second terminal.

### verify

```bash
pnpm verify   # type-check + lint + web build + cargo check
```

### build

```bash
pnpm tauri:build                 # current platform
pnpm tauri:build:linux           # deb + appimage
pnpm tauri:build:windows         # msi + nsis
pnpm tauri:build:macos           # intel
pnpm tauri:build:macos-arm       # apple silicon
```

## how it's built

a few things worth knowing if you're going to read the code:

- the database lives at `$HOME/.local/share/melearn/melearn.db`. there's no remote sync, no fallback, no shadow copy.
- video files stream through a tiny axum server bound to `127.0.0.1`. it starts lazily on the first video you open and never touches the network.
- the frontend calls tauri commands directly. no trpc, no react-query, no extra runtime layer between you and the rust backend.
- background services init on first use, never in the window setup callback. the app opens fast.
- the video player updates the time text and progress bar in a `requestAnimationFrame` loop. no React re-renders per frame.

## privacy

melearn doesn't phone home. no analytics, no telemetry, no update check, no network call. the database and the logs sit in `~/.melearn/` and `~/.local/share/melearn/`, both local to you.

## legal disclaimer

**melearn is a local media player and a file organiser. it does not distribute, stream, download, host, or facilitate access to any content.**

what melearn does not do:

- it doesn't provide any course, video, audio, or document
- it doesn't include any built-in library, catalog, or content source
- it doesn't connect to udemy, coursera, skillshare, pluralsight, or any other platform
- it doesn't bypass, crack, decrypt, or circumvent any drm or paywall
- it doesn't download, scrape, mirror, or index any third-party content

melearn plays files that already exist on your device, inside folders you point it at. you're solely responsible for the legality of the files on your machine and the rights you hold to view them.

by using melearn you confirm that one of the following is true for every file you load: you own it, you have a valid licence or subscription, or it's in the public domain.

the developers of melearn don't endorse:

- illegal downloading, copying, or redistribution of copyrighted material
- using this app to access content you haven't legitimately acquired
- bypassing the terms of service of any course platform, streaming service, or content provider
- sharing or distributing pirated media

if you got a course from a piracy site, a torrent, a file locker, a telegram channel, or a discord server, please don't use melearn for it. support the people who made the material. pay for the courses you want to learn from. piracy hurts the educators, the authors, and the small creators who make learning accessible in the first place.

melearn is provided "as is" without warranty of any kind. the developers aren't liable for any user action that violates copyright law, terms of service, or any applicable regulation in your jurisdiction.

## license

MIT. see [license](LICENSE) for details.
