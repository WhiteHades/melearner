# C++23 and Qt overhaul plan

Research date: 2026-08-24. Scope: the current Tauri production line, the incomplete Native SDK/Rust line, the accepted product interaction contract, Qt 6.11 documentation, CMake 4.4 documentation, SQLite, PDFium, and libmpv's render API. Published ticket graph: https://github.com/WhiteHades/melearner/issues/31 and `docs/specs/cpp23-implementation-plan.md`.

## Conclusion

Build one C++23 desktop application with Qt 6.11 Widgets. Use CMake 4.4 presets, Ninja, CTest, Qt Test, SQLite's C interface, libmpv's render API, PDFium, md4c, Lexbor, and libzip. Keep all application and test source in C++; allow CMake, JSON, YAML, shell, and PowerShell only as build and release machinery.

This stack removes the C ABI and the Rust/Zig transport without weakening its safety intent. RAII, move-only ownership, typed variants, `std::expected`, bounded queues, exception barriers, stop tokens, one SQLite writer, and deterministic shutdown become the enforceable seam.

Qt Widgets is preferred over QML because the user selected an all-C++ application and because standard Widgets provide mature platform focus, keyboard, accessibility, item-view virtualization, native dialogs, and packaging without a declarative runtime. Qt WebEngine, Qt WebView, Qt Multimedia playback, and Qt SQL are excluded.

Primary sources:

- Qt 6.11 documentation: https://doc.qt.io/qt-6/
- Qt Widgets model/view: https://doc.qt.io/qt-6/model-view-programming.html
- `QOpenGLWidget`: https://doc.qt.io/qt-6/qopenglwidget.html
- Qt accessibility: https://doc.qt.io/qt-6/accessible.html
- Qt Linux accessibility and requirements: https://doc.qt.io/qt-6/linux-requirements.html
- Qt deployment: https://doc.qt.io/qt-6/deployment.html
- CMake presets: https://cmake.org/cmake/help/latest/manual/cmake-presets.7.html
- CMake C++ standard property: https://cmake.org/cmake/help/latest/prop_tgt/CXX_STANDARD.html
- SQLite application-defined collation: https://sqlite.org/c3ref/create_collation.html
- SQLite progress handler: https://sqlite.org/c3ref/progress_handler.html
- SQLite FTS5: https://sqlite.org/fts5.html
- libmpv render contract: https://github.com/mpv-player/mpv/blob/master/include/mpv/render.h

## Final process

```text
QApplication and MainWindow
  AppController and AppState
  QAbstractItemModel projections
  native Widgets, focus, accessibility, automation
                 |
                 | typed bounded requests and results
                 v
Library thread: one sqlite3 writer
  state, paging, revisions, transactions
          |                |                 |
          v                v                 v
  scan/search pool   document worker   Player thread
  filesystem/FTS     parsers/PDFium    mpv_handle/events
                                              |
                                              | current-context render only
                                              v
                                      MpvVideoWidget::paintGL
```

The GUI does not call disk, SQLite, parsers, PDFium, or normal libmpv commands. Worker completions enter `AppController::dispatch` on the Qt event thread. Only that path changes `AppState`.

## Deep modules

### AppController

Interface:

```cpp
class AppController final {
public:
    Effects dispatch(Action action);
    const AppState& state() const noexcept;
};
```

`Action` and completion types are `std::variant` values. `AppController` owns navigation, selected IDs, visible pages, loading and error states, query generations, Player generations, overlay invokers, scroll/focus restoration intents, and stale-result rejection. It owns no database, file, parser, PDF, or mpv handle.

### Library

Interface:

```cpp
class Library final : public QObject {
    Q_OBJECT
public:
    std::expected<RequestId, SubmitError> submit(LibraryRequest request);
    CancelResult cancel(RequestId requestId);

signals:
    void event(LibraryEvent event);
};
```

`Library` owns the current schema and every domain transaction. Its interface hides SQL, revisions, queueing, worker coordination, and error translation. Tests use the same request/event seam with real temporary databases.

### LocalFiles

`LocalFiles` canonicalizes paths, rejects URLs and schemes, verifies approved roots, captures safe local directory/file handles, reads and writes marker files without following unsafe replacements, and validates external-open candidates. It returns domain values or typed errors; callers never concatenate trusted paths themselves.

