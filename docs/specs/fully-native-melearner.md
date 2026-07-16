# Fully native melearner

Source map: https://github.com/WhiteHades/melearner/issues/2, with decisions from issues #5, #7, #8, and #9; package implementation is tracked by issue #23.

## Problem Statement

melearner currently depends on a React/WebView/Tauri shell around an embedded libmpv Player. The shipped Linux path can become unresponsive when synchronous Player commands contend with platform-main-thread surface work, and the current architecture makes cross-platform playback, packaging, accessibility, and visual verification harder than they should be. The user wants one responsive, polished, fully native local-first learning app on Linux, macOS, and Windows, with no WebView, browser runtime, external player, or helper process in the final package.

## Solution

Rebuild the entire visible product with Native SDK `.native` views and a Zig Model/Msg/update loop. Retain the proven Rust domain and media logic as an in-process static library behind a versioned C ABI. Complete macOS support for the existing web-layer exclusion contract, add a cross-platform external media surface and Linux/Windows accessibility bridges, and use the pinned Native SDK's implemented bounded application-defined external effects with melearner's existing integration. Preserve the current Library, Course, Lesson, Progress, notes, search, and Player workflows while redesigning their expression as a modern, minimal study instrument.

The production app will be one Native SDK executable. Native SDK owns windows, layout, controls, focus, keyboard behavior, accessibility, animation, and deterministic automation. Rust owns the current SQLite schema, Library scanning, Course identity, search, Progress, Learning activity, notes, document conversion, embedded libmpv, and approved-root validation. All expensive or blocking Rust work is asynchronous from the UI's perspective.

## User Stories

1. As a learner, I want the app to launch into a responsive Library, so that I can resume studying without waiting on an unresponsive window.
2. As a learner, I want the native app to create a clean current Library database, so that obsolete storage formats do not constrain the new architecture.
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
17. As a learner, I want HEVC 10-bit playback to work visibly through the packaged software-capable codec runtime, so that support never depends on optional hardware decoding.
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
28. As a learner, I want timestamped notes to remain attached to Lessons, so that my study context survives app restarts.
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

## Product Interaction Model

### Launch, onboarding, and root selection

- The app opens one main window. While the core opens, the window shows a focusable Library shell and a named busy status rather than a blank surface.
- A fresh native data directory with no root folder opens onboarding. Onboarding explains that each direct child of the selected root can become a Course and offers one primary action: **Choose root folder**.
- Root selection always uses the platform directory picker. Cancel returns to the unchanged screen and writes nothing. An invalid, unreadable, or non-directory selection keeps the previous root and returns a typed root error.
- A valid root candidate starts a scan only after canonicalization succeeds. The root setting and resulting Library revision commit together after a successful or safe partial scan; cancel or fatal failure leaves the previous root and Library unchanged, or leaves first-run onboarding active.
- Changing roots never deletes retained Courses. Courses absent from the committed scan become missing Courses under the durable identity rules.

### Library, missing Courses, and scans

- The Library header contains Search, **Scan root folder** or **Rescan**, and Settings. It shows the current root in a copyable, elided form.
- The resume area contains only available Courses and opens the core-selected resume Lesson. The main virtualized Library contains every Course, including missing Courses, in natural title order. Shelf and list presentation are the same ordered collection, and the chosen presentation persists locally.
- Every Course row exposes name, availability, completed Lessons, total Lessons, and Progress. Opening an available Course enters it. Activating a missing Course opens its recovery panel in the Library; it never opens stale Lesson files.
- A missing-Course recovery panel shows the last known path and missing-since date and offers **Locate Course**, **Rescan root**, and **Change root**. **Locate Course** accepts a directory only and reconnects Progress only through an unambiguous path, marker, or fingerprint match. Ambiguity leaves the Course missing and produces a scan warning.
- Only one scan runs at a time. The Library remains navigable against its last committed revision while a nonmodal status reports the `discovering`, `classifying`, `reconciling`, `committing`, and `writing_markers` phases plus processed and discovered counts. A percentage is shown only when the scanner knows a total.
- A safe partial scan atomically commits the selected root, reconciled Library rows, and one new revision with its recoverable scan and identity warnings. Marker writes begin only after that database commit. They are nontransactional filesystem side effects; a marker-write failure adds a warning to the committed scan result and never rolls the new revision back.
- **Cancel scan** cancels outstanding work before commit and retains the previous root and Library revision. A fatal discovery, reconciliation, validation, storage, or commit failure also commits nothing and offers **Retry**, **Choose root**, and **Copy details**. Once commit begins, cancellation is rejected as too late; the new revision commits and every planned post-commit marker write is attempted.

