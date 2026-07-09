# Native SDK overhaul for melearner

Research date: 2026-07-10. Scope: the Native SDK documentation and source under `/home/efaz/Codes/native`, the current melearner UI and Rust/Tauri implementation, ADRs 0008-0010, and release workflows. This is an architecture and migration decision; no application or Native SDK source changes were applied.

## Conclusion

Proceed with the Native SDK UI and retained Rust core architecture, but do not begin by rewriting screens. The final product should be one Native SDK executable with all UI authored in `.native` markup and Zig, linked to an in-process Rust `staticlib` through a versioned C ABI. Rust remains the sole owner of SQLite, migrations, scanning, course identity, search, progress, notes, document decoding, and embedded libmpv. There is no Tauri, React, Node runtime, browser engine, WebView, or helper process in the shipped app. Bundled dynamic libraries such as libmpv and a PDF renderer are in-process dependencies, not sidecars.

This is a conditional go because the current Native SDK cannot yet host the product as specified. Four upstream capabilities are release blockers:

1. A native-only desktop host/build mode that does not compile or link WebKit, WebKitGTK, WebView2, or CEF.
2. A cross-platform external media-surface widget whose lifecycle, layout, clipping, visibility, and rendering are owned by the Native SDK.
3. An app-defined asynchronous effect source that participates in the SDK's fake executor and session record/replay machinery.
4. OS accessibility bridges for canvas widgets on Linux and Windows, equivalent in purpose to the existing macOS bridge.

The first engineering milestone is therefore an upstream foundation spike, not a melearner screen. It must render a libmpv test pattern inside one Native SDK window on Linux, macOS, and Windows, with no WebView dependency in the binary. If that milestone fails, stop the migration. Do not work around it with a second window, pixel copies through `gpu_surface`, a private overlay unknown to the SDK, or a player sidecar.

## Decision boundaries

The final ownership boundary is:

```text
Native SDK executable (Zig)
  .native views, Model, Msg, update
  navigation, selection, drafts, transient UI state
  native dialogs, menus, shortcuts, accessibility, automation
  external media-surface layout and OpenGL context
             |
             | versioned C ABI, in process
             v
melearner_core static library (Rust)
  SQLite connection, migrations, transactions
  Library scans, durable course identity, marker files
  search index, progress, activity, notes
  document parsing and PDF page rendering
  libmpv lifecycle, commands, events, render context
             |
             v
local files and bundled in-process native libraries
```

The boundary has these non-negotiable rules:

- The Zig UI never executes SQL, scans directories, computes course identity, calls libmpv directly, or owns domain persistence.
- Rust never owns an application window, a second event loop, navigation state, or a user-visible control.
- Every operation that can touch disk, SQLite, search indexes, document parsing, or libmpv is asynchronous from the UI's perspective.
- Only the Native SDK event-loop thread mutates the Zig `Model`.
- Rust callbacks may only enqueue an event and invoke the registered thread-safe waker. They must not call `update`, render UI, or retain Zig memory.
- Paths remain local and must resolve beneath an approved root before Rust reads or plays them. URLs and schemes remain rejected, preserving ADR 0010.
- The existing SQLite database and `.melearner-course.json` identity markers are user data and must be reused, not re-imported into a parallel store.

ADR 0010 remains authoritative for embedded libmpv, one compositor-visible app window, local-file validation, playback controls, and the packaged codec matrix. This report supersedes only its React/Tauri transport and WebView layout statements. ADR 0008 remains authoritative for conservative course and lesson identity. ADR 0009 requires the old frontend, Tauri shell, superseded packaging, and generated leftovers to be deleted in the cutover change once the replacement is verified.[13][14][15]

## Current Native SDK assessment

The Native SDK is a strong fit for the application shell. Its `.native` markup, typed `Model`/`Msg`/`update` loop, compiled release markup, virtual lists, headless layout tests, live automation snapshots, deterministic screenshots, and session record/replay directly address melearner's navigation and testing needs.[2][3][4][5] The `examples/notes` application also proves a multi-pane, keyboard-oriented, persistent desktop UI shape rather than only a demo counter.[12]

It is not yet a complete fit for media, strict binary composition, document viewing, or cross-platform accessibility.

