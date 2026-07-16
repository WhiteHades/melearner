# Course cards do not generate runtime video thumbnails

Course-library scrolling must stay responsive. Generating thumbnails from videos while cards approach the viewport can start many FFmpeg jobs during ordinary scrolling.

## Decision

Course cards should not invoke video decoding or FFmpeg at render/scroll time.

`CourseArtwork` should render the existing course visual surface and any already-persisted thumbnail data, but it should not generate new thumbnails while the user scrolls the library.

Thumbnail generation may run after library scan or persisted-library load as a queued native background job with a persistent cache key derived from source metadata. Card render must only consume already-known thumbnail URLs or the fallback visual.

In the final architecture from ADR 0011, that background work runs in process through the embedded media engine. The package must not contain or launch `ffmpeg` or `ffprobe` executables for thumbnails, metadata, or optional processing. References to FFmpeg in this ADR do not authorize a helper process in the final native line.

## Consequences

- Transitional scrolling stays DOM/CSS-bound and final-native scrolling stays Native SDK list/canvas-bound; neither path decodes media while scrolling.
- Native playback work no longer competes with thumbnail extraction for CPU.
- Dynamic thumbnails can return later only if they are generated in a queued, in-process background job with strict concurrency and cached output metadata.
