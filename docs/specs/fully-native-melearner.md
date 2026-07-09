# Fully native melearner

Source map: https://github.com/WhiteHades/melearner/issues/2

## Problem Statement

melearner currently depends on a React/WebView/Tauri shell around an embedded libmpv Player. The shipped Linux path can become unresponsive when synchronous Player commands contend with platform-main-thread surface work, and the current architecture makes cross-platform playback, packaging, accessibility, and visual verification harder than they should be. The user wants one responsive, polished, fully native local-first learning app on Linux, macOS, and Windows, with no WebView, browser runtime, external player, or helper process in the final package.

## Solution

Rebuild the entire visible product with Native SDK `.native` views and a Zig Model/Msg/update loop. Retain the proven Rust domain and media logic as an in-process static library behind a versioned C ABI. Extend Native SDK with a true native-only host, a cross-platform external media surface, application-defined asynchronous effects, and Linux/Windows accessibility bridges. Preserve the current Library, Course, Lesson, Progress, notes, search, and Player workflows while redesigning their expression as a modern, minimal study instrument.

The production app will be one Native SDK executable. Native SDK owns windows, layout, controls, focus, keyboard behavior, accessibility, animation, and deterministic automation. Rust owns SQLite, migrations, Library scanning, Course identity, search, Progress, Learning activity, notes, document conversion, embedded libmpv, and approved-root validation. All expensive or blocking Rust work is asynchronous from the UI's perspective.

## User Stories

1. As a learner, I want the app to launch into a responsive Library, so that I can resume studying without waiting on an unresponsive window.
2. As a learner, I want my existing Library and Progress to appear after the native upgrade, so that the migration does not erase my history.
3. As a learner, I want renamed or moved Courses to retain their identity, so that Progress follows my local files.
4. As a learner, I want temporarily missing Courses to remain visible with a clear state, so that I understand what happened without losing data.
5. As a learner, I want to select or change my root folder through a native dialog, so that setup feels integrated with my operating system.
6. As a learner, I want Library scans to run without freezing navigation, so that large collections remain usable.
7. As a learner, I want scan warnings and partial failures to be specific and recoverable, so that one bad file does not block my whole Library.
8. As a learner, I want a calm resume area that shows the next meaningful Lesson, so that returning to study takes one action.
9. As a learner, I want Course rows and artwork to communicate Progress without visual noise, so that I can scan my Library quickly.
10. As a learner, I want search results to appear incrementally while I type, so that a large Library still feels immediate.
11. As a keyboard user, I want to open search, move through results, open a Course, and return without a pointer, so that every main flow is efficient.
12. As a learner, I want Course navigation to preserve my selected Section and Lesson, so that I do not lose context.
13. As a learner, I want Lesson lists to remain smooth with very large Courses, so that virtualization is invisible to me.
14. As a learner, I want video to render inside the same app window, so that playback feels like part of the product rather than a sidecar.
15. As a learner, I want MP4 H.264/AAC playback to start reliably, so that common Courses work out of the box.
16. As a learner, I want MKV files with multiple audio tracks to expose and switch tracks without reload, so that multilingual material works naturally.
17. As a learner, I want HEVC 10-bit playback to either work visibly or fail with a precise support message, so that I never see a silent black surface.
18. As a learner, I want external SRT and VTT Subtitle tracks to be selectable, so that local subtitles remain useful.
19. As a learner, I want chapters to be listed and selectable, so that long Lessons are navigable.
20. As a learner, I want play, pause, seek, volume, mute, speed, frame-step, screenshot, and fullscreen controls, so that the native Player remains complete.
21. As a learner, I want Player controls to remain responsive while media opens, seeks, or fails, so that slow native work never freezes the app.
22. As a learner, I want playback position to save and restore after pause, quit, and reopen, so that I can continue exactly where I stopped.
23. As a learner, I want completing a Lesson to update Course Progress and Learning activity atomically, so that stats remain trustworthy.
24. As a learner, I want text and Markdown Lessons rendered natively with readable typography, so that reading feels focused.
25. As a learner, I want safe local HTML and DOCX content converted into native document blocks, so that common documents do not require a browser.
26. As a learner, I want PDFs rendered natively with smooth page virtualization and zoom, so that document Courses remain first-class.
27. As a learner, I want unsupported document formats to open in my default app with a clear explanation, so that the native app fails honestly.
28. As a learner, I want timestamped notes to remain attached to Lessons, so that my study context survives the migration.
29. As a learner, I want stats to summarize actual local Learning activity without looking like a generic analytics dashboard, so that the information supports study rather than administration.
30. As a learner, I want light, dark, and cozy appearances with consistent hierarchy, so that the app fits my environment.
31. As a learner, I want the interface to adapt from compact laptop windows to wide desktop windows, so that no important control is clipped or wastefully stretched.
32. As a learner using assistive technology, I want every control and virtualized row to expose a stable role, name, value, state, and action, so that the app is fully operable.
33. As a learner sensitive to motion, I want reduced-motion preferences to remove nonessential transitions, so that polish never harms usability.
34. As a learner, I want errors to identify the failing Course, Lesson, file, or native subsystem and offer the next safe action, so that recovery is understandable.
35. As a learner, I want the app to remain single-instance, so that repeated launches focus the existing window instead of duplicating database or Player work.
36. As a learner, I want clean native installers on Linux, macOS, and Windows, so that no manual runtime setup is required for supported packages.
37. As a maintainer, I want package audits to prove there is no WebView, Tauri, React, external mpv, FFmpeg helper, or second Player window, so that the architectural promise is enforceable.
38. As a maintainer, I want deterministic headless UI tests and live native automation, so that interface regressions are caught without relying on manual clicking.
39. As a maintainer, I want a versioned Rust C ABI with explicit ownership and panic barriers, so that Zig/Rust integration cannot corrupt memory silently.
40. As a maintainer, I want clean-machine codec and accessibility checks on every desktop OS, so that compile success is never mistaken for product readiness.