| Area | Current evidence | Decision |
| --- | --- | --- |
| Native desktop UI | Native SDK renders `.native` views into real OS windows on all three desktop platforms. macOS has the deepest GPU path; Linux and Windows use the software pixel presenter for canvas UI.[1][7][8] | Use for the whole application shell. Test performance against large Library fixtures before cutover. |
| State model | `UiApp` supplies typed messages, one update path, effects, fake execution, and replay hooks.[3][9] | Keep UI state in Zig and domain state in Rust. Do not mirror the whole database in the model. |
| Large collections | The SDK exposes virtualized list behavior and fixed runtime budgets. Canvas image upload is limited to 1 MiB per image and packet transport is bounded.[2][7] | Page Library/search data from Rust. Keep only visible pages and selected detail in the model. Tile document images to fit declared SDK limits. |
| Embedded media | `gpu_surface` accepts SDK draw packets or CPU RGBA pixels. It is not an app-owned libmpv render target. `adoptViewSurface` is macOS system-host only; Linux and Windows explicitly report no surface-adoption capability.[7][8] | Add an upstream `external_surface`/`media_surface` widget. Do not copy decoded video frames through the canvas image path. |
| Async core integration | Effects cover a fixed set of subprocess, HTTP, file, clipboard, timer, clock, image, window, and audio operations. Results are drained on the UI thread and can be faked or journaled.[9] | Add a first-class external effect adapter for the Rust core. Do not spawn a CLI or poll the core from a timer. |
| Automation | The live server exposes windows, views, semantic widgets, bounds, state, input commands, and deterministic canvas screenshots. Session replay verifies model fingerprints and canvas screenshot hashes.[4][5][10] | Reuse it. Extend snapshots with media-surface diagnostics. Use a deterministic placeholder for video in reference screenshots. |
| OS accessibility | Semantic widget snapshots exist cross-platform, but `update_widget_accessibility_fn` is wired only by the macOS platform source. Linux and Windows expose labels for native controls but do not bridge the canvas widget tree to AT-SPI/UI Automation.[7][8] | Linux and Windows canvas accessibility are release blockers, not post-launch polish. |
| Documents | Native SDK provides text/Markdown primitives and image decoding, but no PDF, DOCX, or general document surface was found.[2][7] | Build a melearner native document adapter. Never hide a WebView behind the document screen. |
| Native-only binary | The public positioning says native UI has no browser or WebView in the binary.[2][6] The current desktop build still selects a `system` web engine: macOS links WebKit, Linux links `webkitgtk-6.0`, and Windows compiles `webview2_host.cpp`, even when `AppInfo.has_web_content` is false.[11] | Add a true `.none`/native-only host path and verify binary imports. Runtime non-use is insufficient. |
| Packaging | macOS packaging creates an `.app`. Linux and Windows packaging currently creates structured artifact directories, not an AppImage/Flatpak/MSI installer. Desktop platform builds require a matching host OS.[6][11] | Keep packaging application-owned initially and build in a native OS CI matrix. Upstream installer support is useful but not a migration blocker. |

The Native SDK is pre-1.0 and its APIs still move.[6] Pin the exact source revision used by melearner. Do not build the application against an unpinned branch.

## Required upstream Native SDK work

### 1. Native-only host mode

Add `WebEngine.none` and make it the default when the manifest declares no web capability or frontend. In that mode:

- macOS compiles an AppKit host without `WKWebView` code and does not link the WebKit framework;
- Linux compiles a GTK4 host without WebKitGTK and does not require `webkitgtk-6.0` at build or runtime;
- Windows compiles a Win32 host without WebView2 code and does not import or initialize WebView2;
- CEF installation, assets, bridge code, navigation policy, and web commands are absent;
- attempts to declare or create a WebView are compile-time or explicit runtime errors;
- `native package` records `web_engine = .none` and does not stage web assets.

This should be capability-driven rather than a melearner-specific fork. The acceptance test is not merely "no WebView created." `otool -L`, `readelf`/`ldd`, and PE import inspection must show no WebKit, WebKitGTK, WebView2, or CEF dependency in a canvas-only packaged app.

### 2. External media surface

Add a declarative canvas widget, provisionally `<external-surface kind="media" ...>`, and a matching platform service. The SDK must own the native child container and its place in the widget tree. Its contract must include:

- stable surface ID and window ID;
- attach, ready, resize, scale-change, visibility, occlusion, detach, and destroy lifecycle events;
- clipping to widget bounds, layout-driven z-order, one-window composition, focus behavior, and fullscreen relayout;
- a host-created OpenGL context and framebuffer on Linux GTK4, macOS AppKit, and Windows Win32/WGL;
- a render callback invoked only while the context is current, carrying framebuffer, physical width/height, scale, and vertical-flip requirements;
- a thread-safe invalidate/request-frame function that libmpv's update callback can call from any thread;
- explicit diagnostics: attached, visible, backend, physical dimensions, rendered frame count, first-frame timestamp, last update flags, and last error;
- null-platform behavior that reports lifecycle deterministically and renders a stable placeholder without initializing libmpv;
- automation snapshot representation with bounds, state, diagnostics, and an accessible name.

