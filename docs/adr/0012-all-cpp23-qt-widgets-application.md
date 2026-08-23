# All-C++23 Qt Widgets application

## Status

Accepted on 2026-08-24. Supersedes ADR 0011.

## Context

The transitional product uses React inside Tauri, and the unreleased replacement work uses Native SDK/Zig with a Rust static core. Carrying either cross-language architecture to release would preserve multiple toolchains and integration seams while the product still needs complete cross-platform media, document, accessibility, performance, and package proof. The final product must be one maintainable native desktop application with no browser runtime, helper process, or dormant fallback.

The user selected an all-C++ implementation and required current stable tooling, efficient algorithms, bounded memory, complete planning before implementation, and Linux, macOS, and Windows package parity before cutover.

## Decision

The final application is one C++23 executable built with CMake 4.4 and Qt 6.11 Widgets. Application and test code are C++. Build, package, and CI definitions may use CMake, JSON, YAML, shell, and PowerShell. The application links approved in-process C libraries such as SQLite, libmpv and its FFmpeg runtime, and PDFium.

The final dependency set excludes Qt Quick, QML, Qt WebEngine, Qt WebView, Tauri, React, Next.js, Node, Native SDK, Zig, Rust, external mpv, and `ffmpeg` or `ffprobe` executables. Qt and the media runtime are dynamically linked under the locked release and license policy.

Six deep modules own the application:

- `AppController` owns the GUI-thread state transition path, navigation, stale-result rejection, focus restoration, and typed effects.
- `Library` owns the current SQLite schema, one writer connection, revisions, scans, reconciliation, identity, search, Progress, Learning activity, notes, stats, and settings.
- `LocalFiles` owns canonicalization, approved roots, safe local handles, marker access, and external-open validation.
- `Documents` owns bounded text, Markdown, sanitized HTML, DOCX normalization, PDFium rendering, and the fixed tile cache.
- `Player` owns libmpv, its event thread, commands, tracks, chapters, screenshots, diagnostics, and render-context lifetime.
- `SingleInstance` owns process locking, startup-route forwarding, validation, and main-window activation.

The Qt GUI thread exclusively owns `QApplication`, widgets, item models, focus, accessibility objects, and OpenGL painting. The Library thread exclusively owns the writable SQLite connection. Bounded scan, search, document, and Player workers perform blocking work. Typed C++ requests and results cross those seams through bounded queues. Replaceable work coalesces; accepted terminal results are never silently dropped.

Views emit actions only. They execute no disk, SQLite, parsing, search, or Player work. `MpvVideoWidget::paintGL()` performs only render-context update and render calls while the Qt OpenGL context is current. It cannot wait on the Player or Library thread.

The C++ line uses a distinct fresh data path and one exact current schema. It never inspects, imports, migrates, backs up, restores, rolls back, or removes databases from the Tauri, Native SDK, Rust, or any obsolete line. An unchanged C++ schema reopens across C++ package replacement. A future schema replacement receives a new data path and decision. Current `.melearner-course.json` files remain domain inputs.

Efficiency is an acceptance property:

- Scanning visits each file once and uses deterministic sorting, for `O(F log F)` time and `O(F)` bounded scan state.
- Reconciliation uses indexed hash maps for expected `O(C + S + L + T)` time. All-pairs identity matching is rejected.
- Library, Course, search, notes, activity, and document projections are paged. The GUI never stores a duplicate complete Library graph.
- Qt item views use model virtualization. A row does not create its own widget.
- Search uses a SQLite FTS index and bounded result pages. GUI-side full-Library filtering is rejected.
- Queues, event payloads, document blocks, PDF tiles, thumbnails, and caches use the fixed reviewed limits in `docs/research/cpp23-qt-overhaul.md`.
- The PDF cache stores at most 64 512x512 RGBA tiles unless a measured profile justifies a reviewed replacement.
- Position and scan progress events coalesce. Terminal events do not.

Tests exercise the highest stable seams: `Library` with real temporary SQLite and filesystem fixtures, `AppController` with deterministic typed completions, Qt Widgets through Qt Test, `Documents` through real files and tile results, `Player` through fake and live libmpv adapters, and installed packages through automation, screen readers, binary audits, process inspection, and the media corpus.

## Cutover

1. Freeze and complete deterministic parity and evidence fixtures.
2. Prove the Qt/C++ foundation, accessibility, single-window libmpv rendering, and browser-free package composition on all three operating systems.
3. Add the fresh C++ Library and asynchronous state/effect seam.
4. Deliver read-only Library, search, Course, and Lesson vertical slices.
5. Add scans, identity recovery, appearance, documents, Player, Progress, notes, stats, single instance, and complete accessibility.
6. Enforce performance, fault, signing, legal, and clean-machine gates for Linux AppImage and Arch assets, a universal macOS DMG, and a Windows x64 MSI.
7. Delete every transitional and superseded implementation only after all four installed artifacts pass from one revision, then rerun the full matrix on the changed post-deletion revision.

Tauri remains the production release until step 7. The C++ line is unreleased before that gate. There is no public dual-stack release, runtime fallback, or downgrade path.

## Consequences

- Story 39's former Rust C ABI wording is superseded by its safety intent: RAII ownership, typed bounded queues, exception containment, cancellation, deterministic shutdown, and GUI-thread isolation.
- Native SDK issues and Rust/Zig implementation tickets are superseded by C++ tracer-bullet tickets rather than completed as release work.
- macOS OpenGL deprecation, dynamic Qt licensing, codec distribution, signing identities, and legal approval remain explicit release gates.
- A failed platform gate delays cutover and does not permit a WebView, sidecar, second window, or host-runtime fallback.
