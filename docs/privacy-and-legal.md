# Privacy and legal

## Privacy

meLearner stores its library and progress on your computer. It has no accounts,
telemetry, analytics, remote sync, update checks, or hosted course catalog.

The database is `library-v1.sqlite3` in Qt's `AppLocalDataLocation` for
organization `WhiteHades` and application `melearner-cpp-v1`. On Linux, this is
typically `$HOME/.local/share/WhiteHades/melearner-cpp-v1/library-v1.sqlite3`.
Course files remain wherever you keep them.

The database stores course identity, scan state, progress, and activity. When
upgrading an older database, meLearner creates a SQLite backup beside it named
`library-v1.sqlite3.before-schema-2-*`. Course folders may also contain
`.melearner-course.json`, a local identity marker used to match courses on later
scans. The marker holds a local course ID. The app does not send database
contents, course files, paths, or markers anywhere.

## Copyright and responsibility

meLearner organises and plays files stored on your device. It does not provide
courses or media, connect to course platforms, bypass access controls, or
download, scrape, mirror, or host third party content.

You are responsible for having the rights or permission to use your files and
for following applicable licenses, service terms, and local law.