### Documents

`Documents` accepts open, block-page, tile, close, and external-open validation requests. It owns the finite document model, active PDFium document, render cancellation, and tile cache. It returns immutable blocks and `QImage` tiles.

### Player

`Player` accepts load and control commands with request IDs. It owns `mpv_handle`, the mpv event client, track/chapter projections, screenshot policy, position coalescing, and render-context lifecycle. It exposes no raw mpv pointer to views.

### SingleInstance

`SingleInstance` uses `QLockFile`, `QLocalServer`, and `QLocalSocket`. It acquires ownership or forwards one validated bounded startup route. It never creates a second Library owner or Player.

### Shared protocol contract

The module seam uses strong value types instead of interchangeable strings or integers. `CourseId`, `SectionId`, `LessonId`, `NoteId`, and `DiagnosticId` are distinct opaque UTF-8 values capped at 128 bytes. `RequestId`, `Revision`, and each route, query, document, and Player `Generation` are distinct nonzero `std::uint64_t` wrappers. Paths cross module boundaries only as `ValidatedRoot`, `ValidatedFile`, or learner-facing display text from `LocalFiles`.

The request families are fixed:

| Owner | Accepted request families |
| --- | --- |
| `Library` | open current database; read/update root and settings; scan/rescan/locate Course; read Library, Course, Lesson, search, notes, stats, and activity pages; write/delete note; commit Player position/completion; shut down |
| `LocalFiles` | validate root; capture Course/file handle; discover one bounded batch; read/write marker; validate external-open candidate |
| `Documents` | open text/Markdown/HTML/DOCX/PDF; read block page; render PDF tile; find/jump/zoom state; close; validate external open through `LocalFiles` |
| `Player` | initialize render context; load/unload; play/pause; seek; volume/mute/rate; select audio/Subtitle/chapter; frame step; screenshot; fullscreen intent; close |
| `SingleInstance` | acquire owner; validate/forward startup route; receive activation; release owner |

Each accepted asynchronous request owns one completion credit. Submission fails immediately with typed `busy`, `invalid`, `stale`, `closing`, or subsystem error when no credit or precondition exists. A completion is exactly one typed success or error carrying its request ID and relevant revisions/generations. Progress snapshots can coalesce, but they do not consume or replace the terminal completion. No result exposes a raw SQLite, filesystem, PDFium, mpv, OpenGL, or Qt object handle.

`Action` contains learner intents and typed module completions only. `AppController::dispatch` emits at most 16 typed effects for one action; larger work is one batch effect. Effects can submit module work, update a model page, restore focus/scroll, show an overlay, or request a Qt window operation. Views cannot manufacture module completions. Cross-thread payloads are immutable and move-owned. Every QObject delivery into `AppController` is queued onto the GUI thread and checked there before state changes.

## State and error contract

`AppState` contains logical projections named `app`, `navigation`, `root`, `library`, `search`, `course`, `lesson`, `player`, `document`, `notes`, `stats`, `settings`, and `overlay`.

Every asynchronous projection uses explicit variants from this set as applicable:

- `loading`
- `empty`
- `ready`
- `warning`
- `recoverable_error`
- `fatal_error`

`AppError` contains:

- stable code
- scope
- operation
- optional local path
- retryable flag
- diagnostic ID
- learner-facing message
- ordered safe actions

Worker entry points catch exceptions and C-library failures. An unexpected exception produces one fatal module result, closes that module's queue, and remains destructible.

## Thread and queue rules

| Owner | Responsibilities |
| --- | --- |
| Qt GUI thread | Widgets, item models, `AppController`, focus, accessibility, OpenGL paint |
| Library thread | One writable `sqlite3*`, revisions, statements, transactions |
| Scan/search pool | Filesystem discovery, fingerprints, immutable search work |
| Document worker | Parsing, PDFium document, visible tile rendering, LRU cache |
| Player thread | `mpv_handle`, commands, properties, event polling |

Rules:

