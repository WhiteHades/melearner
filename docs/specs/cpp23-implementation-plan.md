# C++23 implementation plan

Parent specification: [#31](https://github.com/WhiteHades/melearner/issues/31)

Wayfinder map: [#25](https://github.com/WhiteHades/melearner/issues/25)

This is the dependency and traceability index for the all-C++ product contract in `fully-native-melearner.md`. GitHub stores the native blocking edges and each ticket's detailed acceptance criteria. This file keeps the same plan reviewable without network access.

## Execution rules

- Work only on a ticket whose blocking issues are closed.
- Add a failing test or fixture before new or repaired behavior.
- Keep every slice runnable and preserve the module protocols and limits in `docs/research/cpp23-qt-overhaul.md`.
- Do not add migration, compatibility, rollback, sidecar, helper-process, QML, WebView, or old-runtime fallback paths.
- Do not start final cutover until installed Linux, macOS, and Windows package gates all pass for the same source revision.
- Commit and push each verified slice before starting the next unblocked slice.

## Ticket graph

| Key | Issue | Slice | Blocked by |
|---|---:|---|---|
| T01 | #32 | Land the all-C++ contract and traceability gate | None |
| T02 | #33 | Generate deterministic parity fixtures | T01 |
| T03 | #34 | Lock toolchain, runtime, and release profiles | T01 |
| T04 | #35 | Bootstrap the C++23 Qt application shell | T02, T03 |
| T05 | #36 | Add the macOS Qt foundation | T04 |
| T06 | #37 | Add the Windows Qt foundation | T04 |
| T07 | #38 | Implement the Library database boundary | T04 |
| T08 | #39 | Build root onboarding and management | T07 |
| T09 | #40 | Scan, reconcile, and recover Courses | T07, T08 |
| T10 | #41 | Build Library resume and Course rows | T09 |
| T11 | #42 | Search and route by keyboard | T09, T10 |
| T12 | #43 | Navigate virtualized Courses and Lessons | T10, T11 |
| T13 | #44 | Embed libmpv and prove MP4 playback | T04, T05, T06 |
| T14 | #45 | Switch audio tracks and prove HEVC playback | T13 |
| T15 | #46 | Select Subtitles and chapters | T14 |
| T16 | #47 | Complete responsive Player controls | T15 |
| T17 | #48 | Persist Progress and Learning activity atomically | T07, T16 |
| T18 | #49 | Add timestamped Lesson notes | T12, T17 |
| T19 | #50 | Render text and Markdown natively | T12 |
| T20 | #51 | Convert local HTML and DOCX safely | T19 |
| T21 | #52 | Render virtualized PDFs with PDFium | T19 |
| T22 | #53 | Open unsupported documents safely | T19 |
| T23 | #54 | Build the local learning ledger | T17 |
| T24 | #55 | Add appearances and reduced motion | T04 |
| T25 | #56 | Complete responsive layouts | T10, T12, T16, T18, T20, T21, T22, T23, T24 |
| T26 | #57 | Complete accessibility and recoverable errors | T11, T12, T16, T18, T20, T21, T22, T23, T25 |
| T27 | #58 | Enforce one application instance | T04, T05, T06, T07 |
| T28 | #59 | Add deterministic UI and live automation | T26, T27 |
| T29 | #60 | Enforce concurrency, performance, and fault budgets | T09, T11, T12, T16, T17, T20, T21, T23, T28 |
| T30 | #61 | Package and qualify Linux AppImage and Arch assets | T29 |
| T31 | #62 | Package and qualify the macOS DMG | T05, T29 |
| T32 | #63 | Package and qualify the Windows MSI | T06, T29 |
| T33 | #64 | Pass the same-revision all-platform release matrix | T30, T31, T32 |
| T34 | #65 | Cut over, delete old stacks, and rerun the matrix | T33 |

## Primary story ownership

Every story from `fully-native-melearner.md` has exactly one primary owner. Supporting tickets can test the same flow but cannot claim another primary ownership row.

| Story | Ticket | Issue |
|---|---|---:|
| S1 | T04 | #35 |
| S2 | T07 | #38 |
| S3 | T09 | #40 |
| S4 | T09 | #40 |
| S5 | T08 | #39 |
| S6 | T09 | #40 |
| S7 | T09 | #40 |
| S8 | T10 | #41 |
| S9 | T10 | #41 |
| S10 | T11 | #42 |
| S11 | T11 | #42 |
| S12 | T12 | #43 |
| S13 | T12 | #43 |
| S14 | T13 | #44 |
| S15 | T13 | #44 |
| S16 | T14 | #45 |
| S17 | T14 | #45 |
| S18 | T15 | #46 |
| S19 | T15 | #46 |
| S20 | T16 | #47 |
| S21 | T16 | #47 |
| S22 | T17 | #48 |
| S23 | T17 | #48 |
| S24 | T19 | #50 |
| S25 | T20 | #51 |
| S26 | T21 | #52 |
| S27 | T22 | #53 |
| S28 | T18 | #49 |
| S29 | T23 | #54 |
| S30 | T24 | #55 |
| S31 | T25 | #56 |
| S32 | T26 | #57 |
| S33 | T24 | #55 |
| S34 | T26 | #57 |
| S35 | T27 | #58 |
| S36 | T33 | #64 |
| S37 | T34 | #65 |
| S38 | T28 | #59 |
| S39 | T29 | #60 |
| S40 | T33 | #64 |

## Verification

Run the offline graph and story-ownership check:

```bash
cmake -P scripts/test-cpp-traceability.cmake
```

The check must report 34 valid acyclic ticket rows and 40 canonical, uniquely owned stories before any implementation ticket closes.