### Search, Course, and Lesson navigation

- `Ctrl+K` on Linux/Windows and `Cmd+K` on macOS open the Library search dialog from any nonmodal screen and focus its field. Input is debounced by 100 ms; each query cancels or supersedes the previous request by query ID.
- Search matches Course, Section, and Lesson names. Results are grouped as Courses and Lessons; a Section match returns its Lessons with Course/Section context and missing state. Opening an available result closes search and selects that Course or Lesson; opening a missing result closes search and opens Course recovery. `Up`/`Down` move, `Enter` opens, and `Escape` closes and restores focus to the invoker.
- Opening a Course selects an explicitly requested Lesson first, otherwise the core-provided resume Lesson, otherwise the first incomplete Lesson, otherwise the first Lesson. The selected Section expands automatically.
- Sections are disclosure rows and Lessons form one stable ordered tree. Previous/Next traverse that order across Section boundaries. Returning to Library restores scroll and focus to the originating Course. Returning from a compact Lesson restores focus to that Lesson in the outline.
- If a Course or Lesson becomes unavailable while open, playback and document work stop, the last committed Progress remains, and the app returns to the missing-Course recovery state.

### Documents, Player, and notes

- Text and Markdown render as selectable native blocks. Sanitized HTML and DOCX render their supported native blocks and list omitted constructs as document warnings. They never execute script, CSS, or remote resources.
- PDF uses a virtualized continuous-page view with current page/total page status, page jump, **Fit width**, `100%`, zoom in, and zoom out. Scroll and zoom request only visible tiles. Unsupported formats and failed conversions retain Lesson navigation and offer **Open in default app**; external open never marks a Lesson complete.
- The Player loads a playable Lesson paused at its saved position. Video appears in the one in-window media surface; audio uses the same control model with a named audio surface. Controls stay in stable bands and include play/pause, seek, volume, mute, rate, audio and Subtitle tracks, chapters, frame step, screenshot, fullscreen, Previous, and Next.
- Player commands provide immediate pressed/busy feedback and remain operable while load or seek is pending. End of file saves the final position and marks the Lesson complete; moving to the next Lesson remains explicit. HEVC 10-bit must visibly render using packaged software decoding on every supported clean-machine image.
- Player shortcuts are dispatched only when the media/audio surface or an otherwise unclaimed Player-route background owns focus. A focused editable field, menu, dialog, button, slider, list, tree, disclosure, or other control consumes its normal keys first, and the Player handler never receives an already claimed event. With Player shortcut ownership, `Space`/`K` play or pause, `J`/`Left` seek back 10 seconds, `L`/`Right` seek forward 10 seconds, `M` toggles mute, and `F` toggles fullscreen.
- Notes are scoped to the selected Lesson and open in a side panel on wide layouts or a sheet on compact layouts. **New note** captures the current Player position, or position `0` for an untimed Lesson. Notes are ordered by timestamp, then creation order; they can be edited or deleted, and activating a timed note selects it and seeks the Player.

### Stats, settings, and single instance