The first implementation should retain the already proven OpenGL libmpv render-api shape from `src-tauri/src/native_player/surface/`: GTK `GLArea`, AppKit `NSOpenGLView`, and child HWND/WGL.[13] Native SDK must own those host contexts because its Linux host is GTK4 while the current Tauri surface uses GTK3; passing a GTK3 widget into the GTK4 tree is not viable. Rust should own `mpv_render_context` and expose attach/render/detach functions, while the SDK owns the context and calls Rust's render function from the platform draw callback.

Metal or Vulkan can replace OpenGL later, but that is not part of this migration. A speculative renderer rewrite would combine two high-risk changes and discard the current cross-platform evidence.

### 3. External asynchronous effects

Extend `UiApp` effects with an application-defined adapter rather than adding melearner operations to Native SDK itself. The adapter needs four modes:

| Mode | Required behavior |
| --- | --- |
| Live | Submit/cancel Rust requests, drain ordered completions after a core wake, and map them to typed `Msg` values on the UI thread. |
| Fake | Record requests without Rust, allow tests to inject outcomes, and expose overflow/rejection state. |
| Record | Write bounded external-effect results into the existing session journal before the consuming wake event, preserving current ordering. |
| Replay | Park matching requests and feed journaled outcomes without touching SQLite, files, PDFium, or libmpv. |

The external result record should contain adapter ID, request key, outcome, schema version, and bounded bytes. Over-budget results must fail recording loudly, following the existing recorder contract. Bulk media frames are never effect payloads. Library/search responses are paged, PDF tiles use the image registry, and video is represented by deterministic metadata plus a placeholder during replay.

### 4. Linux and Windows canvas accessibility

Implement the existing `WidgetAccessibilitySnapshot` service on Linux and Windows:

- Linux maps the semantic tree, focus, actions, values, selection, and text state to GTK4 accessibility/AT-SPI.
- Windows exposes the same information and actions through UI Automation.
- focus and actions flow back as the existing `widget_accessibility_action` event shape.
- virtualized rows report stable list position/count without materializing the full Library.
- the external media surface has a role/name and does not swallow keyboard focus from surrounding controls.

Automation semantics are necessary but do not prove screen-reader integration. Release verification must include VoiceOver, NVDA, and Orca smoke tests on installed packages.

### 5. Packaging improvements that can follow in parallel

Native SDK should eventually emit a Windows installer and Linux portable package rather than artifact directories. melearner should not wait for that work: its existing release workflows already contain AppImage, Arch package, MSI signing, and Windows libmpv dependency-staging knowledge.[16] Port that logic to the new binary first, then upstream general pieces that do not encode melearner-specific runtime libraries.

## Rust core design

Create a dedicated crate at `crates/melearner-core` with `crate-type = ["staticlib", "rlib"]`. The `rlib` supports Rust tests and internal tools; the Native SDK executable links the `staticlib`. Move reusable code out of `src-tauri` instead of wrapping Tauri commands around the new ABI. The final crate has no `tauri`, `tauri-plugin-*`, Tokio UI runtime, JavaScript bridge, or WebView types.

The core should contain these modules:

| Module | Responsibility |
| --- | --- |
| `db` | Open the existing database path, apply migrations 1-16 and future migrations transactionally, enforce foreign keys/WAL/busy timeout, and serialize writes. |
| `library` | Load paged Library snapshots and aggregate stats; coordinate scans and transactional reconciliation. |
| `identity` | Preserve ADR 0008 matching order, ambiguity warnings, relative lesson paths, fingerprints, missing courses, and automatic marker files. |
| `scanner` | Preserve supported extensions, ignored/partial-file behavior, natural ordering, warnings, and approved-root canonicalization. |
| `search` | Own index construction, invalidation, and paged query results. |
| `progress` | Update lesson progress, activity rows, completion, and last-accessed timestamps atomically. |
| `notes` | List, create, update, and delete timestamped notes with existing validation. |
| `documents` | Read approved local files, normalize text/Markdown/HTML/DOCX into a native document model, and render PDF tiles. |
| `player` | Own libmpv, local-path validation, tracks, chapters, commands, screenshots, events, and the render context. |
| `ffi` | Contain every `extern "C"` function, panic barrier, handle table, byte ownership rule, and ABI test. |

Use one core coordinator with a dedicated worker for ordered database/domain commands and a separate libmpv event/render path. Scanning and document rendering may use bounded worker jobs, but their completion is serialized through one event queue. SQLite has one writer owner. The UI must not observe half a scan transaction.

The model should not receive the complete Library as one unbounded payload. Rust remains authoritative and returns pages keyed by a revision:

```text
LibraryRevision = monotonically increasing u64
CoursePage       = revision + offset + total + rows
LessonPage       = revision + course_id + section_id + offset + total + rows
SearchPage       = query_id + index_revision + offset + total + rows
```