1. Submission is `O(1)` and never waits for work completion.
2. Every queue has a fixed capacity returned by diagnostics and exercised by load tests.
3. Position, seek, volume, search, scan progress, and PDF tile requests can coalesce by stable key.
4. Accepted terminal success and error results never silently drop.
5. Results carry request ID plus relevant Library, query, route, document, or Player generation.
6. Stop tokens cancel cooperative filesystem and parser work. SQLite installs a progress handler and supports `sqlite3_interrupt`.
7. Scan cancellation has one atomic commit gate. Cancellation before it rolls back; cancellation after it returns too late.
8. Shutdown rejects new requests, cancels pre-commit work, flushes Progress, destroys the mpv render context with the OpenGL context current, joins workers, closes SQLite, releases the process lock, then destroys Qt.

The first release uses these fixed limits. Changing one requires a measured replacement in ADR 0012 or a successor decision.

| Resource | Limit and overflow behavior |
| --- | --- |
| Scan/search pool | Compute once at startup as `clamp(max(reported hardware concurrency, 2) - 2, 2, 8)` and never resize |
| Accepted Library requests | 128 in flight; each reserves one terminal-completion slot or returns `busy` |
| Replaceable Library progress | 64 keyed slots; newest value replaces the prior value for that request/key |
| Scan work | 32 queued batches of at most 256 entries; producers wait cooperatively or cancel |
| Scan warning detail | 512 ordered details plus uncapped per-code counts; excess details increment `omittedWarnings` |
| Search work | 8 query generations; a ninth supersedes the oldest nonterminal query; 100 rows per page |
| Library/Course/Lesson pages | 128 Courses, 128 Sections, or 256 Lessons per page |
| Notes and activity pages | 100 notes; activity is the fixed 84-day window |
| Document work | 64 accepted requests with reserved completions; 128 normalized blocks or 1 MiB UTF-8 per page |
| PDF tile work/cache | 64 keyed pending tiles; 64 cached 512x512 RGBA tiles; visible requests replace prefetch first |
| Player commands | 128 accepted commands with reserved completions; 32 keyed replaceable property/event slots |
| Effect batch | 16 effects from one `dispatch`; bulk domain changes use one paged/batch effect |
| Single-instance route | One UTF-8 route of at most 16 KiB; excess or malformed input is rejected |
| Cross-thread payload | 4 MiB maximum excluding one validated image/tile allocation governed by its own limit |
| Artwork cache | 128 MiB decoded-pixel LRU; one source is at most 16 MiB encoded and 2048x2048 decoded |

Terminal completion publication cannot overflow because acceptance reserves its slot. If the GUI stops draining, workers remain cancellable and shutdown consumes or cancels every reservation before joining. No producer allocates an overflow list.

## SQLite design

Use SQLite's direct C interface behind RAII wrappers, not `QSqlDatabase`. This gives exact control over the schema, one connection owner, `BEGIN IMMEDIATE`, custom natural collation, FTS5, progress interruption, and error codes.

Open sequence:

1. Acquire the C++ process/data lock.
2. Open only the distinct C++ database path.
3. For a new file, create the exact current schema in one transaction.
4. For an existing C++ file, compare the exact expected schema and refuse an incompatible file without changing it.
5. Enable foreign keys, WAL, `synchronous=NORMAL`, a 10-second busy timeout, and `MELEARNER_NATURAL`.
6. Run integrity and foreign-key checks.
7. Allocate a nonzero process-session Library revision. Settings keeps its independent persisted revision.

Visible mutations check expected revision, begin `IMMEDIATE`, validate, apply all rows, cross the commit gate, commit, publish one new revision, and invalidate revision-bound caches.

### Current C++ storage contract

The desktop/application ID remains `io.github.whitehades.melearner`, but storage is deliberately isolated by setting the Qt organization to `WhiteHades`, the internal application name to `melearner-cpp-v1`, and the display name to `melearner`. The only database path is `QStandardPaths::AppLocalDataLocation/library-v1.sqlite3` under that identity. The app does not enumerate sibling application-data locations.

The embedded schema has `PRAGMA user_version = 1`, schema ID `melearner-cpp-library-v1`, and a compile-time SHA-256 of its normalized DDL. `schema_info` stores the same ID and hash in one row. Startup accepts only an empty new file or an exact match for all three values and normalized `sqlite_schema`; any mismatch closes the file unchanged with `incompatible_current_schema`. There are no migration statements.

