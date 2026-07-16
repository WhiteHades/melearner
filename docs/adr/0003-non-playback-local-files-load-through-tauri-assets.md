# Non-playback local files load through Tauri assets

## Status

Superseded for the final architecture by ADR 0011. Tauri asset URLs and WebView rendering remain historical constraints of the transitional production shell only; approved-root local access and the rejection of localhost/browser playback remain accepted.

Documents, images, and thumbnail assets load through Tauri asset URLs instead of a localhost media server. This keeps the app offline and direct while letting the WebView render files selected by the user.

Playable video and audio lessons are not WebView media assets anymore. The native player sends canonical local filesystem paths to embedded libmpv after approved-root validation. Do not reintroduce a localhost media server, browser `<video>`/`<audio>` playback path, Shaka, or compatibility remux/transcode path for ordinary playback.