- Stats remain an inline Library learning ledger. It presents the canonical `revision`, `totalCourses`, `availableCourses`, `missingCourses`, `sections`, `lessons`, `completedLessons`, `completionPercent`, `bytes`, `watchedSeconds`, and `totalSeconds` snapshot fields; `mediaTypes` rows with `type`, `lessons`, `bytes`, `completed`, and `watchedSeconds`; `topCourses` rows with `id`, `name`, `lessons`, `completedLessons`, `bytes`, and `watchedSeconds`; and the fixed 12-week activity rows `date`, `watchedSeconds`, `lessonsTouched`, and `completions`. Snapshot/activity `revision`, `offset`, and `total` values gate stale or paged data and are not rendered as learner metrics. `docs/stats-and-identity-plan.md` owns the field calculations and ordering; this spec owns their ledger presentation.
- Progress time is derived from persisted Lesson positions. The storage fields named `watched_time` and `watched_seconds` represent position-derived Progress and positive position advances, not wall-clock time spent playing. The native app does not add played-time tracking.
- Settings contains the current root and root actions, Light/Dark/Cozy appearance, reduced-motion/high-contrast status inherited from the OS, and build/runtime information. Appearance applies immediately and persists. There is no marker-file setting.
- A second launch never creates another core, database writer, window, or Player. It sends its validated startup route to the existing process, restores and focuses the main window, and then exits. Invalid or missing routes focus Library and show a nonmodal warning.

### Responsive, keyboard, focus, and semantics

- The minimum supported content size is 560x400 logical pixels. Compact mode is 560-767 px, standard mode is 768-1279 px, and wide mode is 1280 px or greater.
- Compact mode uses one primary pane: Library, Course outline, Lesson, notes, and settings are stacked routes or sheets with explicit Back actions. Standard mode uses Course outline plus Lesson content. Wide mode adds the notes/utility pane. Reading text is capped at 80 characters and Player/document surfaces grow without stretching control bands.
- `Tab`/`Shift+Tab` traverse controls, arrow keys move within lists, trees, menus, and radio groups, `Enter` activates, `Space` toggles the focused control, and `Escape` closes the topmost dialog/sheet before navigating back. `Alt+Left` on Linux/Windows and `Cmd+[` on macOS navigate back when no modal is open.
- Async completion never moves focus. Closing a dialog restores its invoker; Back restores the originating row; deleting a note focuses the next note or **New note**; a removed virtual row focuses the nearest surviving row. Keyboard focus always has a visible high-contrast ring.
- Library and search collections expose list/listitem or listbox/option semantics with stable set size and position. Course outline exposes tree/treeitem with expanded, selected, current, unavailable, and completion states. Progress uses progressbar name, numeric value, and value text.
- Search and settings are named dialogs; scan state is a polite live status; terminal errors are alerts; warning summaries are status regions. Player buttons and sliders expose name, value, range, pressed/disabled/busy state, and actions. Document pages expose document/page names, and notes expose list items with timestamp and edit/delete actions.
- Every image has a useful name or is decorative, no state relies on color alone, targets are at least 40x40 logical pixels, text and controls meet WCAG AA contrast, OS text scaling does not clip at 200%, and reduced motion removes all nonessential transitions.

### Native state, message, effect, and view contract

The final native implementation uses one root `Model` with these exact logical fields: `app: AppModel`, `navigation: NavigationModel`, `root: RootModel`, `library: LibraryModel`, `search: SearchModel`, `course: CourseModel`, `lesson: LessonModel`, `player: PlayerModel`, `document: DocumentModel`, `notes: NotesModel`, `stats: StatsModel`, `settings: SettingsModel`, and `overlay: OverlayModel`.

