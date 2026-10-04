# Native library performance check

`library_performance_test` exercises a generated 10k/30k HTML library across
10/30 courses: startup and scan, first-course opening, five search/open cycles,
30 seconds reader-open and reader-closed, then cached-database reopen. The RSS
sampler records the test and descendant processes every 500 ms, including
QtWebEngine children.

Build the target once, then run `scripts/measure-cpp-library-performance.sh
--build-dir build/cpp-dev`. Set `MELEARNER_PERF_SIZES=10000` for only the small
fixture. It uses private Xvfb, D-Bus and a null PulseAudio sink (not the desktop
or physical audio devices). Logs, fixture and CSV artifacts are in
`.tmp/012/performance`; fixtures are removed when each test exits.

## Issue #60 measurements

Measured 2026-10-04: Linux 7.2.5, Ryzen 7 4800H (8C/16T), 32 GiB, Qt 6.11.2,
GCC 16.2.1, Debug. HTML files are 256 bytes; automatic update checks are off.
The initial renderer warned GBM was unsupported and fell back to Vulkan.

Version 0.1.2 observations (single run; times ms, RSS KiB):

| Lessons | UI ready | Scan/list | Search/open median (5) | Cached list | Peak RSS | Reader open late | Closed after 30 s |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 10,000 | 379 | 917 | 383 | 196 | 790,008 | 789,940 | 672,664 |
| 30,000 | 640 | 2,170 | 384 | 356 | 819,280 | 818,268 | 696,904 |

Controlled 10k first-course comparison: three alternating trials per source,
one course click, no duplicate lesson click. DOM readiness is the first HTML
document's expected `document.body.innerText`. Baseline is exact commit
`703cbbf`; application build is commit `af9bfb1` (merged head `c24dfe2`).
Values are medians (ms unless RSS). The outline timestamp is the first
non-empty rowsInserted or modelReset signal.

| Measure | `703cbbf` | Final 0.1.2 |
| --- | ---: | ---: |
| UI ready / scan-list ready | 337 / 844 | 329 / 845 |
| First outline rows signal | 68 | 59 |
| Course click to first HTML DOM ready | 1,231 | 1,219 |
| Peak process-tree RSS (KiB) | 778,036 | 736,172 |

Each controlled QtTest run passed (3/3 checks). Outline and first-DOM medians
are essentially unchanged. QTRY saw rows 652 ms / 687 ms after the clicks, but
the earlier signal timestamps show this was event-processing delay during cold
WebEngine initialization, not late row availability. A separate warm-profile
measurement was 83 ms; it does not remove the roughly 1.2 s first cold reader
path. Full search timings are workload observations only: the early baseline
used Ctrl+K, while 0.1.2 clicks the visible Search button.

The harness writes logs and per-process `rss.csv` under
`.tmp/012/performance/results/`. RSS sums
per-process resident memory and can double-count shared pages; allocator
retention can keep RSS high, and 500 ms polling can miss short-lived children.
Reader-closed samples still showed the app and three WebEngine processes; this
does not prove all renderer memory was released or establish zero leaks. Treat
repeated increases above 25% in a timing phase or 15% in late-run RSS as
workload-specific review triggers, not universal limits, and confirm them with
another run before attribution.