## Implementation Decisions

- The final release uses Native SDK `.native` views and Zig logic only. It ships no WebView, browser engine, React, Tauri, Node runtime, or JavaScript bridge.
- The existing Rust logic is extracted into a `melearner_core` module built as both a static library and a Rust library. The Rust library remains the sole owner of domain rules and native media state.
- The Zig/Rust seam is a versioned C ABI with opaque handles, fixed-width values, explicit byte ownership, panic barriers, bounded queues, cancellation, and a thread-safe empty-to-nonempty waker.
- The UI never blocks on Rust. Requests return IDs immediately; ordered completion events enter the Model only on the Native SDK event-loop thread.
- SQLite keeps one writer owner. Existing tables, migrations, marker files, Course identity behavior, Progress, Learning activity, and notes remain compatible.
- Library, Lesson, search, activity, and document data are paged by stable revision. The Zig Model holds only visible pages and selected detail, not a duplicate canonical Library graph.
- Native SDK gains a native-only host mode that does not compile, link, load, or stage WebKit, WebKitGTK, WebView2, or CEF.
- Native SDK gains a generic external media-surface module on Linux, macOS, and Windows. The SDK owns the native child, layout, clipping, focus, visibility, scale, fullscreen relayout, and current graphics context.
- Rust owns `mpv_render_context` and renders only from the SDK's platform draw callback while the context is current. Control commands and lifecycle events remain asynchronous.
- The media surface exposes attached state, visibility, backend, physical dimensions, rendered-frame count, first-frame time, update flags, and last error to automation and diagnostics.
- Native SDK gains application-defined external effects with live, fake, record, and replay adapters. Bulk video pixels never cross the effect queue.
- Linux and Windows gain real OS accessibility bridges for canvas semantics; automation-only labels are not considered accessibility parity.
- The first native release preserves current Library, Course, Lesson, Player, search, notes, stats, and settings workflows. Navigation behavior is frozen before implementation and changed only through explicit product decisions.
- The visual direction is a quiet study instrument: editorial warmth with technical precision, not a generic dashboard.
- Domain motifs are personal Library shelves, Course spines, a study desk, margin notes, a playback timeline, and Progress traces.
- The signature element is a continuous learning thread that appears as Course Progress, Section/Lesson position, and Player timeline context without becoming decorative chrome.
- The palette uses warm paper, graphite, muted navy ink, fog blue, and restrained amber. Dark and cozy modes preserve the same hierarchy with charcoal and parchment rather than unrelated hues.
- Surface hierarchy uses subtle tonal shifts and restrained ambient lift. Borders are quiet and progressive; heavy shadows, glass blur, gradients, nested card stacks, and ornamental color are rejected.
- Typography carries the design. Headings are compact and editorial, body text is optimized for long study sessions, metadata has a distinct tertiary level, and time/Progress values use tabular numerals.
- The spacing system uses a 4px base, with deliberate 8/12/16/24/32/48px steps. Compact controls retain at least a 40x40px hit target.
- Course presentation replaces generic equal card grids with an adaptive shelf/list that prioritizes resume context, title readability, and visible Progress.
- Stats use an inline learning ledger and direct labels instead of interchangeable metric cards.
- Player controls live in stable native bands around the external media surface. Transparent controls over video are avoided.
- Motion is short, interruptible, and limited to opacity and transform where possible. State transitions use deceleration, no bounce, and no animation on initial load. Reduced-motion removes nonessential movement.
- Responsive behavior is fluid first. Components adapt to their available container; structural layout changes occur only when the window can no longer preserve readable columns and minimum targets.
- Compact windows use a single primary pane with explicit back navigation. Wider windows introduce Course/Lesson split panes; ultrawide windows cap reading and control line lengths instead of stretching content.
- Native document support uses bounded text/Markdown blocks, a sanitized HTML/DOCX document model, tiled PDF rendering, and an external-open fallback for unsupported fidelity. Active HTML and remote resources are never executed.
- Video thumbnails are generated in process through the media engine. The final package launches no FFmpeg or FFprobe helper process.
- The supported media promise is the tested non-DRM libmpv corpus, not every possible codec variant. Missing, corrupt, encrypted, outside-root, unsupported, and detached-surface inputs return typed errors without hanging.
- Linux ships AppImage and Arch artifacts, macOS ships signed/notarized DMG, and Windows ships signed MSI. Each stages and audits the complete libmpv/PDF runtime dependency closure.
- Migration is staged: freeze fixtures, prove Native SDK prerequisites, extract the Rust core, ship read-only native slices internally, add mutations/documents, add Player parity, then package and delete the transitional shell.
- There is no shipped dual-stack fallback. Tauri remains the production line only until native package gates pass; final cutover deletes it rather than hiding it behind a flag.
- During the transition, TypeScript 7 is the primary command-line checker while TypeScript 6 remains available under its compatibility package for Next.js and ESLint's programmatic API.

