# Native SDK overhaul for melearner

Research date: 2026-07-10. Decision update: 2026-07-16. Scope: the Native SDK documentation and source under `/home/efaz/Codes/projects/native`, the current melearner UI and Rust/Tauri implementation, ADRs 0008-0011, and release workflows. This is an architecture and cutover decision; no application or Native SDK source changes were applied.

## Conclusion

Proceed with the Native SDK UI and retained Rust core architecture, but do not begin by rewriting screens. The final product should be one Native SDK executable with all UI authored in `.native` markup and Zig, linked to an in-process Rust `staticlib` through a versioned C ABI. Rust remains the sole owner of the current SQLite schema, scanning, course identity, search, progress, notes, document decoding, and embedded libmpv. There is no Tauri, React, Node runtime, browser engine, WebView, or helper process in the shipped app. Bundled dynamic libraries such as libmpv and a PDF renderer are in-process dependencies, not sidecars.

ADR 0011 approves this architecture, but cutover remains gated because the current Native SDK cannot yet host the product as specified. Three upstream capabilities are release blockers:

1. Completion of the existing `webview_layer = "exclude"` contract on macOS so it omits WebKit as Linux and Windows already omit their WebView layers.
2. A cross-platform external media-surface widget whose lifecycle, layout, clipping, visibility, and rendering are owned by the Native SDK.
3. OS accessibility bridges for canvas widgets on Linux and Windows, equivalent in purpose to the existing macOS bridge.

The first engineering milestone is therefore an upstream foundation spike, not a melearner screen. It must render a libmpv test pattern inside one Native SDK window on Linux, macOS, and Windows, with no WebView dependency in the binary. If that milestone fails, stop the overhaul. Do not work around it with a second window, pixel copies through `gpu_surface`, a private overlay unknown to the SDK, or a player sidecar.

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
  SQLite connection, current schema, transactions
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
- The first native release creates one fresh current database at a distinct path and never inspects or imports a pre-native or obsolete-schema database. An unchanged current native database reopens across native package replacement; a future schema replacement uses a new fresh path rather than migration, backup, restore, rollback, or compatibility code. Current `.melearner-course.json` marker IDs remain Course identity inputs.

ADR 0011 is authoritative for the Native SDK/Zig executable, Rust static core, transport boundary, and staged cutover. ADR 0010 remains authoritative for embedded libmpv, one compositor-visible app window, local-file validation, and playback controls except for its superseded React/Tauri/WebView transport. ADR 0008 remains authoritative for conservative course and lesson identity. ADR 0009 requires the old frontend, Tauri shell, superseded packaging, and generated leftovers to be deleted in the cutover change once the replacement is verified.[13][14][15]

## Current Native SDK assessment

The Native SDK is a strong fit for the application shell. Its `.native` markup, typed `Model`/`Msg`/`update` loop, compiled release markup, virtual lists, headless layout tests, live automation snapshots, deterministic screenshots, and session record/replay directly address melearner's navigation and testing needs.[2][3][4][5] The `examples/notes` application also proves a multi-pane, keyboard-oriented, persistent desktop UI shape rather than only a demo counter.[12]

It is not yet a complete fit for media, strict binary composition, document viewing, or cross-platform accessibility.

| Area | Current evidence | Decision |
| --- | --- | --- |
| Native desktop UI | Native SDK renders `.native` views into real OS windows on all three desktop platforms. macOS has the deepest GPU path; Linux and Windows use the software pixel presenter for canvas UI.[1][7][8] | Use for the whole application shell. Test performance against large Library fixtures before cutover. |
| State model | `UiApp` supplies typed messages, one update path, effects, fake execution, and replay hooks.[3][9] | Keep UI state in Zig and domain state in Rust. Do not mirror the whole database in the model. |
| Large collections | The SDK exposes virtualized list behavior and fixed runtime budgets. Canvas image upload is limited to 1 MiB per image and packet transport is bounded.[2][7] | Page Library/search data from Rust. Keep only visible pages and selected detail in the model. Tile document images to fit declared SDK limits. |
| Embedded media | `gpu_surface` accepts SDK draw packets or CPU RGBA pixels. It is not an app-owned libmpv render target. `adoptViewSurface` is macOS system-host only; Linux and Windows explicitly report no surface-adoption capability.[7][8] | Add an upstream `external_surface`/`media_surface` widget. Do not copy decoded video frames through the canvas image path. |
| Async core integration | At the pinned revision, Native SDK implements bounded application-defined external effects with live/fake adapters and session record/replay, and melearner integrates that capability for the Rust core. Results drain on the UI thread and can be faked or journaled.[9] | Keep the existing integration and its live/fake/record/replay verification. Do not replace it with a CLI or timer polling. |
| Automation | The live server exposes windows, views, semantic widgets, bounds, state, input commands, and deterministic canvas screenshots. Session replay verifies model fingerprints and canvas screenshot hashes.[4][5][10] | Reuse it. Extend snapshots with media-surface diagnostics. Use a deterministic placeholder for video in reference screenshots. |
| OS accessibility | Semantic widget snapshots exist cross-platform, but `update_widget_accessibility_fn` is wired only by the macOS platform source. Linux and Windows expose labels for native controls but do not bridge the canvas widget tree to AT-SPI/UI Automation.[7][8] | Linux and Windows canvas accessibility are release blockers, not post-launch polish. |
| Documents | Native SDK provides text/Markdown primitives and image decoding, but no PDF, DOCX, or general document surface was found.[2][7] | Build a melearner native document adapter. Never hide a WebView behind the document screen. |
| Native-only binary | The capability-driven manifest already supports `webview_layer = "exclude"`, and melearner declares it. Linux and Windows then compile out their WebKitGTK and WebView2 layers. macOS remains the gap because its host still unconditionally compiles and links WebKit.[11] | Extend the existing exclusion contract to macOS, retain explicit rejection of WebView declarations and creation, and verify all packaged binaries by import audit. Runtime non-use is insufficient. |
| Packaging | macOS packaging creates an `.app`. Linux and Windows packaging currently creates structured artifact directories, not an AppImage/Flatpak/MSI installer. Desktop platform builds require a matching host OS.[6][11] | Keep packaging application-owned initially and build in a native OS CI matrix. Upstream installer support is useful but not a cutover blocker. |

