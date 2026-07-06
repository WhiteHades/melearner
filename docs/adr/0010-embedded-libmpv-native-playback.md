# Embedded libmpv native playback

## Status

Accepted.

## Context

melearner is a local-first desktop app for course files already on the user's machine. The app needs robust local-file playback, smooth seeking, native aspect-ratio handling, subtitles, track selection, speed control, and consistent behavior across Windows, macOS, and Linux.

The retired browser-media player path inherited platform codec limits, inconsistent media behavior, and poor control over native seeking/rendering behavior. Keeping that path after choosing the native engine would create two conflicting playback architectures and invite stale code.

The product target is not an external player sidecar. The end user should install one app and use one native playback engine embedded in the app process.

## Decision

The canonical playback architecture is:

```text
React/shadcn UI
  -> typed Tauri commands/events
Rust video engine
  -> embedded libmpv
native video surface / libmpv render path
  -> local files only
```

The app must not keep a browser-media playback engine. The app must not use an mpv sidecar as the target architecture. Existing Tauri libmpv plugins may be studied, but the repo should own its native video engine integration instead of depending on an unproven plugin as the foundation.

The first production native-player UI should keep complex controls in stable WebView bands around the native video surface rather than relying on fragile transparent DOM overlays directly on top of native video.

## Implementation state

The current implementation owns the native playback control path in `src-tauri/src/native_player.rs`:

- local-file and approved-root validation;
- embedded `libmpv2` lifecycle;
- play, pause, seek, volume, mute, rate, audio track, subtitle track, chapter, delay, frame-step, screenshot, and destroy commands;
- structured `track-list` and `chapter-list` reads through mpv node properties;
- React/shadcn controls in `components/video-player.tsx` with no `<video>`, `<audio>`, Shaka, or Limeplay path.

The current implementation does not yet complete native video presentation. `native_player_set_bounds` records the WebView placeholder rectangle, but no platform renderer consumes those bounds to create a visible GPU/native surface. A change is not accepted as completed native playback until libmpv frames render into a real platform surface on the packaged app.

## Requirements

- Local filesystem paths only. Reject URLs, schemes, missing files, and files outside approved library roots.
- Embedded libmpv in-process, not a bundled mpv sidecar controlled through IPC.
- Native playback is the normal path; FFmpeg remux/transcode must not be part of ordinary playback.
- Support play/pause, absolute and relative seek, volume, mute, playback rate, fullscreen, subtitles, audio tracks, chapter data, and screenshots.
- Emit typed state, track, chapter, file-loaded, end-file, and error events from Rust to the frontend.
- Send coarse position events from Rust and interpolate the visible scrubber locally.
- Keep the React/shadcn UI as the app and control layer.
- Keep FFmpeg for thumbnails, metadata, and optional processing only.

## Migration

1. Add a Rust `video_engine` module or plugin with local-file validation, libmpv lifecycle, command queue, event loop, and typed event emission.
2. Add platform renderer implementations for Windows, macOS, and Linux.
3. Add a typed frontend native-player bridge and store.
4. Replace the current lesson player internals with native-player commands and events.
5. Remove stale player files, dependencies, docs, aliases, and generated artifacts as part of the migration.
6. Keep FFmpeg out of ordinary playback.

## Verification

Native playback is not accepted until these pass on packaged builds:

- MP4 H.264/AAC.
- MKV H.264 with multiple audio tracks.
- HEVC/10-bit file.
- External SRT and VTT subtitles.
- Non-ASCII filename.
- Long path.
- Missing/deleted file.
- Corrupt media file.
- Progress save/resume after pause, quit, and reopen.
- Track and subtitle switching without restarting playback.
- No FFmpeg/remux/transcode process during normal playback.

## Consequences

- Player work should move through the native video engine instead of reintroducing a parallel browser-media engine.
- Stale compatibility bridges, package dependencies, generated player files, and docs must be removed when the native path replaces them.
- Cross-platform packaging must bundle or otherwise provide the correct libmpv dependencies for the end user.
- The repo must not add a sidecar player path as the final architecture.
