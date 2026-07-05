# Course cards do not generate runtime video thumbnails

Course-library scrolling must stay responsive. Generating thumbnails from videos while cards approach the viewport can start many FFmpeg jobs during ordinary scrolling.

## Decision

Course cards should not invoke video decoding or FFmpeg at render/scroll time.

`CourseArtwork` should render the existing course visual surface and any already-persisted thumbnail data, but it should not generate new thumbnails while the user scrolls the library.

## Consequences

- Scrolling the course grid/list stays DOM/CSS-bound instead of media-decoding-bound.
- Playback fallback no longer competes with thumbnail extraction for CPU.
- Dynamic thumbnails can return later only if they are generated in a queued background job with strict concurrency, cancellation, and persisted cache metadata.
