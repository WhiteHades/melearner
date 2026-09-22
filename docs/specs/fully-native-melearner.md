# Fully native all-C++ melearner

The repository file `docs/specs/fully-native-melearner.md` is canonical. GitHub issue https://github.com/WhiteHades/melearner/issues/31 is its tracker mirror and ticket parent. Wayfinder map: https://github.com/WhiteHades/melearner/issues/25. Architecture: `docs/adr/0012-all-cpp23-qt-widgets-application.md`. Detailed engineering plan: `docs/research/cpp23-qt-overhaul.md`. Ticket graph: `docs/specs/cpp23-implementation-plan.md`.

## Problem Statement

melearner now has one maintained C++23/Qt application. The former React/WebView/Tauri and Native SDK/Zig/Rust lines were superseded and removed during the Linux-first cutover. Linux verification is active; macOS and Windows qualification remains pending.

The learner needs one responsive, polished, local only desktop application for Courses already on the machine. The final package must contain no WebView, browser runtime, JavaScript UI, external player, helper process, second Player window, or compatibility path for obsolete data.

## Solution

Replace every application layer with one C++23 executable using Qt 6.11 Widgets and CMake 4.4. Qt owns native windows, layouts, item views, focus, keyboard behavior, accessibility, themes, automation, and the OpenGL Player widget. C++ modules own the current SQLite schema, Library scan and identity rules, search, Progress, Learning activity, notes, document conversion, PDF tiles, embedded libmpv, and local-path validation.

All disk, SQLite, scan, search, document, and Player control work is asynchronous from the GUI's perspective. The GUI owns bounded visible projections instead of a duplicate Library graph. Frozen parity inputs remain under `fixtures/parity/`; they are not runtime implementations.

## User Stories

1. **S1.** As a learner, I want the app to launch into a responsive Library, so that I can resume studying without waiting on an unresponsive window.
2. **S2.** As a learner, I want the C++ app to create a clean current Library database, so that obsolete storage formats do not constrain the new architecture.
3. **S3.** As a learner, I want renamed or moved Courses to retain their identity, so that Progress follows my local files.
4. **S4.** As a learner, I want temporarily missing Courses to remain visible with a clear state, so that I understand what happened without losing data.
5. **S5.** As a learner, I want to select or change my root folder through a native dialog, so that setup feels integrated with my operating system.
6. **S6.** As a learner, I want Library scans to run without freezing navigation, so that large collections remain usable.
7. **S7.** As a learner, I want scan warnings and partial failures to be specific and recoverable, so that one bad file does not block my whole Library.
8. **S8.** As a learner, I want a calm resume area that shows the next meaningful Lesson, so that returning to study takes one action.
9. **S9.** As a learner, I want Course rows and artwork to communicate Progress without visual noise, so that I can scan my Library quickly.
10. **S10.** As a learner, I want search results to appear incrementally while I type, so that a large Library still feels immediate.
11. **S11.** As a keyboard user, I want to open search, move through results, open a Course, and return without a pointer, so that every main flow is efficient.
12. **S12.** As a learner, I want Course navigation to preserve my selected Section and Lesson, so that I do not lose context.
13. **S13.** As a learner, I want Lesson lists to remain smooth with very large Courses, so that virtualization is invisible to me.
14. **S14.** As a learner, I want video to render inside the same app window, so that playback feels like part of the product rather than a sidecar.
15. **S15.** As a learner, I want MP4 H.264/AAC playback to start reliably, so that common Courses work out of the box.
16. **S16.** As a learner, I want MKV files with multiple audio tracks to expose and switch tracks without reload, so that multilingual material works naturally.
17. **S17.** As a learner, I want HEVC 10-bit playback to work visibly through the packaged software-capable codec runtime, so that support never depends on optional hardware decoding.
18. **S18.** As a learner, I want external SRT and VTT Subtitle tracks to be selectable, so that local subtitles remain useful.
19. **S19.** As a learner, I want chapters to be listed and selectable, so that long Lessons are navigable.
20. **S20.** As a learner, I want play, pause, seek, volume, mute, speed, frame-step, screenshot, and fullscreen controls, so that the Player remains complete.
21. **S21.** As a learner, I want Player controls to remain responsive while media opens, seeks, resizes, or fails, so that slow native work never freezes the app.
22. **S22.** As a learner, I want playback position to save and restore after pause, quit, and reopen, so that I can continue exactly where I stopped.
23. **S23.** As a learner, I want completing a Lesson to update Course Progress and Learning activity atomically, so that stats remain trustworthy.
24. **S24.** As a learner, I want text and Markdown Lessons rendered natively with readable typography, so that reading feels focused.
25. **S25.** As a learner, I want safe local HTML and DOCX content converted into native document blocks, so that common documents do not require a browser.
26. **S26.** As a learner, I want PDFs rendered natively with smooth page virtualization and zoom, so that document Courses remain first-class.
27. **S27.** As a learner, I want unsupported document formats to open in my default app with a clear explanation, so that the native app fails honestly.
28. **S28.** As a learner, I want timestamped notes to remain attached to Lessons, so that my study context survives app restarts.
29. **S29.** As a learner, I want stats to summarize actual local Learning activity without looking like a generic analytics dashboard, so that the information supports study rather than administration.
30. **S30.** As a learner, I want light, dark, and cozy appearances with consistent hierarchy, so that the app fits my environment.
31. **S31.** As a learner, I want the interface to adapt from compact laptop windows to wide desktop windows, so that no important control is clipped or wastefully stretched.
32. **S32.** As a learner using assistive technology, I want every control and virtualized row to expose a stable role, name, value, state, and action, so that the app is fully operable.
33. **S33.** As a learner sensitive to motion, I want reduced-motion preferences to remove nonessential transitions, so that polish never harms usability.
34. **S34.** As a learner, I want errors to identify the failing Course, Lesson, file, or subsystem and offer the next safe action, so that recovery is understandable.
35. **S35.** As a learner, I want the app to remain single-instance, so that repeated launches focus the existing window instead of duplicating database or Player work.
36. **S36.** As a learner, I want clean native installers on Linux, macOS, and Windows, so that no manual runtime setup is required for supported packages.
37. **S37.** As a maintainer, I want package audits to prove there is no WebView, Tauri, React, Native SDK, Zig, Rust, external mpv, FFmpeg helper, or second Player window, so that the architectural promise is enforceable.
38. **S38.** As a maintainer, I want deterministic headless UI tests and live native automation, so that interface regressions are caught without relying on manual clicking.
39. **S39.** As a maintainer, I want RAII ownership, typed bounded queues, exception barriers, cancellation, and deterministic shutdown, so that asynchronous C++ work cannot corrupt state or cross the GUI thread unsafely.
40. **S40.** As a maintainer, I want clean-machine codec and accessibility checks on every desktop OS, so that compile success is never mistaken for product readiness.

