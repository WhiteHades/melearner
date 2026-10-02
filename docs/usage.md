# Usage

## First Run

1. Open melearner.
2. Click **Choose root folder**.
3. Choose the folder that contains your course folders.
4. Open a course and select a lesson.

melearner groups files into courses, sections, and lessons based on the folder structure it scans.

## Supported Learning Items

- Video files
- Audio files
- Documents including text, Markdown, HTML, PDF, DOCX, CSV, and XLSX
- Subtitle tracks next to playable lessons

Choose **List** or **Cards** above the library to change its layout. The choice is
saved on your computer. Search is available in the library or with `Ctrl+K`.

The course outline stays to the left of the lesson. Previous lesson, Next lesson,
and completion controls are beside the lesson heading. On a narrow window, use
**Lessons** to switch between the outline and the reader.

Markdown and HTML are read inside the app without running scripts or loading
remote content. CSV and XLSX display cell values as tables. XLSX formatting,
charts, and macros are not rendered, and formulas show their stored values.
Other indexed files show readable text when possible, or a bounded hexadecimal
preview for unrecognized binary content. **Open in default app** is available
for document formats that need a dedicated application.

PDF zoom and page controls sit directly above the PDF. Reading-page controls
appear above long text documents only when the document spans multiple pages.

## Playback Shortcuts

| Key | Action |
| --- | --- |
| `Space` | Play or pause when the player is focused |
| `h` / `Left` | Seek back 10 seconds in the player; collapse the selected outline section elsewhere |
| `l` / `Right` | Seek forward 10 seconds in the player; expand the selected outline section elsewhere |
| `f` | Toggle fullscreen |
| `m` | Mute or unmute |
| `,` / `.` | Seek back one second / advance one frame |
| `[` / `]` | Previous / next lesson |

## Keyboard-first navigation

The app keeps Qt's normal Tab, Shift+Tab, arrows, Enter, Space, text selection,
IME, and editing behavior. Vim-style motions apply when a library, outline, or
read-only lesson view has focus; editable fields keep their native bindings.

| Key | Action |
| --- | --- |
| `j` / `k` | Move down / up in the focused list or scroll a read-only lesson |
| `gg` / `G` | Jump to the first / last focused item |
| `Ctrl+D` / `Ctrl+U` | Scroll down / up one page |
| `c` | Mark the current lesson complete or incomplete |
| `o` | Toggle the course outline on narrow windows |
| `Ctrl+K` / `/` | Search the library |
| `:` / `Ctrl+Space` | Open the searchable command palette |
| `?` / `F1` | Open the searchable shortcut list |
| `Escape` | Close fullscreen or return to the Library |

The shortcut popup is intentionally searchable and grouped by context. It is
the source of truth for the commands currently implemented; melearner does not
embed a Vim or Neovim editor runtime.

## Playback Compatibility

Playable lessons use the in-app native player. It opens the original local file
without creating a converted playback copy. Hardware decoding is selected
automatically when the codec and driver support it; software decoding handles
the fallback. Supported formats depend on the bundled media libraries and the
available graphics drivers.

When Qt reports `llvmpipe` or `softpipe` as the OpenGL renderer, melearner asks
libmpv to render each frame into a CPU image, then Qt presents that image in
the video widget. This fallback still requires a functioning Qt OpenGL
context. It does not promise support for every software OpenGL driver.

If a file cannot be opened, the status message reports the error. Select another
lesson from the outline to continue using the library.

For a blank video or a decoder problem, close melearner and launch it from a
terminal with `melearner --software-decoding`. This skips hardware decoder probes
and selects software decoding. Decode mode is separate from presentation: on
`llvmpipe` or `softpipe`, the CPU-image presentation path is still used; other
renderers use the OpenGL presentation path. The fallback does not bypass a
broken Qt OpenGL context or repair a damaged media file.

## Progress

Progress saves automatically to local SQLite. The app keeps the last position and completion state for each lesson.

Continue learning prefers an unfinished lesson with saved progress. When no
unfinished lesson has progress, it opens the first unfinished lesson in course
order. A course with every lesson completed opens from the beginning.

Course identity uses local database IDs and content fingerprints, not just absolute paths. If you rename or move a course folder and scan it again, melearner tries to reconnect the course and its lessons to the existing progress.

If a course folder is missing during a refresh, melearner keeps its progress, subtitles, and lesson records in SQLite. The course stays visible with a missing-folder label and cannot be opened until the folder is scanned again.

If two existing courses look identical, melearner does not guess. It leaves progress on the existing records and shows a scan warning instead of assigning progress to the wrong course.

## Stats and Activity

The library dashboard shows local stats for courses, completion, watched progress, storage, media type mix, top courses, and recent activity. The activity heatmap is built from local `lesson_activity` rows written when lesson progress changes.

## Identity Markers

melearner writes `.melearner-course.json` into available course folders automatically after scans and after loading existing libraries. Future scans use that marker ID before fingerprint matching.

Marker files are local metadata only. They are not telemetry, sync, or remote identifiers. Existing marker files with a different identity are not overwritten, duplicate marker IDs are ignored with warnings, and missing courses are skipped.

## Search

Use the search control or `Ctrl K` to search across courses and lessons.
