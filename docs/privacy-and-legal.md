# Privacy and Legal

## Privacy

melearner is local-first.

- No account
- No telemetry
- No analytics
- No remote sync
- No update check
- No hosted course catalog

Local data paths:

- Database: `$HOME/.local/share/melearner/melearner.db`
- Logs: `~/.melearner/`
- User course files: wherever you keep them

Course identity data, fingerprints, missing-folder state, notes, progress, and lesson activity stay in the local SQLite database. Fingerprints are derived from local course structure and learning-item metadata so renamed or moved folders can reconnect to existing progress.

Marker files are optional. If enabled, melearner writes `.melearner-course.json` into available course folders so later scans can match by marker ID. If disabled, melearner only reads the folders you choose to scan and writes app metadata under its local app data paths.

## Legal Disclaimer

melearner is a local media player and file organizer. It does not distribute, stream, download, host, or facilitate access to any content.

melearner does not:

- Provide courses, videos, audio, or documents
- Include a built-in course collection or content source
- Connect to Udemy, Coursera, Skillshare, Pluralsight, or similar platforms
- Bypass DRM, paywalls, licenses, or terms of service
- Download, scrape, mirror, or index third-party content

You are responsible for the legality of the files on your machine. Use melearner only with files you own, have a valid license or subscription for, or are allowed to view.

melearner is provided as-is without warranty. The developers are not liable for user actions that violate copyright law, terms of service, or local regulations.
