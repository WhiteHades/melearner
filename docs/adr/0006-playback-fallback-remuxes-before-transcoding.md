# Playback fallback remuxes before transcoding

Some course files have an `.mp4` file name but are actually MPEG-TS containers. `ffprobe` reports these as `format_name = mpegts` even when the streams inside are browser-safe H.264 video and AAC audio.

WebKitGTK's HTML media element can reject those files as unsupported because the container does not match the expected MP4 layout. That failure is not a Rust media-player failure; the app uses WebKitGTK playback through the Tauri WebView. Rust only prepares local files for the frontend.

## Decision

When browser playback fails, melearner prepares a compatible cached copy by trying a fast remux first:

```text
ffmpeg -i input -map 0:v:0? -map 0:a:0? -dn -sn -c copy -movflags +faststart -f mp4 output.mp4
```

This rewrites the container to real MP4 without re-encoding the media streams. It should be much faster and lower CPU than full transcoding.

Only if remuxing fails should the app fall back to bounded transcoding. Transcoding must use a low thread count so one fallback cannot saturate the machine.

## Operational Rules

- Never launch multiple playback-prep jobs at once.
- Cancel stale playback-prep jobs when the player unmounts, the lesson changes, or a new fallback starts.
- Write to a `.part` file first, then atomically move it into the cache only after `ffprobe` can read it.
- Remove partial outputs on failure or cancellation.
- Drop data and subtitle streams during remux/transcode because they can make WebKit reject otherwise playable files.

## Consequences

- Mislabeled MPEG-TS files become playable without full CPU-heavy conversion.
- The user no longer sees long-running duplicate `ffmpeg` processes for separate lessons.
- Some genuinely unsupported codecs still require transcode or may fail if FFmpeg cannot decode them.