- `AppModel` owns appearance, platform capabilities, window state, single-instance activation, and fatal core state.
- `NavigationModel` owns `Route` (`onboarding`, `library`, `course`, or `lesson`), the compact-mode back stack, origin IDs, scroll restoration, and focus restoration. `lesson` is a separate route only in compact mode; standard and wide layouts render it inside `course`. Settings opens through `OverlayModel` as a dialog or compact sheet rather than creating another route owner.
- `RootModel` owns the committed root, an uncommitted picker candidate, root validation, and first-run state. `LibraryModel`, `SearchModel`, `CourseModel`, `LessonModel`, and `StatsModel` hold only bounded pages or selected projections with their revision, request ID, loading state, warnings, and typed error.
- `PlayerModel` owns coarse controls, tracks, chapters, pending command IDs, saved position, and media-surface diagnostics, but never libmpv or decoded frames. `DocumentModel` owns the normalized visible block/page range and bounded tile IDs. `NotesModel` owns the selected Lesson's bounded note page and edit draft.
- `SettingsModel` owns local UI preferences and read-only runtime/build information. `OverlayModel` owns the active search dialog, recovery panel, warning summary, confirmation dialog, sheet, or menu and the invoker to restore.

`Msg` is one tagged union with the exact families `AppMsg`, `NavigationMsg`, `RootMsg`, `LibraryMsg`, `SearchMsg`, `CourseMsg`, `LessonMsg`, `PlayerMsg`, `DocumentMsg`, `NotesMsg`, `StatsMsg`, `SettingsMsg`, `OverlayMsg`, and `CoreMsg`. User actions, platform events, timers, accessibility actions, and core completions enter through one of these families. `CoreMsg` carries validated external-effect completions with adapter ID, operation, request ID, sequence, schema version, outcome, and bounded payload; `update` discards a completion whose request ID, revision, route owner, or query ID is stale.

`update(model, msg, effects)` is the only mutation path. It may enqueue typed `core`, `platform`, `window`, `timer`, `clipboard`, `external_open`, and `focus` effects, but it performs no disk, SQLite, document, or Player work directly. Each asynchronous effect has a stable operation and request ID, cancellation is explicit, and every terminal result returns as a `Msg` on the Native SDK event-loop thread. Views emit intent messages only and never call Rust, mutate the Model, or retain effect payload memory.

The view hierarchy is normative:

```text
RootView
  OnboardingView
  AppShellView
    LibraryView
      ResumeView
      CourseCollectionView
      StatsLedgerView
      ScanStatusView
    CourseView
      CourseOutlineView
      LessonView
        PlayerView | DocumentView | UnsupportedLessonView
        NotesView
  OverlayHostView
    SearchDialogView | SettingsView | CourseRecoveryView | WarningSummaryView
    ConfirmationDialogView | SheetView | MenuView
```

`RootView` chooses onboarding or the app shell from committed state and always mounts one overlay host. Responsive composition may move the same logical view between a route, pane, dialog, or sheet, but it does not create a second state owner or alternate message path.

### Typed state and recovery contract

- Every async surface has explicit `loading`, `empty`, `ready`, `warning`, `recoverable_error`, and `fatal_error` variants as applicable. Errors carry `code`, `scope`, `operation`, optional local path, retryability, user message, diagnostic ID, and an ordered set of safe actions.
- Recovery never imports a pre-native or obsolete-schema database, launches a browser fallback, starts a helper process, or silently discards the last committed Library revision.

| Code family | Required UI and recovery |
| --- | --- |
| `root.required`, `root.unavailable`, `root.denied` | Onboarding or root banner; choose/retry root without changing committed data. |
| `library.open_failed`, `storage.read_only`, `storage.full` | Keep the shell responsive; retry or quit, and copy details. Never replace the database silently. |
| `scan.cancelled` | Before commit, retain the prior root and revision and show the cancellation summary. Once commit begins, cancellation is rejected as too late and the operation completes against the new revision. |
| `scan.partial` | Install the new root and revision atomically, keep safe results, and show the persistent warning summary. Marker-write warnings are part of this committed result. |
| `scan.failed` | Retain the prior root and revision; show details and offer retry, rescan, or choose root as applicable. |
| `course.missing`, `lesson.missing`, `lesson.outside_root` | Stop file work and offer locate Course, rescan root, change root, Back, or Next. |
| `search.stale`, `search.failed` | Discard stale results automatically; keep the query and offer retry/rebuild for a real failure. |
| `document.unsupported`, `document.decode_failed`, `document.runtime_failed` | Keep navigation; retry when safe or open the local file in the default app. |
| `player.unsupported_codec`, `player.decode_failed`, `player.surface_failed`, `player.runtime_failed` | Stop media work; retain saved Progress and offer retry, Back, Next, and copy details. A HEVC 10-bit fixture failure blocks release instead of using this state as acceptance. |
| `core.busy`, `core.queue_full` | Keep current data, coalesce replaceable work, and offer retry after capacity returns. |
| `core.failed` | Show a fatal state with restart and quit only; no alternate runtime is started. |

