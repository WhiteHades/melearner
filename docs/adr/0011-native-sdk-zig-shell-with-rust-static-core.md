# Native SDK Zig shell with Rust static core

## Status

Accepted on 2026-07-16.

## Context

The transitional product uses React inside a Tauri/WebView shell while Rust and embedded libmpv own most local domain and playback behavior. That transport makes UI responsiveness, native accessibility, package composition, and same-window media rendering harder to prove. The final product must have one native UI architecture without a dormant browser or sidecar fallback.

## Decision

The final application is one Native SDK executable authored in Zig and `.native` views, linked in process to the Rust `melearner_core` static library through a versioned C ABI. Native SDK owns the window, Model/Msg/update loop, layout, controls, focus, keyboard input, accessibility, automation, and the host graphics context. Rust remains the sole owner of the current SQLite schema, scans, durable identity, search, Progress, Learning activity, notes, documents, and embedded libmpv state.

All disk, database, search, document, and Player control work is asynchronous from the UI perspective. Rust returns request IDs, enqueues bounded typed events, and wakes the Native SDK event loop; only that event-loop thread mutates the Zig Model. The media render call is synchronous only inside the SDK-owned native surface draw callback while its graphics context is current.

The shipped package and process contain no Tauri, React, Node runtime, WebView, browser engine, JavaScript bridge, external mpv, `ffmpeg`/`ffprobe` executable, or second Player window. Bundled libmpv, FFmpeg libraries, and the PDF renderer are in-process runtime libraries, not sidecars.

This ADR supersedes the final Tauri/React/WebView host and transport portions of ADRs 0001, 0002, 0003, and 0010; the final package/runtime rules in ADRs 0005 and 0010; and any interpretation of ADR 0007 or ADR 0010 that permits an FFmpeg/FFprobe helper in the final native package. It preserves:

- the local-first, offline desktop product from ADR 0001;
- SQLite as the sole durable Library store from ADR 0002;
- approved-root local-file access and no localhost/browser playback from ADR 0003;
- curated, tested release artifacts from ADR 0005;
- no render-time thumbnail generation and bounded cached background work from ADR 0007, with final work performed in process;
- one compositor-visible window, in-process libmpv, local files, complete Player controls, and no sidecar from ADR 0010;
- conservative durable Course identity from ADR 0008; and
- the cleanup requirement from ADR 0009.

The first native release uses one distinct fresh current database and does not inspect or import pre-native or obsolete-schema databases. Native-to-native package replacement reopens that database while the exact current schema is unchanged. A future schema replacement uses a new fresh data path and decision, not migration, backup, restore, rollback, or compatibility code. Current `.melearner-course.json` markers remain current Course identity inputs.

## Cutover

Cutover is staged and gated:

1. Freeze deterministic current-domain, scan, document, media, accessibility, and package fixtures.
2. Prove Native SDK native-only hosts, external media surface, external effects, and Linux/Windows accessibility on packaged fixture apps.
3. Stabilize the Rust static core and versioned C ABI while preserving the Tauri crate's existing direct Rust reuse of the core scanner.
4. Build read-only Library, search, Course, and Lesson native slices.
5. Add scans, Progress, stats, notes, documents, and Player parity.
6. Build, sign, install, automate, update, remove, and inspect AppImage, Arch, DMG, and MSI packages on clean machines.
7. Delete the transitional React/Tauri/Node shell, old release jobs, and obsolete artifacts only after every package gate passes.

The Tauri shell remains the production line during stages 1-6 and must pass its frozen regression suite at every stage gate. Its existing Cargo dependency on `melearner_core` and direct scanner reuse remain frozen; they do not need to be decoupled before cutover. Before Stage 6, the transitional shell must not be redirected through the C ABI, native database ownership, Native SDK effects, or Native SDK UI, and no additional adapter is introduced. The Native SDK target is unreleased until the final gate. There is no public dual-stack release, runtime feature flag, database fallback, or rollback to the transitional shell.

## Consequences

- Native SDK foundation work and package evidence are release blockers, not optional follow-up work.
- A failed platform package gate delays cutover; it does not justify a WebView, sidecar, or second-window exception.
- Transitional ADR implementation notes remain readable until cutover but their status pointers identify which details are no longer the final direction.
- The exact interaction contract is in `docs/specs/fully-native-melearner.md`; codec, package, acceptance, and evidence policy is in `docs/research/native-sdk-overhaul.md`.