## Product Interaction Model

### Launch, onboarding, and root selection

- The app opens one main window. It immediately paints a focusable Library shell and named busy status while the Library opens.
- A fresh C++ data directory with no root opens onboarding with one primary action: **Choose root folder**.
- `QFileDialog` selects directories. Cancel writes nothing. Invalid, unreadable, or non-directory candidates retain the committed root and return a typed root error.
- A valid root commits only after canonicalization and a successful or safe-partial scan. The root, reconciled Library, and new revision commit together. Fatal failure or pre-commit cancellation changes nothing.
- Changing roots retains prior Courses as missing Courses. It never deletes retained Progress, notes, activity, or identity.

### Library, scans, missing Courses, and resume

- The header contains Search, Rescan, Settings, and the current root in a copyable elided label.
- The resume area selects only available Courses. The main virtualized Library contains all Courses in natural title order, including missing Courses. Presentation choice persists locally.
- A Course row exposes name, availability, completed Lessons, total Lessons, and Progress. Available activation opens the Course. Missing activation opens recovery with last path, missing date, **Locate Course**, **Rescan root**, and **Change root**.
- Only one scan runs at a time. The last committed revision remains navigable during `discovering`, `classifying`, `reconciling`, `committing`, and `writing_markers` phases.
- Safe partial scans commit healthy results and structured warnings. Failed Course paths are not falsely marked missing. Marker writes begin only after commit; every planned write is attempted and failures add warnings without rollback.
- Cancellation succeeds before the commit gate. Once commit wins, cancellation returns too late and the scan completes.

### Search, Course, and Lesson navigation

- `Ctrl+K` on Linux/Windows and `Cmd+K` on macOS open search from any nonmodal route. A 100 ms debounce submits a query ID; newer queries cancel or supersede older ones.
- Search uses indexed Course, Section, and Lesson names. Results group Courses and Lessons, retain missing state, and page incrementally.
- `Up` and `Down` move, `Enter` opens, and `Escape` closes and restores focus to the invoker.
- A Course selects an explicitly requested Lesson, otherwise the resume Lesson, otherwise the first incomplete Lesson, otherwise the first Lesson. The selected Section expands.
- Previous and Next traverse one stable Lesson order across Section boundaries. Back restores keyed focus and scroll to the originating Course or Lesson.
- If an open Course becomes unavailable, file work stops and the UI switches to recovery without changing the last committed Progress.