All IDs are the bounded opaque values from the shared protocol. Timestamp columns ending in `_at` or `_since` are signed 64-bit UTC Unix milliseconds, file modification values are signed 64-bit nanoseconds supplied by the platform, `duration` and `watched_time` are nonnegative integer seconds, `last_position` and note `timestamp` are nonnegative real seconds, byte counts are nonnegative signed 64-bit values, and activity dates are ISO `YYYY-MM-DD` UTC strings. The schema contains exactly these durable relations:

| Relation | Required columns and constraints |
| --- | --- |
| `schema_info` | singleton key fixed to 1, unique `schema_id`, unique `ddl_sha256` |
| `library_root` | singleton key fixed to 1, unique `path`, `updated_at` |
| `settings` | singleton key fixed to 1, `appearance` in light/dark/cozy, `library_presentation` in comfortable/compact, nonnegative independent `revision`, `updated_at` |
| `courses` | primary `id`, unique `identity_id`, `name`, unique `path`, `fingerprint`, nullable `thumbnail_source_path`, nullable `last_accessed`, `last_scanned_at`, nullable `missing_since` |
| `sections` | primary `id`, foreign `course_id`, `name`, nonnegative `order_index`, unique `(course_id, id)` and `(course_id, order_index)` |
| `lessons` | primary `id`, foreign `(course_id, section_id)`, `name`, unique `path`, `relative_path`, type in video/audio/document/quiz, nonnegative `duration`, `watched_time`, `last_position`, `file_size`, and `order_index`, boolean `completed`, `modified_ns`, `updated_at`, unique `(course_id, id)` and `(course_id, relative_path)` |
| `notes` | primary `id`, foreign `lesson_id`, nonnegative `timestamp`, nonempty `text`, `created_at`, `updated_at` |
| `lesson_subtitles` | primary `id`, foreign `lesson_id`, unique `path`, `relative_path`, `language`, `label`, nonnegative `order_index`, unique `(lesson_id, path)` |
| `lesson_activity` | primary `id`, foreign `(course_id, lesson_id)`, `activity_date`, nonnegative `watched_seconds`, boolean `completed`, `created_at` |
| `library_search` | contentless FTS5 rows for Course, Section, and Lesson `name`, with unindexed kind/object/course/section IDs, `unicode61 remove_diacritics 2`, and prefix indexes 2, 3, and 4 |

Foreign keys cascade only from Course to its Sections/Lessons and from Lesson to notes, Subtitles, and activity. Missing Courses are retained, so normal scanning never deletes a Course merely because its path is absent. Required B-tree indexes cover Course identity, path, fingerprint, missing/title order, Section `(course_id, order_index)`, Lesson `(course_id, section_id, order_index)`, Lesson path and `(course_id, relative_path)`, notes `(lesson_id, created_at)`, Subtitles `(lesson_id, order_index)`, and activity by date, Course/date, and Lesson/date. FTS rows update inside the same transaction as their source names.

Player events carry integer milliseconds at the module seam. A Progress write stores `last_position = position_ms / 1000.0`, replaces `watched_time` with `floor(position_ms / 1000)`, and stores media `duration` as the nonnegative integer floor of duration seconds. The activity delta is `max(new watched_time - previous watched_time, 0)` in `lesson_activity.watched_seconds`. This is the only milliseconds-to-seconds conversion and matches `docs/stats-and-identity-plan.md`.

The DDL is authored once at `cpp-app/schema/library-v1.sql` and embedded into the executable. `cpp-app/schema/library-v1.lock.json` independently records the accepted schema ID and DDL hash, while `fixtures/schema/library-v1.sqlite3` is an immutable prior-package compatibility fixture covered by the fixture manifest. Tests compare the resource to the lock, inspect tables/columns/constraints/indexes/FTS, open and mutate the immutable fixture, and reopen it after package replacement. The v1 lock and fixture are never regenerated after T07 closes. A schema change requires a new ID, data path, fixture, and decision rather than editing v1.

## Efficient algorithms and data structures