Any mutation that changes visible domain data returns a new revision. Zig discards stale pages whose revision or request ID no longer matches the active route/query.

## C ABI contract

The ABI is intentionally narrow and C-shaped. Generate `include/melearner_core.h` from Rust, check it into source control, and fail CI when regeneration changes it unexpectedly. The header is an active interface artifact, not disposable generated output.

### Lifecycle and transport

```c
uint32_t ml_abi_version(void);
ml_status_t ml_core_create(const ml_config_v1 *, ml_core_t **out_core);
void ml_core_destroy(ml_core_t *core);
ml_status_t ml_core_set_waker(ml_core_t *, ml_wake_fn, void *context);
ml_status_t ml_core_poll_event(ml_core_t *, ml_event_v1 *out_event);
void ml_core_release_event(ml_core_t *, ml_event_v1 *event);
ml_status_t ml_core_cancel(ml_core_t *, uint64_t request_id);
```

`ml_config_v1`, `ml_event_v1`, and every future public struct begin with `struct_size` and `abi_version`. Public integers use fixed widths. Booleans are `uint8_t`. Strings and arbitrary bytes use `{const uint8_t *ptr; size_t len;}` and are UTF-8 where declared. Rust enums never cross the boundary directly.

`ml_event_v1` contains a monotonically increasing sequence, request ID, event kind, status, payload schema version, payload pointer, and payload length. Domain payloads use versioned UTF-8 JSON initially because Zig has a maintained parser and these responses are low-frequency, paged data. Video frames and raw audio never use JSON or the event queue. The event remains Rust-owned until `ml_core_release_event`; Zig must copy only the fields it retains in `Model`.

Inputs are borrowed only for the duration of the call. Rust must copy accepted input before returning. The callback function and context remain borrowed until cleared or core destruction. Rust catches panics at every exported entry point; no unwind may cross C. A caught panic marks the core failed, emits one fatal event if possible, rejects further work, and still permits destruction.

The waker fires only on an empty-to-nonempty queue transition. It calls the Native SDK's thread-safe wake service and does no other work. On `.wake`, the UI thread drains events until empty, maps each event to `Msg`, and returns through normal `update` dispatch. Event sequence is global so tests can prove ordering.

The queue is bounded and loss behavior is typed:

- replaceable player-position and scan-progress events coalesce by operation;
- terminal request results, errors, file-loaded/end-file, and identity warnings are never silently dropped;
- when pressure remains, new submissions return `ML_STATUS_BUSY` rather than allocating without bound;
- one explicit overflow event reports coalesced/dropped nonterminal counts;
- the queue capacity and maximum payload are returned by `ml_core_limits_v1` and pinned in load tests.

### Typed request families

Do not expose one stringly `invoke` function. Export typed asynchronous request families whose accepted call returns a request ID:

| Family | Operations |
| --- | --- |
| Library | open current Library, scan root, load course/lesson page, refresh stats, write markers |
| Search | build/rebuild index, query page, cancel query |
| Progress | update position/completion, mark course accessed, list activity days |
| Notes | list, save, delete |
| Documents | open, close, load normalized block page, render/release PDF tile |
| Player | load, play, pause, seek, volume, mute, rate, select audio/subtitle/chapter, frame step, screenshot, destroy |
| Surface | attach render target, render current frame, set visibility, detach, query diagnostics |

High-level examples are `ml_library_scan_v1`, `ml_search_query_v1`, `ml_progress_put_v1`, `ml_notes_save_v1`, and `ml_player_load_v1`. Names and structs are version-suffixed when their shape changes. Additive event kinds and JSON fields are allowed within ABI v1; incompatible ownership or struct changes require ABI v2.

### Media render seam

The surface seam is synchronous only inside the SDK's platform render callback:

```text
SDK media surface becomes ready with a current OpenGL context
  -> ml_player_surface_attach_v1(get_proc_address, surface_id)
libmpv update callback on any thread
  -> SDK thread-safe request_surface_frame(surface_id)
SDK draw callback with context current
  -> ml_player_surface_render_v1(surface_id, fbo, width_px, height_px, flip_y)
SDK hides/resizes/destroys widget
  -> visibility metadata or ml_player_surface_detach_v1(surface_id)
```

The render function performs only `mpv_render_context_render` and diagnostics. It must not lock the database worker or dispatch UI events. Control commands remain asynchronous. A missing, zero-sized, detached, or failed surface prevents visible media load and returns a clear error, preserving ADR 0010's fail-closed behavior.

## Native UI model

The Zig `Model` owns only what the current view needs:

