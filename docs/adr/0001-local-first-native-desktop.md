# Local-first native desktop app

## Status

Accepted for the local-first desktop decision. ADR 0012 supersedes the Tauri and Native SDK host choices for the final architecture; this Tauri statement remains a record of the transitional production shell until C++ package gates pass.

melearner is a native Tauri desktop app instead of a web-only app because the core workflow depends on selecting local folders, scanning files, playing local media, reading local documents, and working offline without accounts or network calls.
