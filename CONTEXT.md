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

**Player**:
The in-app surface for video and audio lessons.
_Avoid_: External player, playback service

**Course identity**:
The local identity model that keeps a course connected to its progress when its folder is renamed, moved, temporarily missing, or scanned again. It uses paths, automatic local marker IDs, and conservative fingerprints.
_Avoid_: Path-only identity

**Learning activity**:
Historical local lesson-progress events used for stats and heatmaps.
_Avoid_: Telemetry, analytics
