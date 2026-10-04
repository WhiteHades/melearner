# Usage

Choose a folder that contains your course folders. meLearner groups scanned files
into courses, sections, and lessons. Open a course, then choose a lesson.

## Supported files

meLearner reads video, audio, subtitle, text, Markdown, HTML, PDF, DOCX, CSV, and
XLSX files. Subtitle files must sit beside playable lessons. CSV and XLSX show
cell values. XLSX formulas show stored values; formatting, charts, and macros do
not appear. Other indexed files show readable text when possible, or a limited
hex preview. Use **Open in default app** for formats that need another reader.

PDF controls appear above the page. Long text documents have page controls when
they span multiple pages. HTML scripts can run, but remote content is blocked
and local resources are limited to the open course.

## Library and lessons

Use **List** or **Cards** to change the library layout. Search the library with
the search field or `/` outside a text field. Cards may show a cached still from
a video. The dashboard shows a course to resume and a muted video preview when
available. The preview starts after three seconds, pauses when you leave the
dashboard or switch apps, and does not save progress.

The course outline sits beside the lesson. Use **Lessons** to show or hide it.
Lesson rows show completion and media type. Video titles appear below the player;
document titles appear above the reader.

## Playback

| Key | Action |
| --- | --- |
| `Space` | Play or pause |
| `Left` / `Right`, `h` / `l` | Seek back or forward 3 seconds when the player is focused |
| `, f` | Toggle fullscreen |
| `, m` | Mute or unmute |
| `, ,` / `, .` | Seek back one second or advance one frame |
| `K` / `J` | Previous or next lesson |

Click the video to play or pause. Double click its left or right third to seek
3 seconds, or its middle third to toggle fullscreen. Autoplay is off by default.
When enabled, the next video starts after a five second countdown. Choose
**Cancel** to stop it. The autoplay setting is saved on this computer.

The sound icon toggles mute. The volume slider mutes at zero and restores sound
when raised. **Capture** copies the current frame to the clipboard. Fullscreen
shows only the video and controls; press `Escape` to leave it.

The shortcut list is searchable with `?`. The comma is the leader key. Press the
next key within two seconds. Leader commands do not work in editable fields.
Other useful commands include `/` to search, `:` for the command palette, `, o`
to toggle the outline, `, b` to return to the library, and `, c` to change lesson
completion. Lists and read only lessons also support `j` and `k` to move, `gg`
and `G` to jump to the start or end, and `Ctrl+D` and `Ctrl+U` to scroll a page.
Editable fields keep their normal keyboard behavior.

## Progress and playback support

Progress saves automatically in the local library database. meLearner remembers
each lesson's last position and completion. Continue learning opens the saved
unfinished lesson when available, otherwise the first unfinished lesson. If you
move or rename a course, scan its folder again to reconnect it with saved
progress. A missing course stays listed until its folder is scanned again.

The player opens original local files without making converted copies. Supported
codecs depend on the bundled media libraries and graphics drivers. If playback
fails, the status message reports the error. For decoder problems, close the app
and run `melearner --software-decoding` from a terminal. This does not repair a
damaged file or a broken Qt OpenGL context.

## Stats

**Stats** shows completion, storage, media types and twelve weeks of activity.
Progress time comes from saved lesson positions, rather than elapsed study time.
Total duration includes only lessons with known durations.
