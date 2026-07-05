# melearner

melearner is a local-first course learner for legally obtained files already on the user's machine. The domain is about turning a folder of local learning material into a navigable library without becoming a content service.

## Language

**Library**:
The user's complete learning collection inside melearner.
_Avoid_: Catalog, marketplace, content service

**Root folder**:
The folder the user selects for scanning. Each direct child that contains learning files can become a course.
_Avoid_: Import source, sync folder

**Course**:
A collection of sections and lessons found under a course folder.
_Avoid_: Class, playlist, bundle

**Section**:
A named grouping of lessons inside a course.
_Avoid_: Chapter, module, folder group

**Lesson**:
One playable or readable learning item in a section.
_Avoid_: File, asset, resource

**Learning item**:
A neutral UI label for any lesson type, including video, audio, document, or quiz.
_Avoid_: Content item, media item

**Progress**:
The user's local completion state and last position for lessons.
_Avoid_: Analytics, tracking

**Subtitle track**:
A timed text file associated with a playable lesson.
_Avoid_: Caption file, transcript

**Local content**:
Files the user already has permission to view on their own machine.
_Avoid_: Hosted content, streamed content, downloaded content

**Prepared media**:
A cached playback-compatible copy generated only after the browser engine rejects the original file. Prefer remuxing over transcoding.
_Avoid_: Downloaded media, converted library source

**Course identity**:
The future stable identifier for a course that should survive folder renames and moves.
_Avoid_: Path-only identity

**Learning activity**:
Historical watch/read events used for future stats and heatmaps.
_Avoid_: Telemetry, analytics