## Testing Decisions

- Tests assert observable module behavior through the highest available seam. Source-text boundary tests are used only for architectural constraints that currently lack a runnable scheduler seam.
- Native SDK framework tests cover native-only host selection, unsupported WebView declarations, external-surface lifecycle, fake external effects, record/replay ordering, and Linux/Windows accessibility semantics.
- Rust core tests cover every database migration, transaction, scan rule, Course identity ambiguity, marker behavior, search revision, Progress update, note operation, approved-root check, document conversion, and media metadata extraction.
- C ABI tests cover create/destroy cycles, stale handles, null and malformed input, UTF-8 validation, ownership/release, cancellation, queue pressure, panic containment, event order, and concurrent wake behavior.
- A deterministic scheduler test forces the former Player/platform-main-thread lock ordering and proves the UI executor remains able to service surface work.
- Headless native UI tests drive the real Model/Msg/update loop with fake Rust effects. They cover loading, empty, warning, error, stale-result, keyboard, focus, reduced-motion, and compact/wide layout states.
- Native SDK record/replay tests compare Model fingerprints and deterministic canvas screenshots. Real video frames are represented by a stable placeholder in deterministic captures.
- Live installed-package automation drives Library load, scan, search, Course navigation, Lesson selection, notes, documents, Player controls, fullscreen, error recovery, and quit/reopen Progress resume.
- Media package tests cover H.264/AAC MP4, multi-audio H.264 MKV, HEVC 10-bit, SRT, VTT, chapters, Unicode paths, spaces, long paths, moved Courses, missing files, corrupt files, unsupported codecs, and detached surfaces.
- Playback acceptance requires one app window, in-process libmpv, nonzero meaningful frame dimensions, a first-frame marker, no render error, responsive controls, and no mpv/FFmpeg/FFprobe child process.
- Responsiveness gates measure startup, first usable Library paint, search response, navigation, Player command completion, resize, and shutdown. A watchdog captures thread stacks and diagnostics before killing any hung test.
- Visual review checks the design at compact laptop, standard desktop, and ultrawide sizes in light, dark, and cozy modes. It verifies hierarchy, line length, hit targets, optical alignment, image outlines, and motion behavior.
- Accessibility gates combine semantic snapshots, full keyboard traversal, focus restoration, contrast checks, reduced-motion checks, and installed-package VoiceOver, NVDA, and Orca smoke tests.
- Package tests inspect binary imports and runtime process trees to prove no WebView/browser runtime or helper process remains.
- Database migration tests always operate on copied fixtures and verify rollback/backups before applying new schema versions.
- Existing native Player, startup-route, packaging, database, scanner, identity, stats, and boundary tests remain the transitional oracle until equivalent native-core/full-loop gates replace them.

## Out of Scope

- DRM or encrypted commercial streaming formats.
- Hosted Course distribution, accounts, cloud synchronization, remote telemetry, or streaming services.
- A browser or WebView fallback for documents or UI.
- An external mpv process, FFmpeg playback helper, or second compositor-visible Player window.
- Rewriting proven Rust domain rules in Zig.
- Replacing OpenGL with Metal, Vulkan, or another renderer during the migration.
- Redesigning the product's navigation or feature set before behavioral parity.
- Shipping Linux, macOS, or Windows support based only on compile-time checks.

## Further Notes

- Native SDK is pre-1.0 and must be pinned to an exact source revision.
- The current application-not-responding defect is addressed separately by moving all transitional Tauri Player commands away from the platform UI thread. That repair remains necessary until final cutover.
- The full architecture research is in `docs/research/native-sdk-overhaul.md`; the transitional TypeScript migration research is in `docs/research/typescript-7-migration.md`.
- The accepted design should be saved as a Native SDK token/theme contract once the first native shell is rendered and visually reviewed.
