<div align="center">

<img src="src-tauri/icons/icon.svg" width="120" height="120" alt="melearn logo" />

# melearn

a beautiful, native desktop app for learning from your **legally obtained** local course library. scan a folder of files you already own, open a lesson, take notes — everything stays on your machine.

melearn is a **viewer only**. it does not download, stream, distribute, or provide any content. see the [legal disclaimer](#legal-disclaimer).

<br />

[![platform](https://img.shields.io/badge/platform-linux%20%7C%20macos%20%7C%20windows-6366f1?style=for-the-badge)](https://tauri.app)
[![stack](https://img.shields.io/badge/tauri%202%20%C2%B7%20next.js%2016%20%C2%B7%20rust-0f172a?style=for-the-badge)](https://nextjs.org)
[![storage](https://img.shields.io/badge/data-100%25%20local-22c55e?style=for-the-badge)](#privacy)
[![license](https://img.shields.io/badge/license-source%20available-a855f7?style=for-the-badge)](#license)

</div>

---

## features

- **scan local folders** — point at a directory, get an instant library of courses, sections, and lessons
- **native video player** — smooth scrubbing, resume-position memory, keyboard shortcuts
- **lesson notes** — timestamped notes that save with the lesson
- **progress tracking** — watched-time, completion state, last-accessed, all in sqlite
- **instant search** — full-text search across courses, sections, and lessons
- **dark and light themes** — system-aware
- **frameless window** — feels native, not webby
- **works fully offline** — no accounts, no telemetry, no network

## stack

| layer            | what                                                                |
| ---------------- | ------------------------------------------------------------------- |
| desktop shell    | tauri 2                                                             |
| frontend         | next.js 16 (static export), react 19, tailwind 4                    |
| ui               | shadcn/ui (radix primitives)                                        |
| backend          | rust (axum for the local video server)                              |
| storage          | sqlite via tauri plugin-sql, persisted to `~/.local/share/melearn`  |
| search           | minisearch                                                          |
| forms / schema   | react-hook-form, zod                                                |
| state            | zustand (with persist)                                              |
| url state        | nuqs                                                                |

## getting started

### prerequisites

- node 20+
- pnpm 10+
- rust toolchain (`rustup`)
- tauri 2 system dependencies for your os ([guide](https://tauri.app/start/prerequisites/))

### develop

```bash
pnpm install
pnpm tauri:dev
```

this boots the next.js dev server and opens the native desktop window in one step.

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

install on linux:

```bash
sudo cp src-tauri/target/release/melearn /usr/local/bin/melearn
```

## keyboard shortcuts

| key       | action             |
| --------- | ------------------ |
| space / k | play / pause       |
| m         | mute / unmute      |
| f         | fullscreen         |
| j / ←     | seek back 10s      |
| l / →     | seek forward 10s   |
| ↑ / ↓     | volume up / down   |
| n         | next lesson        |
| p         | previous lesson    |

## architecture notes

- **all data is local** — sqlite database at `$HOME/.local/share/melearn/melearn.db`, no network calls.
- **video files stream from a local axum server** bound to `127.0.0.1` (started lazily on first video open).
- **no `trpc` / `react-query` runtime overhead** — the frontend calls tauri commands directly.
- **lazy async init** — background services initialize on first use, never block window setup.
- **rAF-driven video player** — zero React re-renders per frame.

## privacy

melearn does not phone home. there is no analytics, no telemetry, no auto-update check, no network request of any kind. your library is yours.

## legal disclaimer

**melearn is a local media player and course organizer. it does not distribute, stream, download, host, or facilitate access to any content.**

melearn does not:

- ❌ provide any course, video, audio, or document
- ❌ include any built-in library, catalog, or content source
- ❌ connect to udemy, coursera, skillshare, pluralsight, or any other platform
- ❌ bypass, crack, decrypt, or circumvent any drm, paywall, or access control
- ❌ download, scrape, mirror, or index any third-party content
- ❌ promote, encourage, or facilitate piracy or copyright infringement in any form

melearn is a **viewer for files that already exist on your device**. it only plays media located inside folders you explicitly point it at. you are solely responsible for the legality of the files on your machine and the rights you hold to view them.

by using melearn you confirm that:

- ✅ you own the media you load, **or**
- ✅ you have a valid license, subscription, or other legal right to access and view the media, **or**
- ✅ the media is in the public domain or otherwise free of copyright restrictions

the developers of melearn **do not endorse, condone, or support**:

- the illegal downloading, copying, or redistribution of copyrighted material
- the use of this app to access content you have not legitimately acquired
- bypassing the terms of service of any course platform, streaming service, or content provider
- sharing, selling, or distributing pirated media

if you obtained a course from a piracy site, torrent, file locker, telegram channel, discord server, or any other unauthorized source — **do not use melearn for it**. support the creators and pay for the courses you want to learn from. piracy harms the educators, authors, and small creators who make learning accessible.

melearn is provided "as is" without warranty of any kind. the developers are not liable for any user action that violates copyright law, terms of service, or any applicable regulation in your jurisdiction.

## license

source available — see [license](LICENSE) for details.
