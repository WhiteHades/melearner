# Privacy and legal

## Privacy

meLearner stores your library and progress on your computer. It has no accounts,
analytics, remote sync, or hosted course catalog. Manual update checks contact
GitHub. Update notifications are on by default and can be turned off in Settings.
They check at startup and daily while the app is open. Successful checks are
cached for a day. Checks send the app
version, not your course files, paths, database or progress.

The database `library-v1.sqlite3` lives in Qt's `AppLocalDataLocation` for
organization `WhiteHades` and application
`melearner-cpp-v1`. On Linux, it is typically at
`$HOME/.local/share/WhiteHades/melearner-cpp-v1/library-v1.sqlite3`. Course files
remain wherever you keep them. The database stores course identity, scan state,
progress, and activity. Upgrading an older database creates a
SQLite backup beside it named `library-v1.sqlite3.before-schema-2-*`.

Course folders may contain `.melearner-course.json`, a local identity marker with
a course ID that helps match the course on later scans.

HTML and Markdown use an offline document viewer. Leaving a reading destroys
its page and access token. One private profile stays warm for up to 30 seconds
to speed up another reading in the same course. Changing course folders clears
it. The viewer blocks internet requests and does not keep cookies on disk.

## Copyright and responsibility

meLearner plays files stored on your device. It does not provide courses or
media, connect to course platforms, bypass access controls, or download, scrape,
mirror, or host third party content. You are responsible for having the rights
or permission to use your files and for following applicable licenses, service
terms, and local law.