- route and back-stack;
- selected course, section, lesson, note, and search result IDs;
- visible Library/search/activity pages and their revisions;
- loading, empty, warning, and error state;
- edit drafts and dialog state;
- coarse player state, tracks, chapters, and surface diagnostics;
- document viewport, visible block/page IDs, and tile IDs;
- appearance, window chrome, and focus state.

The model does not own a second canonical course graph. Course and lesson rows are projections from the current Rust revision. Mutations update Rust first and then install the returned revision/page. This avoids divergent identity logic and keeps large libraries compatible with fixed canvas budgets.

Use Native SDK virtual lists for courses, sections, lessons, search, notes, and activity. IDs come from Rust and remain keyed across rebuilds. Every interactive state must be represented by a typed `Msg`; direct mutable callbacks and global player singletons do not cross into Zig.

The first player layout should continue ADR 0010's stable-band rule: native controls sit in SDK-rendered rows above or below the media surface rather than in a transparent canvas overlay over the platform child. Fullscreen changes the same app window state and relayouts the surface; it does not create a second window or invoke DOM fullscreen.

## Document strategy without a WebView

The current app directly renders text, Markdown, HTML, PDF, and DOCX, and opens unsupported documents externally. That behavior cannot be preserved by Native SDK alone.[17] Implement this explicit replacement:

| Input | Native implementation |
| --- | --- |
| `.txt` | Rust returns bounded UTF-8 block pages; Native SDK renders selectable text. |
| `.md`, `.markdown` | Rust reads the file; Native SDK's Markdown/document primitives render headings, lists, links, and code blocks. |
| `.html`, `.htm` | Rust parses local HTML into a sanitized document AST. Ignore scripts, remote resources, CSS execution, and active content. Render supported blocks natively and expose an "Open in default app" action for unsupported fidelity. |
| `.docx` | Rust parses ZIP/XML into the same document AST, including paragraphs, headings, lists, tables, and embedded local images where supported. Report omitted constructs rather than rendering arbitrary HTML. |
| `.pdf` | Rust uses a pinned cross-platform PDFium build to render visible pages into 512x512 RGBA tiles, each at or below the SDK's current 1 MiB image limit. Native SDK virtualizes pages and keeps a bounded visible-tile cache. |
| `.doc` and unknown formats | Preserve the current external-open fallback. Do not claim direct rendering. |

All document reads use the same approved-root validation as playback. HTML never gets network access or script execution. PDFium is bundled as an in-process dynamic library and included in package dependency audits. If PDFium packaging or licensing review is not accepted, direct PDF rendering becomes a named product-scope decision before cutover, not a reason to reintroduce a WebView.

Generate video thumbnails in process through libmpv's screenshot/render path. Do not launch `ffmpeg` or `ffprobe` helpers in the final package. ADR 0010 permits FFmpeg for optional processing, but the stronger final no-sidecar decision removes helper processes from this architecture.

## Migration plan and gates

### Stage 0: freeze behavior and fixtures

Record the existing contract before changing ownership:

1. Copy an anonymized migrated SQLite fixture covering migrations 1-16, progress, notes, activity, missing courses, marker identity, fingerprint matches, and ambiguous matches.
2. Keep the existing scanner fixture set, including partial downloads, ignored folders, Unicode names, long paths, subtitles, duplicate markers, and duplicate fingerprints.
3. Keep ADR 0010's codec corpus and installed-package verification scripts as the playback oracle.
4. Capture automation-level flows for initial Library load, scan, search, course navigation, progress resume, notes, document open, and error recovery.
5. Record package dependency manifests for each OS so removal of browser libraries can be proven later.

Gate: the current implementation passes its tests and the fixtures are reproducible without personal Library data.

### Stage 1: prove Native SDK prerequisites upstream

Implement native-only hosts, the media-surface widget, external effects, and Linux/Windows accessibility in `/home/efaz/Codes/native`. Build a small fixture app, not melearner, that:

- has one canvas window and no WebView capability;
- embeds a test libmpv render context through the proposed surface seam;
- resizes, hides, restores, and fullscreens the surface;
- exposes diagnostics in automation snapshots;
- records and replays a fake external completion;
- is keyboard and screen-reader operable;
- packages and starts on all three operating systems without a browser import.

Gate: all four upstream blockers pass on packaged builds. Failure stops the overhaul.

### Stage 2: extract and stabilize `melearner_core`

Move database migrations, database operations, scanner, identity, marker writing, search, progress/activity, notes, document adapters, and player control behind a safe Rust API and the versioned C ABI. Keep current table and marker formats. Add Rust tests for every ambiguity and migration case, C ABI ownership tests, panic tests, queue-pressure tests, and a Zig smoke client.

Gate: the core opens a copy of an existing database without data loss, produces identical scan/identity/search results for fixtures, and passes sanitizer/ABI tests. No UI is needed for this gate.