## Implementation Decisions

- The final release uses Native SDK `.native` views and Zig logic only. It ships no WebView, browser engine, React, Tauri, Node runtime, or JavaScript bridge.
- `melearner_core` is built as both a static library and a Rust library. The Tauri Cargo crate already depends on that Rust crate and directly reuses its scanner; this existing direct Rust reuse remains frozen and regression-tested until cutover. The final-native app uses the versioned C ABI, but the transitional shell must not be redirected through that ABI, native database ownership, Native SDK effects, or Native SDK UI.
- The Zig/Rust seam is a versioned C ABI with opaque handles, fixed-width values, explicit byte ownership, panic barriers, bounded queues, cancellation, and a thread-safe empty-to-nonempty waker.
- The UI never blocks on Rust. Requests return IDs immediately; ordered completion events enter the Model only on the Native SDK event-loop thread.
- SQLite keeps one writer owner. The first native release, and any future schema replacement, creates one fresh current schema at a distinct data path and never inspects or imports pre-native or obsolete-schema databases. Native-to-native package replacement reopens the current native database only while that exact schema remains current; there is no migration, backup, restore, rollback, or compatibility path.
- Library, Lesson, search, activity, and document data are paged by stable revision. The Zig Model holds only visible pages and selected detail, not a duplicate canonical Library graph.
- Native SDK gains a native-only host mode that does not compile, link, load, or stage WebKit, WebKitGTK, WebView2, or CEF.
- Native SDK gains a generic external media-surface module on Linux, macOS, and Windows. The SDK owns the native child, layout, clipping, focus, visibility, scale, fullscreen relayout, and current graphics context.
- Rust owns `mpv_render_context` and renders only from the SDK's platform draw callback while the context is current. Control commands and lifecycle events remain asynchronous.
- The media surface exposes attached state, visibility, backend, physical dimensions, rendered-frame count, first-frame time, update flags, and last error to automation and diagnostics.
- The pinned Native SDK implements bounded application-defined external effects with live/fake adapters and record/replay, and melearner already integrates them. Bulk video pixels never cross the effect queue.
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
- The supported media promise is the tested non-DRM libmpv corpus, including required visible HEVC 10-bit software decoding, not every possible codec variant. Missing, corrupt, encrypted, outside-root, unsupported, and detached-surface inputs return typed errors without hanging.
- Linux ships AppImage and Arch artifacts, macOS ships signed/notarized DMG, and Windows ships signed MSI. Each stages and audits the complete libmpv/PDF runtime dependency closure.
- Cutover is staged: freeze current-domain fixtures, prove Native SDK prerequisites, stabilize the Rust core while preserving the frozen direct Tauri scanner reuse, ship read-only native slices internally, add mutations/documents, add Player parity, then package and delete the transitional shell.
- There is no shipped dual-stack fallback. Tauri remains the production line only until native package gates pass; final cutover deletes it rather than hiding it behind a flag.
- During the transition, TypeScript 7 is the primary command-line checker while TypeScript 6 remains available under its compatibility package for Next.js and ESLint's programmatic API.

## Testing Decisions