The Native SDK is pre-1.0 and its APIs still move.[6] Pin the exact source revision used by melearner. Do not build the application against an unpinned branch.

## Required upstream work and implemented integration

### 1. Complete web-layer exclusion on macOS

Native SDK already has the capability-driven manifest contract `webview_layer = "exclude"`, which melearner uses. Linux and Windows honor it by compiling out their WebView host layers. Extend that same contract to macOS rather than introducing another web-engine mode:

- macOS compiles an AppKit host without `WKWebView` code and does not link the WebKit framework;
- the existing Linux GTK4 exclusion continues to omit WebKitGTK and its build/runtime requirement;
- the existing Windows Win32 exclusion continues to omit WebView2 code and imports;
- attempts to declare or create a WebView while the layer is excluded remain compile-time or explicit runtime errors;
- CEF installation, bridge code, navigation policy, web commands, and web assets remain absent; and
- `native package` records `webview_layer = "exclude"` and stages no web assets.

This remains a capability-driven Native SDK contract rather than a melearner-specific fork. The acceptance test is not merely "no WebView created." `otool -L`, `readelf`/`ldd`, and PE import inspection must show no WebKit, WebKitGTK, WebView2, or CEF dependency in a canvas-only packaged app on every target platform.

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

Metal or Vulkan can replace OpenGL later, but that is not part of this cutover. A speculative renderer rewrite would combine two high-risk changes and discard the current cross-platform evidence.

### Implemented: bounded external asynchronous effects

At the pinned Native SDK revision, `UiApp` already implements bounded application-defined external effects and melearner already integrates the adapter for Rust core operations. The implemented adapter provides four modes:

| Mode | Implemented behavior |
| --- | --- |
| Live | Submit/cancel Rust requests, drain ordered completions after a core wake, and map them to typed `Msg` values on the UI thread. |
| Fake | Record requests without Rust, allow tests to inject outcomes, and expose overflow/rejection state. |
| Record | Write bounded external-effect results into the existing session journal before the consuming wake event, preserving current ordering. |
| Replay | Park matching requests and feed journaled outcomes without touching SQLite, files, PDFium, or libmpv. |

The implemented external result record contains adapter ID, request key, outcome, schema version, and bounded bytes. Over-budget results fail recording loudly under the existing recorder contract. Bulk media frames are never effect payloads. Library/search responses are paged, PDF tiles use the image registry, and video is represented by deterministic metadata plus a placeholder during replay. This capability remains a required verification surface even though it is not an unresolved upstream blocker.

### 3. Linux and Windows canvas accessibility

Implement the existing `WidgetAccessibilitySnapshot` service on Linux and Windows:

- Linux maps the semantic tree, focus, actions, values, selection, and text state to GTK4 accessibility/AT-SPI.
- Windows exposes the same information and actions through UI Automation.
- focus and actions flow back as the existing `widget_accessibility_action` event shape.
- virtualized rows report stable list position/count without materializing the full Library.
- the external media surface has a role/name and does not swallow keyboard focus from surrounding controls.

Automation semantics are necessary but do not prove screen-reader integration. Release verification must execute the installed-package VoiceOver, NVDA, and Orca gates defined by `fixtures/parity/accessibility-manifest-v1.json`.

### Packaging improvements that can follow in parallel

Native SDK should eventually emit a Windows installer and Linux portable package rather than artifact directories. melearner should not wait for that work: its existing release workflows already contain AppImage, Arch package, MSI signing, and Windows libmpv dependency-staging knowledge.[16] Port that logic to the new binary first, then upstream general pieces that do not encode melearner-specific runtime libraries.

## Rust core design