### Stage 3: build a read-only Native SDK vertical slice

Create `native-app/app.zon`, `native-app/src/main.zig`, and `.native` views for startup, Library, course, lesson, and search. Connect them through the external-effect adapter to paged core results. Include loading, empty, missing-course, warning, and error states from the start.

Gate: a 100,000-lesson fixture remains responsive through virtual lists; stale request results are rejected by revision/request ID; headless tests and live automation cover keyboard navigation on all three platforms.

### Stage 4: mutations, stats, notes, and documents

Add scan selection, progress/activity, stats, course access, notes, automatic marker writes, text/Markdown/HTML/DOCX normalization, PDF tiling, and external-open fallback. Keep every write in Rust transactions.

Gate: the database fixture remains compatible, identity behavior exactly matches ADR 0008, stats match current outputs, and direct document formats pass installed-package smoke tests without network or WebView use.

### Stage 5: embedded playback

Move the current native-player state machine and event extraction into `melearner_core`, connect it to the upstream media surface, and recreate controls in `.native` UI. Preserve tracks, chapters, subtitles, screenshots, coarse position updates, resume, visibility, and fail-closed surface attachment.

Gate: every ADR 0010 packaged visual test passes on Linux, macOS, and Windows. Diagnostics report a live render thread, nonzero frames and dimensions, and no render error. OS window inspection reports one melearner window. Process inspection reports no mpv, ffmpeg, or ffprobe child process.

### Stage 6: package, cut over, and delete

Replace release workflows with the Native SDK/Cargo build matrix and package the in-process runtime libraries. Back up the user database before the first native build applies any new migration. After all installed-package gates pass, remove Next.js, React, shadcn, Tauri, frontend assets, JavaScript bridges, Node scripts that no longer apply, old release jobs, and obsolete generated artifacts in the same cutover change.

Gate: source and binary scans find no Tauri, WebView, WebKit, WebView2, CEF, React, or browser-runtime path; all user-visible flows pass; the old shell is absent rather than dormant behind a flag.

There is no shipped dual-stack fallback. Until Stage 6, the existing Tauri release remains the production line while the native app is an unreleased build target. After Stage 6, rollback is a release rollback using the pre-migration database backup, not runtime compatibility code.

## Codec and playback acceptance matrix

The discovery extension list is broader than the guaranteed codec corpus. A recognized `.avi`, `.webm`, `.flac`, or other extension means "show this learning item," not "every codec variant in this container is supported." The release guarantee is the tested corpus below plus any later fixture added deliberately.

| Case | Required result |
| --- | --- |
| MP4 H.264/AAC | First frame, audio, seek, pause/resume, progress persistence |
| MKV H.264 with multiple audio tracks | Track list and live switching without reload |
| HEVC 10-bit | Visible playback or a named unsupported hardware/software decode failure on the clean test machine; no silent black surface |
| External SRT and VTT | Registration, selection, rendering, and Unicode text |
| Chapters | Ordered list, current chapter, and selection |
| Playback controls | Absolute/relative seek, volume, mute, rate, frame step, screenshot, fullscreen |
| Paths | Unicode, spaces, long path, renamed/moved course, deleted file |
| Failures | Missing, outside-root, corrupt, unsupported, and detached-surface cases return typed errors |
| Architecture | One app window, in-process libmpv, render-api surface, no normal-playback FFmpeg process |

HEVC wording must be resolved before release: ADR 0010 currently lists the file as a pass case, so the preferred gate is successful visible playback on each clean-machine image. If hardware and bundled software decoding cannot guarantee that, update the ADR and product support statement explicitly rather than weakening the test silently.

## Packaging and release design

Build on the target operating system because the current Native SDK build graph rejects a selected desktop platform that differs from the target host.[11]

| Platform | Artifact | In-process runtime staging | Release checks |
| --- | --- | --- | --- |
| macOS | Signed/notarized `.app` in DMG | libmpv and transitive dylibs under `Contents/Frameworks`; PDFium alongside them; corrected install names/rpaths; sign libraries before app | `codesign --verify --deep --strict`, `spctl`, notarization, `otool -L`, clean-machine launch and playback |
| Windows | Signed MSI | `melearner.exe`, `libmpv-2.dll`, its dependency closure, and PDFium DLL in the install directory; reuse the current staging assertions as a baseline | PE import scan, signature verification, install/uninstall, non-admin launch, clean-machine playback |
| Linux | AppImage plus Arch package | AppImage bundles libmpv/PDFium and non-baseline dependencies with `$ORIGIN` RUNPATH; Arch package may depend on distro `mpv`/libmpv and PDFium if pinned packaging exists | `readelf`/`ldd`, AppImage extraction audit, Wayland and X11 launch, package install/remove, playback |

