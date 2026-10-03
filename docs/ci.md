# Manual builds

Open the repository's Actions tab, choose a platform workflow and select Run workflow.

Builds run on native GitHub machines for Linux, Windows and macOS. They do not run on pushes or pull requests. Start only the platform affected by a change, then inspect its build and test logs. Fix a failure before starting another run.

The workflows compile the app and its existing test executables, then run the core course library and document workflow. They do not upload installers or publish releases. A passing build does not certify video playback, graphics drivers or installer compatibility on users' machines.
The macOS UI test uses the runner's native display. The offscreen Qt plugin cannot provide its window activation and graphics context.
UI tests focus a field before typing and wait for queued commands to complete, rather than assuming that closing a dialog also completes its action.
Document fixtures use `.lnk` shortcuts on Windows and symbolic links on Unix.
The viewer must reject either type when it points outside the course folder.

Dependency caches reduce repeated downloads and compilation. Save them before the application build so an application failure does not discard the dependencies. Use standard runners and keep cache storage within the included allowance. Do not enable paid runners or raise storage limits without approval.

Linux builds target an Ubuntu baseline. macOS builds target Apple silicon and use Xcode 26 for standard C++ thread support. Windows builds use the native Microsoft toolchain. Each platform still needs package deployment and clean machine playback checks before its download is offered.
