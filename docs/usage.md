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
- Documents including text, markdown, HTML, PDF, and DOCX
- Subtitle tracks next to playable lessons

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

Playable lessons use the in-app native player for local files. The app should play the original file directly instead of preparing a converted playback copy first.

If a file cannot be opened, melearner should report the local-file problem clearly and keep the rest of the library usable.

## Progress

Progress saves automatically to local SQLite. The app keeps the last position and completion state for each lesson.

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