- Tests assert observable module behavior through the highest available seam. Source-text boundary tests are used only for architectural constraints that currently lack a runnable scheduler seam.
- Native SDK framework tests cover native-only host selection, unsupported WebView declarations, external-surface lifecycle, fake external effects, record/replay ordering, and Linux/Windows accessibility semantics.
- Rust core tests cover the current schema, every transaction, scan rule, Course identity ambiguity, marker behavior, search revision, Progress update, note operation, approved-root check, document conversion, and media metadata extraction.
- C ABI tests cover create/destroy cycles, stale handles, null and malformed input, UTF-8 validation, ownership/release, cancellation, queue pressure, panic containment, event order, and concurrent wake behavior.
- A deterministic scheduler test forces the former Player/platform-main-thread lock ordering and proves the UI executor remains able to service surface work.
- Headless native UI tests drive the real Model/Msg/update loop with fake Rust effects. They cover loading, empty, warning, error, stale-result, keyboard, focus, reduced-motion, and compact/wide layout states.
- Native SDK record/replay tests compare Model fingerprints and deterministic canvas screenshots. Real video frames are represented by a stable placeholder in deterministic captures.
- Live installed-package automation drives Library load, scan, search, Course navigation, Lesson selection, notes, documents, Player controls, fullscreen, error recovery, and quit/reopen Progress resume.
- Media package tests cover H.264/AAC MP4, multi-audio H.264 MKV, HEVC 10-bit, SRT, VTT, chapters, Unicode paths, spaces, long paths, moved Courses, missing files, corrupt files, unsupported codecs, and detached surfaces.
- Playback acceptance requires one app window, in-process libmpv, nonzero meaningful frame dimensions, a first-frame marker, no render error, responsive controls, and no mpv/FFmpeg/FFprobe child process.
- Responsiveness gates measure startup, first usable Library paint, search response, navigation, Player command completion, resize, and shutdown. A watchdog captures thread stacks and diagnostics before killing any hung test.
- The numeric budgets, separate hard hang timeouts, deterministic fault-injection matrix, and clean-package evidence bundle are normative in `docs/research/native-sdk-overhaul.md`.
- Visual review checks the design at compact laptop, standard desktop, and ultrawide sizes in light, dark, and cozy modes. It verifies hierarchy, line length, hit targets, optical alignment, image outlines, and motion behavior.
- Accessibility gates combine semantic snapshots, full keyboard traversal, focus restoration, contrast checks, reduced-motion checks, and the installed-package VoiceOver, NVDA, and Orca scripted checks defined by `fixtures/parity/accessibility-manifest-v1.json`.
- Package tests inspect binary imports and runtime process trees to prove no WebView/browser runtime or helper process remains.
- Fresh-schema tests create isolated empty databases and verify the exact current tables, constraints, and foreign keys without migration, backup, restore, or rollback paths.
- Existing Tauri Player, startup-route, packaging, scanner, identity, stats, and boundary tests remain frozen regression gates through Stage 6, including the direct Rust reuse of the core scanner. Native-core and full-loop gates are additive before cutover; they do not redirect the transitional shell through the C ABI, native database ownership, Native SDK effects, or Native SDK UI.

## Out of Scope

- DRM or encrypted commercial streaming formats.
- Hosted Course distribution, accounts, cloud synchronization, remote telemetry, or streaming services.
- A browser or WebView fallback for documents or UI.
- An external mpv process, FFmpeg playback helper, or second compositor-visible Player window.
- Rewriting proven Rust domain rules in Zig.
- Replacing OpenGL with Metal, Vulkan, or another renderer during the cutover.
- Redesigning the product's navigation or feature set before behavioral parity.
- Shipping Linux, macOS, or Windows support based only on compile-time checks.

## Further Notes

- Native SDK is pre-1.0 and must be pinned to an exact source revision.
- The current application-not-responding defect is addressed separately by moving all transitional Tauri Player commands away from the platform UI thread. That repair remains necessary until final cutover.
- The full architecture research is in `docs/research/native-sdk-overhaul.md`; the transitional TypeScript migration research is in `docs/research/typescript-7-migration.md`.
- The accepted design should be saved as a Native SDK token/theme contract once the first native shell is rendered and visually reviewed.