Do not bundle glibc, GPU drivers, or compositor libraries into the AppImage. Maintain an allowlist for expected system libraries and fail packaging on an unresolved dependency. Generate a machine-readable dependency manifest and checksums for every artifact.

The release CI matrix should have separate Linux, macOS, and Windows jobs that build Rust, build Zig, run core and headless UI tests, package, inspect binary imports, install the artifact, run Native SDK automation, run the media corpus, and upload logs/diagnostics on failure. Compile-only checks do not reopen Windows or macOS releases.

## Deterministic test strategy

Use four tiers with different evidence. Do not ask one screenshot mechanism to prove everything.

### Rust core tests

- migration from every supported schema fixture;
- scan, identity, marker, search, progress, stats, notes, and approved-root tests;
- C ABI create/destroy, ownership, cancellation, invalid UTF-8, stale handle, panic, and backpressure tests;
- document parser and 512x512 PDF tile tests;
- libmpv state/track/chapter extraction tests with audio output disabled.

### Headless Native SDK tests

- drive the real `.native` view and typed messages through `MarkupView`/`TestHarness`;
- use the fake external-effect executor and feed deterministic pages, warnings, errors, player metadata, and document tiles;
- rebuild after each dispatch and assert keyed IDs, layout floors, virtual ranges, focus, and accessible names;
- sweep minimum and representative desktop sizes;
- represent video with a stable poster/test pattern and deterministic diagnostics.

### Record/replay

- journal external core outcomes that affect `Model`;
- replay with no SQLite, filesystem, PDFium, or libmpv access;
- compare model fingerprints and deterministic canvas screenshot hashes;
- keep scenarios bounded so document image payloads do not exceed the existing explicit journal budget;
- never journal decoded video frames.

### Installed-package automation and visual playback

- use Native SDK accessibility snapshots and commands for navigation, input, dialogs, notes, search, and player controls;
- use media-surface diagnostics and first-frame logs for render liveness;
- capture an OS-level screenshot when proving the native child surface is visibly composited;
- verify frame counters and dimensions increase after seek/resume rather than accepting a black rectangle;
- inspect process trees and loaded libraries for sidecars and WebView runtimes;
- run VoiceOver, NVDA, and Orca smoke scripts/manual checklists for the final package.

Deterministic canvas screenshots deliberately exclude real video pixels. Codec decoding, color conversion, timing, and hardware output are not stable golden-image inputs. The native media surface is proven by diagnostics plus packaged visual evidence, while the surrounding UI remains pixel-deterministic.

## Principal risks and mitigations

| Risk | Mitigation and stop condition |
| --- | --- |
| Upstream scope expands into an SDK fork | Land generic capabilities with SDK tests before melearner depends on them. Stop if native-only host or media surface cannot be accepted upstream. |
| OpenGL is deprecated on macOS | Retain it only as the migration's proven libmpv path; isolate behind the surface ABI so a Metal path can replace it later. Do not combine that replacement with the UI cutover. |
| Zig/Rust memory or thread bugs | Opaque handles, explicit byte ownership, event release, panic barriers, one model thread, sanitizers, and ABI stress tests. |
| Event queue floods during playback or scans | Coalesce nonterminal progress/position, preserve terminal events, reject new work with typed busy status, and expose overflow diagnostics. |
| Large libraries exceed fixed SDK budgets | Rust-owned pagination, virtual lists, bounded model pages, stable IDs, and 100,000-lesson load tests. |
| Existing progress is corrupted | Reuse the current database, apply migrations transactionally, test fixture copies, back up before new migration, and preserve ADR 0008 matching exactly. |
| Linux GTK generation mismatch | SDK owns GTK4 widgets and GL context. Rust receives only the renderer callback seam, not a GTK3 widget. |
| PDF/document work becomes a browser substitute | Use a finite native document AST and PDF tiles. Unsupported fidelity is disclosed with external-open; no active HTML execution. |
| Packages work only on developer machines | Build/install on clean OS images, stage dependency closures, audit imports/rpaths, and require visual playback before release. |
| Canvas UI is inaccessible on Linux/Windows | Treat AT-SPI/UI Automation bridges and real screen-reader smoke tests as Stage 1 and release gates. |
| Native SDK API churn breaks the app | Pin a source revision and upgrade deliberately with the upstream fixture app and ABI/UI test suite. |

## Rejected alternatives