| Operation | Required algorithm | Time | Resident space |
| --- | --- | ---: | ---: |
| Root scan | One visit per entry plus deterministic natural sort | `O(F log F)` | `O(F)` bounded snapshot |
| Identity reconciliation | Hash indexes by Course path/marker/fingerprint and Lesson path/relative path/metadata | Expected `O(C + S + L + T)` | `O(C + S + L + T)` |
| Main Library | Indexed SQL aggregate and bounded page | `O(log C + P)` plus page aggregates | `O(P)` |
| Course outline | Stable ordered page index | `O(log Lc + P)` | `O(P)` plus bounded key map |
| Search build/query | FTS5 index and bounded top-result heap | Index `O(N)`, query index-dependent | `O(H)` |
| Stats | Set-based aggregate transaction | `O(C + L + Awindow)` | Fixed result rows |
| Text normalization | Streaming decode and bounded blocks | `O(bytes)` | Reviewed source/block bounds |
| HTML/DOCX | Iterative finite-tree conversion | `O(nodes + bytes)` | Node/depth/image bounds |
| PDF tile | Clip render for requested pixels | `O(tile pixels)` | 64 fixed RGBA tiles |
| Player render | libmpv renders directly into current FBO | `O(frame pixels)` in renderer | No frame queue copy |

Here `F` is visited filesystem entries, `C` Courses, `S` Sections, `L` Lessons, `T` Subtitle tracks, `P` one bounded page, `N` indexed Course/Section/Lesson names, `H` the bounded search heap, `Lc` Lessons in the selected Course, and `Awindow` activity rows in the fixed 84-day window.

Rejected structures:

- all-pairs Course or Lesson matching
- a second full Library graph in GUI memory
- `setIndexWidget` or one `QWidget` per virtual row
- unbounded `std::vector`, event queue, warning list, or image cache fed by external data
- GUI-side linear search over every Library row
- recursive parsing without a depth counter
- N+1 stats, Library, or Course queries
- decoded video frames in Qt signals or state

## Scan and identity

The scan phases remain `discovering`, `classifying`, `reconciling`, `committing`, and `writing_markers`.

Discovery returns healthy Course snapshots plus structured skipped-Course warnings. It captures root identity and safe handles. The Library thread remains available for revision-bound reads while discovery runs. Other mutations queue behind the active scan.

Course identity order remains exact path, one unambiguous marker identity, one unambiguous fingerprint, then new Course. Lesson identity remains exact path, relative path, one unambiguous metadata match, then new Lesson. Hash indexes make ambiguity detection linear instead of comparing each new item with every retained item.

At commit, all previously available Courses are candidates for missing state except successful or explicitly skipped present paths. Root, rows, missing state, and revision commit atomically. Marker writes use captured handles after commit; all are attempted and their failures become counted warnings.

## Qt UI architecture

- `QMainWindow` hosts one stable root widget and overlay host.
- `QStackedLayout` changes top-level routes without creating duplicate state owners.
- `QListView` plus paged `QAbstractListModel` drives Library, search, notes, and activity.
- `QTreeView` plus paged `QAbstractItemModel` drives Section and Lesson navigation.
- `canFetchMore` and `fetchMore` request bounded pages.
- Stable domain IDs live in model roles and restore focus, selection, and scroll after updates.
- A 100 ms `QTimer` debounces search. Query IDs reject late pages.
- Responsive layout changes at 560, 768, and 1280 logical pixels. It rearranges stable panes without reparenting an active OpenGL Player.
- Standard Widgets supply platform semantics. Custom Player, document, Course artwork, Progress, and activity surfaces implement `QAccessibleInterface` behavior.

## Documents

- Text uses streaming UTF-8 validation and bounded selectable blocks.
- Markdown uses md4c callbacks to build the finite document model.
- HTML uses Lexbor for standards-tolerant parsing, then converts only an allowlist. Script, style, event attributes, schemes, and remote resources never enter the model.
- DOCX uses libzip and `QXmlStreamReader`. Archive paths, compressed and uncompressed bytes, XML nodes, nesting, relationships, and images are bounded.
- PDFium renders visible 512x512 clips. The cache key is document ID, page, quantized scale, x tile, and y tile. A fixed LRU holds at most 64 RGBA tiles and prioritizes visible pages.
- External open uses `LocalFiles` validation followed by `QDesktopServices::openUrl(QUrl::fromLocalFile(...))` on the GUI thread.

Document acceptance limits are 64 MiB source bytes for text/Markdown/HTML, 128 MiB compressed and 512 MiB total expanded bytes for DOCX, 4,096 archive entries or relationships, 500,000 parsed nodes, depth 128, 100,000 normalized blocks, 128 MiB normalized UTF-8, 256 images, 32 MiB per encoded image, and 256 MiB total decoded image pixels. PDF accepts at most a 2 GiB local file and 5,000 pages; only the active document metadata plus the fixed tile cache remain resident. A breached limit returns a typed oversized/complexity error and external-open action where safe. Partial parser output never commits as a successful document.