Use the dedicated crate at `crates/melearner-core` with `crate-type = ["staticlib", "rlib"]`. The `rlib` supports Rust tests, the existing Tauri Cargo crate dependency, and internal tools; the Native SDK executable links the `staticlib`. The Tauri crate already directly reuses `melearner_core::scanner`; keep that existing Rust reuse frozen and regression-tested until cutover. Core expansion is additive, but it must not redirect the transitional shell through the C ABI, native database ownership, Native SDK effects, or Native SDK UI. Do not decouple the existing scanner reuse before cutover and do not add another adapter. The final crate has no `tauri`, `tauri-plugin-*`, Tokio UI runtime, JavaScript bridge, or WebView types.

The core should contain these modules:

| Module | Responsibility |
| --- | --- |
| `db` | Create or reopen only the distinct current database path, install the exact current schema on first creation, enforce foreign keys/WAL/busy timeout, and serialize writes. |
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
ml_status_t ml_core_create(const ml_config_v2 *, ml_core_t **out_core);
void ml_core_destroy(ml_core_t *core);
ml_status_t ml_core_set_waker(ml_core_t *, ml_wake_fn, void *context);
ml_status_t ml_core_poll_event(ml_core_t *, ml_event_v1 *out_event);
void ml_core_release_event(ml_core_t *, ml_event_v1 *event);
ml_status_t ml_core_cancel(ml_core_t *, uint64_t request_id);
```

`ml_config_v2`, `ml_event_v1`, and every future public struct begin with `struct_size` and `abi_version`. Public integers use fixed widths. Booleans are `uint8_t`. Strings and arbitrary bytes use `{const uint8_t *ptr; size_t len;}` and are UTF-8 where declared. Rust enums never cross the boundary directly.

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

High-level examples are `ml_library_scan_v1`, `ml_search_query_v1`, `ml_progress_put_v1`, `ml_notes_save_v1`, and `ml_player_load_v1`. Names and structs are version-suffixed when their shape changes. Additive event kinds and JSON fields are allowed within the current ABI generation; incompatible ownership or struct changes require a new ABI generation and replacement structs.

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

## Cutover plan and gates

Stages 0-5 add and verify the replacement while the Tauri app keeps its existing direct Rust reuse of the core scanner, implementation path, and release path. Its full regression suite must pass at every stage gate through Stage 6. No intermediate stage may redirect the transitional shell through the C ABI, native database ownership, Native SDK effects, or Native SDK UI. The existing scanner reuse does not need to be decoupled before ownership switches in the verified Stage 6 cutover.

### Stage 0: freeze behavior and fixtures

Record the existing contract before changing ownership:

1. Check in `fixtures/parity/fixture-manifest-v1.json`. It identifies every database, scanner, document, media, and load fixture by stable ID; records its path, SHA-256, schema/format version, generator and seed when generated, license/source when redistributed, expected row/count/result facts, and the tests that consume it. The existing `database-current.sql`, `oracle-v1.json`, and `scanner-v1.json` become entries rather than implicit inputs.
2. Check in `fixtures/parity/screenshot-manifest-v1.json`. Deterministic non-video goldens run as fixed cases at 560x400, 1024x768, and 1440x900 logical pixels with scale 1.0, locale `en-US`, timezone UTC, checked-in fonts, fixed light/dark/cozy themes, and reduced motion both off and on. They use the reference software canvas presenter and a deterministic media placeholder. The semantic hash must match exactly; the pixel hash must match exactly when the same reference profile is used. OS-level media proof is separate and must meet a declared non-black/changing-frame pixel-variance threshold recorded in the manifest. Each case also records scenario ID, fixture ID, route/state, contrast, text scale, reference-profile ID, Native SDK revision, and expected hashes. A hash or threshold change requires a reviewed manifest change; tests do not rewrite goldens.
3. Check in `packaging/reference-profiles-v1.json`. Each profile pins an immutable OS image ID and digest plus exact hardware allocations, including architecture, CPU model/count, RAM, GPU, and display. It also records the filesystem, graphics presenter, toolchain/runtime versions, and allowed repetitions. Only exact declared profiles enforce pixel hashes and responsiveness budgets; other runners provide non-gating diagnostics.
4. Check in `fixtures/parity/accessibility-manifest-v1.json` as the versioned installed screen-reader gate. For each target it pins the immutable OS image ID and exact VoiceOver, NVDA, or Orca version; scripts root, scan, search, Course, Lesson, document, Player, notes, and settings flows; and records expected semantic role, name, state, value, action, list position/count, focus order/restoration, and action round-trips. Required evidence includes the installed package hash, OS and screen-reader versions, command transcript, semantic snapshots, focus/action logs, and captured screen-reader output. Every expectation has an objective pass/fail result, and any missing or mismatched required result fails the gate. Manual notes may supplement this evidence but cannot replace the scripted checks.
5. Keep ADR 0010's playback behavior and this report's required codec corpus as the installed-package oracle.
6. Capture automation-level flows for initial Library load, scan, search, Course navigation, Progress resume, notes, document open, and error recovery, and record package dependency manifests for each OS so browser-library removal can be proven later.

Gate: the current implementation passes its tests, every manifest validates its schema and file hashes, and the fixtures are reproducible without personal Library data.

### Stage 1: prove Native SDK prerequisites upstream

Extend `webview_layer = "exclude"` to the macOS host, and implement the media-surface widget and Linux/Windows accessibility in `/home/efaz/Codes/projects/native`. Build a small fixture app, not melearner, that:

- has one canvas window and no WebView capability;
- embeds a test libmpv render context through the proposed surface seam;
- resizes, hides, restores, and fullscreens the surface;
- exposes diagnostics in automation snapshots;
- verifies the implemented melearner external-effect integration in live and fake modes, including bounded record/replay of an external completion;
- is keyboard and screen-reader operable;
- packages and starts on all three operating systems without a browser import.

Gate: all three upstream blockers pass on packaged builds, and the implemented external-effect fake/record/replay verification remains green. Failure stops the overhaul.

### Stage 2: add and stabilize `melearner_core`

Add current-schema database operations, identity, marker writing, search, Progress/activity, notes, document adapters, and Player control behind a safe Rust interface and the versioned C ABI for the final-native target. Keep the existing direct Tauri reuse of `melearner_core::scanner` unchanged and keep current marker behavior. Do not route the transitional shell through the C ABI, native database owner, Native SDK effects, or Native SDK UI. Add Rust tests for the exact current schema, fresh creation, restart opening, every identity ambiguity, C ABI ownership, panic containment, queue pressure, and a Zig smoke client.

Gate: the core creates and reopens its distinct current database, never touches a sibling pre-native or obsolete-schema database, produces the expected scan/identity/search results for current fixtures, and passes sanitizer/ABI tests. No UI is needed for this gate.

### Stage 3: build a read-only Native SDK vertical slice

Create `native-app/app.zon`, `native-app/src/main.zig`, and `.native` views for startup, Library, course, lesson, and search. Connect them through the external-effect adapter to paged core results. Include loading, empty, missing-course, warning, and error states from the start.

Gate: a 100,000-lesson fixture remains responsive through virtual lists; stale request results are rejected by revision/request ID; headless tests and live automation cover keyboard navigation on all three platforms.

### Stage 4: mutations, stats, notes, and documents

Add scan selection, Progress/activity, stats, Course access, notes, automatic marker writes, text/Markdown/HTML/DOCX normalization, PDF tiling, and external-open fallback. Keep database writes in Rust transactions; attempt marker filesystem writes only after the scan transaction commits, and return their failures as warnings on the committed revision.

Gate: the current-schema fixture remains valid, identity behavior exactly matches ADR 0008, stats match current outputs, and direct document formats pass installed-package smoke tests without network or WebView use.

### Stage 5: embedded playback

Port the current native-player state machine and event extraction into `melearner_core` without redirecting the frozen Tauri Player, connect the new core to the upstream media surface, and recreate controls in `.native` UI. Preserve tracks, chapters, subtitles, screenshots, coarse position updates, resume, visibility, and fail-closed surface attachment.

Gate: every packaged visual test in ADR 0010 and this report passes on Linux, macOS, and Windows, including software-decoded HEVC 10-bit. Diagnostics report a live render thread, nonzero frames and dimensions, and no render error. OS window inspection reports one melearner window. Process inspection reports no mpv, ffmpeg, or ffprobe child process.

### Stage 6: package, cut over, and delete

Replace release workflows with the Native SDK/Cargo build matrix and package the in-process runtime libraries. The native package uses only its distinct current database path and leaves pre-native and obsolete-schema database files untouched. After all installed-package gates pass, remove Next.js, React, shadcn, Tauri, frontend assets, JavaScript bridges, Node scripts that no longer apply, old release jobs, and obsolete generated artifacts in the same cutover change.

Gate: source and binary scans find no Tauri, WebView, WebKit, WebView2, CEF, React, or browser-runtime path; all user-visible flows pass; the old shell is absent rather than dormant behind a flag.

There is no shipped dual-stack fallback. Until Stage 6, the existing Tauri release remains the production line with its frozen direct Rust scanner reuse while the native app is an unreleased build target. The native app has no pre-native database rollback, restore, import, or compatibility path; unchanged current native data follows the package lifecycle below.

## Codec and playback acceptance matrix

The discovery extension list is broader than the guaranteed codec corpus. A recognized `.avi`, `.webm`, `.flac`, or other extension means "show this learning item," not "every codec variant in this container is supported." The release guarantee is the tested corpus below plus any later fixture added deliberately.

| Case | Required result |
| --- | --- |
| MP4 H.264/AAC | First frame, audio, seek, pause/resume, progress persistence |
| MKV H.264 with multiple audio tracks | Track list and live switching without reload |
| HEVC 10-bit | Visible playback with hardware decoding disabled, using the packaged software decoder; failure blocks that platform release |
| External SRT and VTT | Registration, selection, rendering, and Unicode text |
| Chapters | Ordered list, current chapter, and selection |
| Playback controls | Absolute/relative seek, volume, mute, rate, frame step, screenshot, fullscreen |
| Paths | Unicode, spaces, long path, renamed/moved course, deleted file |
| Failures | Missing, outside-root, corrupt, unsupported, and detached-surface cases return typed errors |
| Architecture | One app window, in-process libmpv, render-api surface, no normal-playback FFmpeg process |

HEVC 10-bit is a required pass case. Production may use safe hardware acceleration only when libmpv can fall back to software, but package verification disables hardware decoding and must still produce changing visible frames on every supported clean-machine image. An unsupported-codec message, audio-only playback, a black surface, or a hardware-only pass fails the release gate.

## Packaging and release design

Build on the target operating system because the current Native SDK build graph rejects a selected desktop platform that differs from the target host.[11]

### Codec runtime and inventory policy

Every artifact carries one private, software-capable runtime assembled from the same release lock. It includes libmpv, the required shared FFmpeg libraries and their software H.264, AAC, and HEVC Main 10 decoders, the audio/render dependency closure, and the pinned PDF renderer. Hardware decoding is optional acceleration. The app must remain correct with hardware decoding disabled. No artifact contains or invokes `mpv`, `ffmpeg`, or `ffprobe` executables.

`packaging/runtime-lock.json` is a checked-in source artifact owned and populated by issue #23. For every staged native file it records component name, exact version or commit, source URL, source checksum, build toolchain, configure flags, enabled codec/demuxer set, staged filename, linkage, SPDX license, notice/source location, artifact checksum, and the production signing identity reference where signing applies. Packaging generates an SPDX SBOM, `THIRD_PARTY_NOTICES`, the complete corresponding-source/build-script offer required by each redistributed license, and a load/import manifest. CI fails on an undeclared file, checksum drift, unresolved import, unexpected system lookup, or license without recorded approval.

Issue #7 defines package policy; it is not an executable release recipe. Package implementation or publication remains blocked until issue #23 replaces every placeholder or floating value in `packaging/runtime-lock.json` and the signing configuration with exact versions/commits, source and artifact checksums, build toolchains and flags, enabled decoders, staged dependency filenames, approved license records, and production signing identity references.

The release codec build uses shared LGPL-mode libmpv and FFmpeg: mpv is built with GPL features disabled, FFmpeg is built with `--disable-gpl --disable-nonfree`, and no GPL-only, AGPL, nonfree, or proprietary codec component is admitted without a new legal/architecture decision. The built-in FFmpeg HEVC decoder is required; an x265 encoder is not. The release record enumerates any relink, replacement, reverse-engineering exception, source, and notice obligations identified for the included licenses. Before publication, the maintainer records in `packaging/runtime-lock.json` and the release evidence the license and codec-patent review scope, declared distribution channels and regions, and approval reference for that release. A missing approval record blocks publication to the declared scope. This document states an engineering release requirement, is not legal advice, and does not assert that review or approval has already occurred.

| Platform | Artifact | Private runtime location and baseline exclusions |
| --- | --- | --- |
| Linux | AppImage | Bundle the locked closure under the AppDir and resolve it with `$ORIGIN` RUNPATH. Do not bundle glibc, the kernel ABI, GPU drivers, or compositor libraries; all allowed host libraries are versioned in a baseline allowlist. |
| Linux | Arch package | Install the same locked closure under `/usr/lib/melearner` with private RUNPATH. Do not load the host `mpv` package or an unpinned system libmpv/FFmpeg at runtime. |
| macOS | Signed `.app` in DMG | Put libmpv, FFmpeg dylibs, PDF runtime, and non-system dependencies in `Contents/Frameworks`; rewrite install names to app-relative paths and reject package-manager prefixes. |
| Windows | MSI | Put `melearner.exe`, libmpv, FFmpeg/runtime DLLs, PDF runtime, and the complete non-system DLL closure in the application directory; reject PATH or MSYS2 runtime resolution. |

### Signing and publication rules

- Every release publishes SHA-256 checksums, `packaging/runtime-lock.json`, the SPDX SBOM, and notices beside the curated artifacts. The checksum manifest, AppImage, and Arch package have detached signatures from the project release key; Arch package metadata consumes its package signature.
- The macOS app signs every nested executable and dylib from the inside out with a Developer ID Application identity, hardened runtime, and secure timestamp. The final app and DMG are signed, submitted to Apple notarization, accepted, and stapled. `codesign --verify --deep --strict`, `spctl --assess`, staple validation, and clean-machine Gatekeeper launch must pass.
- The Windows release signs the melearner executable and MSI with the production Authenticode identity and RFC 3161 timestamp, then verifies both signatures on a clean image. Third-party DLL signatures are preserved when present and every DLL is covered by the signed checksum manifest. Self-signed or test certificates are CI-only and can never publish a supported artifact.
- CI never promotes an unsigned rebuild, replaces bytes under an existing version, or signs an artifact whose runtime lock, SBOM, license review, dependency audit, and clean-package evidence do not match.

### Native package lifecycle

These rules apply only between releases built under ADR 0011. A pre-native Tauri installation is not an upgrade source: the first native release uses its distinct native data path, never reads or removes pre-native databases/artifacts, and may require the user to remove the transitional application package separately. There is no downgrade support.

| Artifact | Native-to-native update or upgrade | Remove behavior |
| --- | --- | --- |
| AppImage | No in-app updater. Download and verify the new signed AppImage, quit the running app, atomically replace the file at the user-selected stable path, then relaunch. Never patch a mounted/running image. | Delete the AppImage and any user-created desktop integration. |
| Arch package | Upgrade only through `pacman -U` or the configured AUR helper after package-signature verification. The package manager atomically replaces owned program/runtime files and runs no data migration hook. | `pacman -R` removes only package-owned files. |
| DMG | Quit melearner, mount the notarized DMG, and replace the signed app bundle in `/Applications`; there is no privileged helper or background updater. Gatekeeper verifies the replacement before launch. | Move the app bundle to Trash; there is no package receipt or data-removal script. |
| MSI | Use one stable native-line `UpgradeCode`, a new `ProductCode` per release, and a versioned major upgrade. The MSI refuses while melearner is running, verifies the signed package, and replaces application files in place without custom data actions. | Apps & Features invokes MSI removal for installer-owned files only. |

Native-to-native upgrades preserve the native database, settings, logs, and cached document/thumbnail data because they live outside package-owned locations. Uninstall/remove also retains them, and never deletes Course files or `.melearner-course.json` markers. Data deletion must be a separate explicit user action or manual directory removal. In-place native upgrades are permitted only while the exact current native schema is unchanged; a future schema replacement requires a new fresh data path and decision, not migration, backup, restore, rollback, or compatibility code.

The release CI matrix has separate Linux, macOS, and Windows jobs that build Rust, build Zig, run core and headless UI tests, package, inspect binary imports, install the artifact, run Native SDK automation, run the media corpus, exercise the package lifecycle, and upload the evidence bundle. Compile-only checks do not reopen a platform release.

## Deterministic responsiveness gates

Budgets are product acceptance limits, not hang timeouts. They run against the installed artifact with networking disabled only on immutable profiles declared in `packaging/reference-profiles-v1.json`. A job whose image digest, hardware shape, display/presenter, filesystem, or pinned toolchain differs from its declared profile records diagnostics but cannot pass, fail, or rebaseline the release budget. Profile changes require a reviewed new manifest version rather than editing an accepted profile in place.

The fixed release fixture is identified and hashed by `fixtures/parity/fixture-manifest-v1.json`; it contains 1,000 Courses, 100,000 Lessons, 84 activity days, paged notes, a 500-page PDF, H.264/AAC media, and HEVC Main 10 media on local storage. Screenshot cases and hashes come only from `fixtures/parity/screenshot-manifest-v1.json`. One run starts immediately after reference-image boot and four runs restart the app; every measured run must meet every budget.

| Interaction | Measurement | Maximum budget |
| --- | --- | --- |
| Startup | Process creation to visible window that accepts and paints a focus command | 1,000 ms |
| First usable Library | Process creation to first committed Course page with Search, Settings, and navigation accepting input; no startup rescan | 2,000 ms |
| Library/Lesson paging | Accepted page request to painted replacement rows and restored keyed focus | 200 ms |
| Search | Query submission after the fixed 100 ms debounce to painted first result or empty state | 200 ms |
| Course navigation | Course activation to usable outline and selected resume Lesson shell | 250 ms |
| Lesson navigation | Lesson activation to selected Lesson shell and stable controls; media/document content may remain loading | 100 ms |
| Scan UI responsiveness | Input-probe round trip while the 100,000-Lesson scan runs; accepted progress event to painted status | 100 ms probe; 250 ms progress |
| Document blocks/pages | Request to painted text/Markdown/HTML/DOCX block page | 200 ms |
| PDF tiles | Scroll/zoom request to first painted visible tile, with remaining tiles progressive | 300 ms |
| Player command acceptance | UI dispatch to request ID and pressed/busy feedback; non-decoding command to confirmed state | 50 ms acceptance; 150 ms state |
| Player first frame | Accepted local load to first changing visible frame with audio initialization nonblocking | 2,000 ms H.264; 3,000 ms HEVC 10-bit software decode |
| Player seek | Seek dispatch to request ID; accepted seek to first changing post-seek frame | 50 ms acceptance; 750 ms frame |
| Resize | Platform resize event to committed layout and a correctly sized presented frame, including active video | 150 ms |
| Shutdown | Close acceptance to process exit after Progress/database flush | 2,000 ms |

The hard watchdog limits are deliberately looser: 30 seconds for startup/first Library, 300 seconds for the deterministic scan, 15 seconds for media load or seek, and 10 seconds for page/search/navigation/document/ordinary command/resize/shutdown phases. A budget miss fails acceptance and records timing without being labeled a hang. A hard timeout labels a hang, captures app/core/media thread stacks, process tree, open handles, last Model/event sequence, surface diagnostics, and logs, then terminates the process group.

## Fault-injection requirements

The fake external-effect adapter and installed-package diagnostic hooks must deterministically inject:

- delayed, cancelled, reordered, duplicated, stale-revision, malformed UTF-8, oversized, queue-full, and panic outcomes at the C ABI/event seam;
- unavailable roots, permission denial, path escape, files disappearing mid-scan, ambiguous marker/fingerprint matches, marker-write failure, read-only/full storage, SQLite busy, and corrupt current database;
- malformed HTML/DOCX/PDF, PDF runtime absence, per-page decode failure, and tile-cache pressure;
- missing runtime libraries, corrupt/unsupported media, detached/zero-size surfaces, delayed first frame, decoder failure, seek failure, renderer loss, and screenshot-write failure; and
- a second launch during scan/playback plus close, resize, display-scale, suspend/resume, and accessibility-action races.

Each case must prove that unrelated UI probes remain within budget, the expected typed state and safe actions appear, stale results do not enter the Model, no partial transaction becomes visible, retry succeeds after the fault is removed when the error is retryable, and shutdown leaves no child or helper process.

## Clean-package evidence

Every release candidate installs or stages each artifact on a newly provisioned target image with no developer checkout, compiler, mpv/FFmpeg executable, user-installed codec pack, PDF runtime, or preexisting melearner data. The image is taken offline after artifact acquisition. The run covers fresh install, cold launch, root selection, scan, paging, search, Course/Lesson navigation, notes, stats, documents, Player corpus, single-instance activation, accessibility, shutdown, native-to-native upgrade from the previous accepted native release when one exists, remove, and reinstall with retained native data.

The uploaded evidence bundle contains:

- artifact hash/signature/notarization result, fixture/screenshot/accessibility/reference-profile manifest versions, immutable OS image ID, CPU/RAM/GPU/display details, build provenance, `packaging/runtime-lock.json`, SBOM, notices, and dependency/import/load audits;
- automation transcript, semantic snapshots, deterministic non-video screenshots, scripted accessibility-manifest results, pinned screen-reader versions and captured output, focus/action round-trip logs, per-interaction timing JSON, watchdog report, logs, process trees, and media-surface diagnostics;
- OS-level H.264 and HEVC screenshots plus frame counters, dimensions, pixel-variance evidence, and a post-seek frame change with hardware decoding disabled;
- install/upgrade/remove command logs and before/after inventories of package-owned files, processes, services, startup entries, user-data directories, and Course marker files; and
- fault-injection results with the injected code, expected typed state, recovery action, retry result, and transaction/revision assertions.

Manual screen-reader notes may supplement the accessibility evidence but cannot replace any scripted manifest check or required artifact.

The first native release records native-to-native upgrade as not applicable and supplies the fresh install/remove/reinstall evidence. Missing evidence, a developer-machine-only pass, a host codec dependency, or an unsigned/unnotarized artifact fails the package gate.

## Deterministic test strategy

Use four tiers with different evidence. Do not ask one screenshot mechanism to prove everything.

### Rust core tests

- fresh creation and restart opening of the exact current schema;
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
- compare model fingerprints and the reviewed hashes in `fixtures/parity/screenshot-manifest-v1.json`;
- keep scenarios bounded so document image payloads do not exceed the existing explicit journal budget;
- never journal decoded video frames.

### Installed-package automation and visual playback

- use Native SDK accessibility snapshots and commands for navigation, input, dialogs, notes, search, and player controls;
- use media-surface diagnostics and first-frame logs for render liveness;
- capture an OS-level screenshot when proving the native child surface is visibly composited;
- verify frame counters and dimensions increase after seek/resume rather than accepting a black rectangle;
- inspect process trees and loaded libraries for sidecars and WebView runtimes;
- run the versioned VoiceOver, NVDA, and Orca installed-package scripts from `fixtures/parity/accessibility-manifest-v1.json`; manual notes may supplement but cannot replace them.

Deterministic canvas screenshots deliberately exclude real video pixels. Codec decoding, color conversion, timing, and hardware output are not stable golden-image inputs. The native media surface is proven by diagnostics plus packaged visual evidence, while the surrounding UI remains pixel-deterministic.

## Principal risks and mitigations

| Risk | Mitigation and stop condition |
| --- | --- |
| Upstream scope expands into an SDK fork | Land generic capabilities with SDK tests before melearner depends on them. Stop if macOS web-layer exclusion or the media surface cannot be accepted upstream. |
| OpenGL is deprecated on macOS | Retain it only as the cutover's proven libmpv path; isolate behind the surface ABI so a Metal path can replace it later. Do not combine that replacement with the UI cutover. |
| Zig/Rust memory or thread bugs | Opaque handles, explicit byte ownership, event release, panic barriers, one model thread, sanitizers, and ABI stress tests. |
| Event queue floods during playback or scans | Coalesce nonterminal progress/position, preserve terminal events, reject new work with typed busy status, and expose overflow diagnostics. |
| Large libraries exceed fixed SDK budgets | Rust-owned pagination, virtual lists, bounded model pages, stable IDs, and 100,000-lesson load tests. |
| Native Progress is corrupted | Keep one Rust writer, use strict constraints and transactions, test the current seed plus restart behavior, and preserve ADR 0008 matching exactly. |
| Linux GTK generation mismatch | SDK owns GTK4 widgets and GL context. Rust receives only the renderer callback seam, not a GTK3 widget. |
| PDF/document work becomes a browser substitute | Use a finite native document AST and PDF tiles. Unsupported fidelity is disclosed with external-open; no active HTML execution. |
| Packages work only on developer machines | Build/install on clean OS images, stage dependency closures, audit imports/rpaths, and require visual playback before release. |
| Canvas UI is inaccessible on Linux/Windows | Treat AT-SPI/UI Automation bridges and the versioned installed screen-reader manifest checks as Stage 1 and release gates. |
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
3. The Rust static library creates and reopens only its distinct current database and matches all scan, identity, search, Progress, stats, and notes fixtures.
4. Linux, macOS, and Windows installed packages pass the codec and failure corpus, including visible software-decoded HEVC 10-bit, with one user-visible window and no helper process.
5. Headless tests, fake core tests, record/replay, live automation, and screen-reader checks all pass.
6. Document behavior has an accepted native implementation or an explicitly approved reduced scope; no WebView exception exists.
7. AppImage, Arch package, MSI, and macOS signed/notarized artifacts pass runtime inventory, license, lifecycle, numeric budget, fault-injection, and clean-package evidence gates.
8. The Tauri/React/Node implementation and stale packaging are deleted in the same verified cutover, leaving one architecture.

The architecture is viable if Native SDK accepts the three foundation changes. It is not viable as a melearner-only overlay on the SDK as it exists today.

## Sources

1. Native SDK, [Platform Support](https://native-sdk.dev/platform-support).
2. Native SDK, [Native UI](https://native-sdk.dev/native-ui).
3. Native SDK, [App Model](https://native-sdk.dev/app-model).
4. Native SDK, [Testing](https://native-sdk.dev/testing).
5. Native SDK, [Automation](https://native-sdk.dev/automation).
6. Native SDK, [Packaging](https://native-sdk.dev/packaging), and local `/home/efaz/Codes/projects/native/README.md`.
7. Native SDK local platform/runtime contracts: `/home/efaz/Codes/projects/native/src/platform/types.zig`, `/home/efaz/Codes/projects/native/src/runtime/effects.zig`, `/home/efaz/Codes/projects/native/src/runtime/session_record.zig`, `/home/efaz/Codes/projects/native/src/runtime/session_replay.zig`, and `/home/efaz/Codes/projects/native/src/runtime/automation_snapshot.zig`.
8. Native SDK desktop implementations: `/home/efaz/Codes/projects/native/src/platform/macos/root.zig`, `/home/efaz/Codes/projects/native/src/platform/linux/root.zig`, and `/home/efaz/Codes/projects/native/src/platform/windows/root.zig`.
9. Native SDK effect implementation, `/home/efaz/Codes/projects/native/src/runtime/effects.zig`.
10. Native SDK automation implementation: `/home/efaz/Codes/projects/native/src/runtime/automation_commands.zig`, `/home/efaz/Codes/projects/native/src/runtime/automation_snapshot.zig`, `/home/efaz/Codes/projects/native/src/runtime/session_record.zig`, and `/home/efaz/Codes/projects/native/src/runtime/session_replay.zig`.
11. Native SDK build and package implementation: `/home/efaz/Codes/projects/native/build/app.zig` and `/home/efaz/Codes/projects/native/src/tooling/package.zig`.
12. Native SDK examples: `/home/efaz/Codes/projects/native/examples/notes` and `/home/efaz/Codes/projects/native/examples/gpu-surface`.
13. melearner playback decision and implementation: `/home/efaz/Codes/projects/melearn/docs/adr/0010-embedded-libmpv-native-playback.md`, `/home/efaz/Codes/projects/melearn/src-tauri/src/native_player.rs`, and `/home/efaz/Codes/projects/melearn/src-tauri/src/native_player/surface/`.
14. melearner identity decision and implementation: `/home/efaz/Codes/projects/melearn/docs/adr/0008-durable-course-identity-uses-local-fingerprints.md`, `/home/efaz/Codes/projects/melearn/lib/course-identity.ts`, `/home/efaz/Codes/projects/melearn/lib/database.ts`, and `/home/efaz/Codes/projects/melearn/src-tauri/src/scanner.rs`.
15. melearner cleanup decision, `/home/efaz/Codes/projects/melearn/docs/adr/0009-remove-stale-and-redundant-artifacts.md`.
16. melearner packaging evidence: `/home/efaz/Codes/projects/melearn/.github/workflows/release.yml` and `/home/efaz/Codes/projects/melearn/.github/workflows/windows-msi.yml`.
17. melearner current document behavior, `/home/efaz/Codes/projects/melearn/components/content-viewer.tsx` and `/home/efaz/Codes/projects/melearn/types/index.ts`.
