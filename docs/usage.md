# Usage

## First Run

1. Open melearner.
2. Click **Scan root folder**.
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
| `Space` / `K` | Play or pause |
| `M` | Mute or unmute |
| `F` | Fullscreen |
| `J` / `Left` | Seek back 10 seconds |
| `L` / `Right` | Seek forward 10 seconds |

## Playback Compatibility

The app first tries to play the original local file through WebKitGTK. If the browser engine rejects a playable-looking file, melearner can prepare a cached compatible copy.

The common case is a file named `.mp4` that is actually an MPEG-TS container. In that case the app remuxes the file into a real MP4 without re-encoding, which is much faster than transcoding.

Only genuinely incompatible streams should need transcode fallback.

## Progress

Progress saves automatically to local SQLite. The app keeps the last position and completion state for each lesson.

Current progress is tied to the scanned library records. Durable progress across folder rename/move is planned in `docs/stats-and-identity-plan.md`.

## Search

Use the search control or `Ctrl K` to search across courses and lessons.