## Player

`MpvVideoWidget` derives from `QOpenGLWidget`.

- `initializeGL` creates `mpv_render_context` while the widget context is current.
- The mpv update callback only sets an atomic dirty flag and queues `update()` to the GUI thread.
- `paintGL` reads `defaultFramebufferObject()` every time, calls render update, and renders. It acquires no Library or Player command lock.
- Resize, DPR change, hide/show, occlusion, fullscreen, context destruction, detach, and shutdown update diagnostics and preserve one top-level window.
- The Player thread owns normal mpv calls and events. Commands use request IDs and asynchronous mpv commands.
- Local media and Subtitle paths pass `LocalFiles` before load.
- Position events coalesce. File-loaded, end-file, command failures, and renderer failures are terminal.
- Package acceptance disables hardware decoding and requires changing visible HEVC Main 10 frames.

## Build and dependency lock

- Ticket T03 freezes the lock before the Qt shell or any third-party integration can close. Later tickets consume it and cannot resolve a newer dependency implicitly.
- CMake presets define developer, test, sanitizer, and release configurations.
- Release CI installs an exact Qt 6.11 patch build from the official Qt distribution and records its archive hashes.
- A pinned vcpkg baseline supplies SQLite, md4c, Lexbor, and libzip where supported.
- libmpv/FFmpeg and PDFium use separately pinned source or binary records because package layout and codec flags are release-critical.
- `packaging/runtime-lock.json` records every source URL, version/commit, checksum, toolchain, configure flag, enabled decoder, staged filename, linkage, SPDX license, notice/source offer, artifact checksum, and signing/legal requirement ID. T03 does not require a production secret or completed publication approval.
- Qt is dynamically linked. The release ships Qt LGPL notices and relink/replacement information required by the approved legal review.
- `windeployqt` and `macdeployqt` are staging helpers only; the project-owned audit rejects undeclared plugins, Qt WebEngine/View, host paths, and unresolved imports.

The curated release set is exactly Linux x86_64 AppImage plus Arch `pkg.tar.zst`, one signed/notarized universal macOS DMG, and one signed Windows x64 MSI. The macOS bundle contains both arm64 and x86_64 slices. An artifact is not qualified until install, launch, all product flows, package replacement with the unchanged C++ schema, uninstall without deleting the C++ data directory, reinstall, retained-data reopen, and final uninstall evidence pass on its immutable profile. Those lifecycle checks are current-data behavior, not backup, restore, migration, or rollback support.

`packaging/reference-profiles-v1.json` is created with the fixture lock and is immutable once accepted. Each profile records schema version, platform and architecture, OS image digest, runner provider and hardware shape, CPU and memory, GPU/display/presenter, filesystem, locale/timezone, screen reader, exact compiler/CMake/Ninja/Qt tools, runtime-lock hash, network policy, hardware-decoding policy, and measurement commands. A mismatched profile can emit diagnostics but cannot pass, fail, or rebaseline release budgets.

T03 creates `packaging/evidence-schema-v1.json` and `packaging/release-evidence-schema-v1.json` as JSON Schema 2020-12 documents with `additionalProperties: false`. One installed artifact writes `out/evidence/<sourceRevision>/<artifactId>.json`. Its required fields are `schemaVersion` fixed to 1; 40-hex `sourceRevision`; `artifact` with fixed ID/platform/architecture/format/SHA-256/signature status and public certificate or notarization reference; profile, fixture, runtime-lock, and SBOM IDs/SHA-256 values; boolean network-disabled and hardware-decoding-disabled conditions; S1-S40 status/evidence references; test ID/status/log-hash rows; metric ID/value/unit/budget/status rows; accessibility tool/status/log-hash rows; codec and visible-frame results; loaded-library/process/window/package-exclusion audits; install/replace/uninstall/reinstall/retained-data lifecycle results; attachment hashes; legal requirement approval references; and creation time.

