# SQLite is the durable library store

## Status

Accepted for local SQLite ownership. ADR 0011 supersedes the browser-storage, frontend write-queue, `tauri-plugin-sql`, and Tauri transport details for the final architecture; they remain transitional-shell constraints until cutover.

Course, section, lesson, subtitle, setting, and progress metadata live in local SQLite. Browser storage is reserved for tiny UI preferences only because full course libraries can exceed WebView storage quotas and user content bytes must remain in the selected folders.

Frontend database writes use the app-level write queue plus SQLite autocommit statements. Do not issue manual `BEGIN`, `COMMIT`, or `ROLLBACK` commands through `tauri-plugin-sql`; that plugin is backed by a `sqlx` pool, so separate frontend commands are not guaranteed to run on the same SQLite connection. If a future change needs a true multi-statement transaction, implement it as a Rust command that owns one pinned SQLite connection for the whole transaction.