### Documents, Player, notes, stats, and settings

- Text and Markdown render as selectable native blocks. HTML and DOCX render a finite sanitized document model and list omitted constructs. They execute no script, CSS, scheme, or remote resource.
- PDF uses continuous virtual pages with page status, jump, Fit width, 100 percent, zoom in, and zoom out. Visible 512x512 tiles load first into a fixed 64-tile LRU cache.
- Unsupported formats and failed conversions preserve Lesson navigation and offer validated `QDesktopServices` external open. External open never completes a Lesson.
- `MpvVideoWidget` renders video inside the one main window. Audio uses the same controls with a named audio surface.
- Player controls stay in stable bands and include play/pause, seek, volume, mute, rate, audio tracks, Subtitle tracks, chapters, frame step, screenshot, fullscreen, Previous, and Next.
- A load opens paused at saved position. End of file saves final position and marks the Lesson complete. HEVC Main 10 must visibly render with hardware decoding disabled on every release target.
- Notes are scoped to the selected Lesson. Wide layouts use a side panel; compact layouts use a sheet. New notes capture current Player position or zero. Timed activation seeks the Player.
- Stats remain an inline learning ledger with the exact snapshot, media, top-Course, and fixed 84-day activity fields in `docs/stats-and-identity-plan.md`.
- Settings contains root actions, Light/Dark/Cozy appearance, inherited reduced-motion/high-contrast status, and real build/runtime information. Appearance has an independent revision.
- `QLockFile` plus `QLocalServer` enforce one process owner. Later launches validate and forward a bounded startup route, focus the existing window, and exit.

### Responsive, keyboard, focus, and semantics

- Minimum content size is 560x400. Compact is 560-767 px, standard is 768-1279 px, and wide is 1280 px or greater.
- Compact uses one primary pane with explicit Back. Standard uses Course outline plus Lesson. Wide adds notes or utility. Reading text is capped near 80 characters.
- Standard Qt widgets provide normal Tab, Shift+Tab, arrows, Enter, Space, and Escape behavior. Player shortcuts run only when no focused control claims the key.
- Async completion never moves focus. Closing overlays restores their invoker. Removed virtual rows focus the nearest surviving row.
- Item models expose stable IDs, row positions, counts, availability, selection, current state, and Progress. Custom Player, document, artwork, and activity widgets implement explicit Qt accessibility interfaces.
- Text and controls meet WCAG AA, targets are at least 40x40 logical pixels, 200 percent text scaling does not clip, and reduced motion removes nonessential transitions.

## Implementation Decisions

### Toolchain and dependencies

- C++23 is the stable language baseline. Draft C++26-only features are not required.
- Qt 6.11 Widgets is the UI framework. Qt Quick, QML, WebEngine, WebView, Multimedia playback, and SQL abstractions are not application dependencies.
- CMake 4.4 presets, Ninja, CTest, Qt Test, compiler warnings, sanitizers, and static analysis define the build loop.
- SQLite's C interface provides exact schema, one writer, transactions, progress interruption, custom natural collation, and FTS.
- libmpv render API provides playback. PDFium provides bounded PDF tiles. All runtime artifacts are pinned and audited.

### Deep modules and state

- `AppController` has one `dispatch(Action)` mutation path and exposes immutable `AppState` projections to views.
- `Library` accepts a typed `LibraryRequest`, returns a request ID immediately, supports cancellation, and emits typed `LibraryEvent` values.
- `LocalFiles`, `Documents`, `Player`, and `SingleInstance` expose the narrow typed protocols in `docs/research/cpp23-qt-overhaul.md`.
- `AppState` contains `app`, `navigation`, `root`, `library`, `search`, `course`, `lesson`, `player`, `document`, `notes`, `stats`, `settings`, and `overlay` projections.
- Every async surface has explicit loading, empty, ready, warning, recoverable-error, and fatal-error states as applicable.
- Errors carry code, scope, operation, optional path, retryability, diagnostic ID, user text, and ordered safe actions.

### Threads, queues, and cancellation

