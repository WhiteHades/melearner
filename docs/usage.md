# Usage

## Set up your library

We recommend keeping courses inside one parent folder, with a separate folder
for each course. For example:

```text
Courses/
  Biology/
  History/
```

Select `Courses` in meLearner. It scans folders into courses, sections, and
lessons. Open a course, then choose a lesson.

## Find lessons

Choose **List** or **Cards**. Search with the field or press `/` outside a text
field. The dashboard offers a course to resume and may show a muted video
preview. It starts after three seconds, pauses when you leave the dashboard or
switch apps, and does not save progress. Video cards may show a cached still.

Choose **Lessons** to show or hide the outline. Rows show completion and media
type. Video titles appear below the player, document titles above the reader.

## Play video

Click the video to play or pause. Double click the left third to go back three
seconds, the right third to go forward three seconds, or the middle third for
fullscreen. **Capture** copies the current frame to the clipboard. The sound
icon toggles mute. The volume slider mutes at zero. Press
`Escape` to leave fullscreen.

Autoplay is off by default. Turn it on with the **Autoplay** setting to start the
next video after a five second countdown. Choose **Cancel** to stop it. The
setting is saved on this computer.

| Key | Action |
| --- | --- |
| `Space` | Play or pause |
| `Left` / `Right`, `h` / `l` | Seek back or forward three seconds when the player is focused |
| `, f` | Toggle fullscreen |
| `, m` | Mute or unmute |
| `, ,` / `, .` | Seek back one second or advance one frame |
| `K` / `J` | Previous or next lesson |

Press `?` to search the full shortcut list. Comma starts a leader command. Press
the next key within two seconds. Leader commands do not work in editable fields.

## Progress and file support

Progress saves automatically. meLearner remembers each lesson's last position
and completion. Continue learning opens the saved unfinished lesson, or the
first unfinished lesson. If you move or rename a course, scan its folder again
to reconnect its progress. Missing courses stay listed until rescanned.

Supported files include video, audio, subtitle, text, Markdown, HTML, PDF, DOCX,
CSV, and XLSX. Put subtitles beside playable lessons. CSV and XLSX show cell
values. XLSX shows stored formula values, not formatting, charts, or macros.
Other indexed files show readable text when possible or a limited hex preview.
Use **Open in default app** for other readers. PDF controls appear above the
page. Long documents have page controls. HTML scripts can run, but remote content
is blocked and local resources are limited to the open course.

The player opens original files without converted copies. Codecs depend on
bundled media libraries and graphics drivers. If playback fails, read the status
message. For decoder problems, close the app and run
`melearner --software-decoding` in a terminal.

## Stats

**Stats** shows completion, storage, media types, and twelve weeks of activity.
Progress time uses saved positions, not elapsed study time. Duration includes
only lessons with known lengths.