The aggregate `out/evidence/<sourceRevision>/release.json` requires exactly four valid artifact manifests with IDs `appimage-linux-x86_64`, `arch-linux-x86_64`, `dmg-macos-universal`, and `msi-windows-x86_64`. All four must have the same source/profile-set/fixture/runtime-lock/SBOM schema hashes and passing required statuses. T33 rejects any missing, duplicate, extra, stale, or mixed-revision artifact. T34 changes the revision by deleting old stacks, then regenerates all four manifests and the aggregate before it can close. Production signing identities and legal approvals are required by T30-T33 evidence, not by the pre-implementation T03 lock.

## Testing seams

1. `Library::submit` with real temporary SQLite and filesystem fixtures.
2. `AppController::dispatch` with deterministic typed results and no Qt platform dependency.
3. Qt Test against real item models and widgets, including offscreen and installed automation.
4. `Documents` through real malformed, boundary, oversized, and PDF tile fixtures.
5. `Player` through a fake mpv adapter for races and live libmpv for media behavior.
6. Installed packages for accessibility, process, window, import, signature, lifecycle, performance, and visible media evidence.

## Numeric budgets

| Interaction | Maximum |
| --- | ---: |
| Visible focusable window | 1,000 ms |
| First usable Library page | 2,000 ms |
| Page replacement and keyed focus | 200 ms |
| Search after the fixed debounce | 200 ms |
| Course shell | 250 ms |
| Lesson shell | 100 ms |
| Scan input probe; progress paint | 100 ms; 250 ms |
| Text/Markdown/HTML/DOCX page | 200 ms |
| First visible PDF tile | 300 ms |
| Player acceptance; state confirmation | 50 ms; 150 ms |
| H.264 first frame | 2,000 ms |
| HEVC Main 10 software first frame | 3,000 ms |
| Seek acceptance; changed frame | 50 ms; 750 ms |
| Resize including active video | 150 ms |
| Shutdown after flush | 2,000 ms |

The fixed release fixture contains 1,000 Courses, 100,000 Lessons, 84 activity days, paged notes, a 500-page PDF, H.264/AAC media, multi-audio MKV media, HEVC Main 10 media, SRT/VTT Subtitles, malformed and oversized document cases, missing Courses, identity ambiguities, and non-ASCII/long paths. Its manifest hashes every generated-input recipe and checked-in corpus file. One measurement run starts after a clean profile boot and four runs restart the installed app; every run must meet every applicable latency budget.

Private resident/working-set budgets are 256 MiB for a steady idle Library, 384 MiB while browsing the fully indexed 100,000-Lesson fixture, 512 MiB while rendering the bounded document/PDF fixture, and 768 MiB during HEVC Main 10 software playback. Shared system libraries and filesystem cache are reported separately by the profile command and cannot be subtracted differently between runs. Queue, page, warning, document, artwork, and tile limits are asserted directly in addition to process memory.

Hard watchdogs are 30 seconds for startup/Library, 300 seconds for the fixed scan, 15 seconds for media load/seek, and 10 seconds for all other measured phases. A budget miss is a failure; a hard timeout captures stacks, process tree, open handles, last action/result IDs, queue diagnostics, and Player surface state before termination.

## Release sequence

1. Contract and traceability.
2. Complete deterministic fixtures and their manifests.
3. Lock the exact toolchain, runtime, license inventory, artifact formats, and immutable reference profiles.
4. Prove the Qt foundation on Linux, macOS, and Windows.
5. Add the bounded async seam and fresh exact SQLite schema.
6. Deliver root, scan, identity, Library, search, Course, and Lesson slices.
7. Deliver the embedded Player, media corpus, Progress, and Learning activity.
8. Deliver notes, text, Markdown, HTML, DOCX, PDF, external open, and stats.
9. Complete appearance, responsive layout, single instance, errors, and accessibility.
10. Enforce deterministic automation, performance, memory, watchdog, and fault gates.
11. Build and qualify AppImage, Arch, DMG, and MSI artifacts with the locked runtime.
12. Aggregate same-revision Linux, macOS, and Windows evidence.
13. Delete every old stack and rerun the complete installed matrix on the changed revision.

The repository specification owns canonical stories, interaction behavior, and release acceptance. ADR 0012 owns architecture decisions. This document owns exact module protocols, schema/storage contract, limits, algorithms, dependency/profile/evidence schemas, and numeric budgets. The GitHub graph mirrors executable ticket detail and native blocking relationships. Where a summary is repeated, these ownership rules decide the source.
