# Linux-first C++ cutover

On 2026-09-20 the user approved building and verifying the Linux C++ app first, then removing the superseded implementations once their replacement works. On 2026-09-22 the user narrowed the current release to Linux. Windows preparation continues for a later build and qualification on the user's Windows PC. macOS is deferred. Neither platform blocks the Linux release.

This supersedes the cross-platform sequencing gates in ADR 0012 and the implementation plan. The architecture, product behavior, fresh C++ data path, no-compatibility policy, and Linux verification requirements remain unchanged. Local development may use the installed Qt 6.11 and in-process media libraries while the distributable runtime lock is completed. A local build is not evidence of a qualified installer.