- Only the Qt GUI thread owns widgets, item models, state transitions, focus, accessibility, and OpenGL painting.
- One Library thread owns the writable SQLite connection. Scan discovery, search indexing, document work, and Player events run outside it so reads remain available during long work.
- Queues are bounded. Search, PDF tiles, position, volume, seek, and scan progress coalesce when replaceable. Terminal completion and failure events never silently drop.
- Requests and visible pages carry request IDs and revisions. `AppController` rejects stale route, revision, query, document, and Player generations.
- Stop tokens cancel cooperative work. SQLite uses a progress handler and `sqlite3_interrupt`. Scan cancellation has a single commit gate.

### Efficient algorithms and bounded data

- Scan is `O(F log F)` because each filesystem entry is visited once and deterministic natural ordering sorts discovered paths. Worker count is fixed.
- Course and Lesson reconciliation is expected `O(C + S + L + T)` through indexed maps and sets. Quadratic all-pairs matching is forbidden.
- Course, Lesson, search, notes, activity, and document queries return bounded pages. Qt item views fetch more pages and never instantiate a widget per row.
- Search uses SQLite FTS with indexed prefix terms and a bounded top-result heap. The GUI does no `O(N)` full-Library filtering.
- Stats use set-based aggregate queries in one read transaction. N+1 query paths fail review and load tests.
- Normalized documents have source, block, depth, image, and warning limits. PDF rendering cost is proportional to requested tile pixels and cache memory remains fixed.
- Player render transfers no decoded frame through a queue. Update requests coalesce through one atomic dirty flag.
- Performance changes require measured baselines. An asymptotically worse replacement needs evidence and an explicit decision.

## Testing Decisions

- Tests assert observable behavior through the highest available module interface. Implementation-text tests are reserved for architectural exclusions that lack a runnable seam.
- `Library` tests use real temporary SQLite databases and filesystem fixtures for schema, scans, identity, search, Progress, activity, notes, stats, and cancellation.
- `AppController` tests use deterministic fake completions and verify state, effects, stale rejection, navigation, and focus intent.
- Qt Test drives real item models and widgets for keyboard, focus, accessibility, responsive layouts, themes, and deterministic screenshots.
- Document tests cover empty, malformed, nested, oversized, active, remote, compressed, and tiled inputs.
- Player tests use a fake adapter for command/state races and live libmpv for the checked-in media corpus. Installed package tests prove visible changing frames.
- Load tests include 1,000 Courses, 100,000 Lessons, 84 activity days, paged notes, and a 500-page PDF. They assert both budgets and bounded resident memory.
- Package tests install the Linux x86_64 AppImage and Arch package, universal macOS DMG, and Windows x64 MSI on clean target images; disable networking and hardware decoding where required; and inspect imports, loaded libraries, windows, process trees, signatures, replacement/reinstall lifecycle, retained current C++ data, accessibility, and all user flows.
- The numeric budgets and watchdog limits in `docs/research/cpp23-qt-overhaul.md` are normative. A different runner records diagnostics but cannot pass or rebaseline a release profile.

## Cutover Rules

- The C++ app uses a distinct data path and exact fresh schema. It does not read or delete old app data.
- Under [ADR 0013](../adr/0013-linux-first-cpp-cutover.md), the C++ application is the Linux implementation. macOS and Windows qualification remain explicitly pending and do not block Linux work or source cleanup.
- The Linux cutover deletion of React, Next.js, Tauri, Node, Native SDK, Zig, Rust core, C ABI, obsolete workflows, stale docs, and generated leftovers is applied. Rerun the Linux installed matrix on each changed source revision; the all-platform release remains gated on all four artifacts from one source revision.
- There is no shipped fallback, dual-stack feature flag, import, migration, restore, rollback, or downgrade path.

## Out of Scope

- DRM or encrypted commercial streaming formats.
- Hosted Course distribution, accounts, cloud sync, telemetry, remote content, or streaming services.
- WebView, browser, QML, external mpv, FFmpeg helper, second-window, or host-codec fallbacks.
- Replacing OpenGL during this cutover unless the required macOS package proof makes the chosen Qt/libmpv seam impossible.
- Redesigning the domain vocabulary, navigation model, or learning feature set before parity.
- Publishing a platform based only on compile success.

## Further Notes

- `PRODUCT.md` and `DESIGN.md` define the product register and visual direction.
- `docs/adr/0008-durable-course-identity-uses-local-fingerprints.md` remains authoritative for identity.
- `docs/stats-and-identity-plan.md` remains authoritative for stats field calculations and ordering.
- Release signing identities, distribution regions, codec-patent review, and license approval are human-controlled publication gates.