- **Keep Tauri only as an invisible host:** rejected because the shipped binary still contains the browser/WebView architecture and keeps two UI systems alive.
- **Use a separate mpv process or player window:** rejected by ADR 0010 and the one-window/no-sidecar decision.
- **Copy every decoded video frame into `gpu_surface`:** rejected because it adds CPU copies, fights bounded image/pixel paths, and bypasses libmpv's render API.
- **Attach private GTK/AppKit/Win32 children behind the SDK's back:** rejected because layout, clipping, visibility, fullscreen, teardown, accessibility, automation, and replay would not share one owner.
- **Move the whole app to Rust:** rejected because it discards the chosen Native SDK state, markup, automation, and deterministic UI model without reducing the media or packaging work.
- **Keep SQL or course identity in Zig for convenience:** rejected because two domain owners would make persisted progress and ambiguity behavior diverge.
- **Render documents in a hidden WebView:** rejected because it violates the binary boundary and turns document support into an untestable exception.

## Final go/no-go gates

The overhaul may cut over only when all of these are true:

1. Packaged canvas-only binaries have no WebView/browser dependency or runtime module on all three OSes.
2. One in-window Native SDK media surface visibly renders libmpv on all three OSes and survives resize, hide/show, fullscreen, and destroy.
3. The Rust static library opens existing databases and matches all scan, identity, search, progress, stats, and notes fixtures.
4. Linux, macOS, and Windows installed packages pass the ADR 0010 codec and failure corpus with one user-visible window and no helper process.
5. Headless tests, fake core tests, record/replay, live automation, and screen-reader checks all pass.
6. Document behavior has an accepted native implementation or an explicitly approved reduced scope; no WebView exception exists.
7. AppImage, Arch package, MSI, and macOS signed/notarized artifacts pass clean-machine dependency and launch checks.
8. The Tauri/React/Node implementation and stale packaging are deleted in the same verified cutover, leaving one architecture.

The architecture is viable if Native SDK accepts the four foundation changes. It is not viable as a melearner-only overlay on the SDK as it exists today.

## Sources

1. Native SDK, [Platform Support](https://native-sdk.dev/platform-support).
2. Native SDK, [Native UI](https://native-sdk.dev/native-ui).
3. Native SDK, [App Model](https://native-sdk.dev/app-model).
4. Native SDK, [Testing](https://native-sdk.dev/testing).
5. Native SDK, [Automation](https://native-sdk.dev/automation).
6. Native SDK, [Packaging](https://native-sdk.dev/packaging), and local `/home/efaz/Codes/native/README.md`.
7. Native SDK local platform/runtime contracts: `/home/efaz/Codes/native/src/platform/types.zig`, `/home/efaz/Codes/native/src/runtime/effects.zig`, `/home/efaz/Codes/native/src/runtime/session_record.zig`, `/home/efaz/Codes/native/src/runtime/session_replay.zig`, and `/home/efaz/Codes/native/src/runtime/automation_snapshot.zig`.
8. Native SDK desktop implementations: `/home/efaz/Codes/native/src/platform/macos/root.zig`, `/home/efaz/Codes/native/src/platform/linux/root.zig`, and `/home/efaz/Codes/native/src/platform/windows/root.zig`.
9. Native SDK effect implementation, `/home/efaz/Codes/native/src/runtime/effects.zig`.
10. Native SDK automation implementation: `/home/efaz/Codes/native/src/runtime/automation_commands.zig`, `/home/efaz/Codes/native/src/runtime/automation_snapshot.zig`, `/home/efaz/Codes/native/src/runtime/session_record.zig`, and `/home/efaz/Codes/native/src/runtime/session_replay.zig`.
11. Native SDK build and package implementation: `/home/efaz/Codes/native/build/app.zig` and `/home/efaz/Codes/native/src/tooling/package.zig`.
12. Native SDK examples: `/home/efaz/Codes/native/examples/notes` and `/home/efaz/Codes/native/examples/gpu-surface`.
13. melearner playback decision and implementation: `/home/efaz/Codes/melearn/docs/adr/0010-embedded-libmpv-native-playback.md`, `/home/efaz/Codes/melearn/src-tauri/src/native_player.rs`, and `/home/efaz/Codes/melearn/src-tauri/src/native_player/surface/`.
14. melearner identity decision and implementation: `/home/efaz/Codes/melearn/docs/adr/0008-durable-course-identity-uses-local-fingerprints.md`, `/home/efaz/Codes/melearn/lib/course-identity.ts`, `/home/efaz/Codes/melearn/lib/database.ts`, and `/home/efaz/Codes/melearn/src-tauri/src/scanner.rs`.
15. melearner cleanup decision, `/home/efaz/Codes/melearn/docs/adr/0009-remove-stale-and-redundant-artifacts.md`.
16. melearner packaging evidence: `/home/efaz/Codes/melearn/.github/workflows/release.yml` and `/home/efaz/Codes/melearn/.github/workflows/windows-msi.yml`.
17. melearner current document behavior, `/home/efaz/Codes/melearn/components/content-viewer.tsx` and `/home/efaz/Codes/melearn/types/index.ts`.
