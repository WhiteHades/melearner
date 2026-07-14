# Remove stale and redundant artifacts

Stale files, generated leftovers, and obsolete code make this local-first desktop app harder to verify and release. This repo treats that clutter as a correctness risk, not harmless noise.

## Decision

Every implementation, documentation, release, or verification pass must remove artifacts it creates once they are no longer needed. Agents must also remove already-present stale files when the stale status is proven by live docs, tests, release notes, or current code. Keeping stale material "just in case" is not an acceptable default in this repository. The default action for proven stale material is deletion in the same change, not preserving it for a later cleanup pass.

This applies to:

- Generated build output that is not part of source control.
- Temporary screenshots, fixture folders, package staging folders, logs, and one-off verification files.
- Completed execution plans that duplicate current ADRs or product docs.
- Obsolete code, comments, branches, fields, or structures made unnecessary by the same change.
- Duplicate docs that repeat older behavior after the canonical docs have moved on.
- Superseded UI paths, settings, feature flags, generated components, or package files after a replacement is fully wired and verified.

Deletion must stay conservative around user data and required tooling. Do not remove:

- User libraries, app databases, or course marker files outside isolated test fixtures.
- Required source files, package manager lockfiles, or active dependency installs.
- Active local build caches used for iterative development installs, especially `.next`, `out`, `src-tauri/target`, and `tsconfig.tsbuildinfo`. These ignored paths are not stale merely because they are generated; deleting `src-tauri/target` forces slow full Tauri/Rust rebuilds.
- Release artifacts before they have been installed, uploaded, checksummed, or otherwise consumed.
- Current behavior that still has a tested product purpose. Previous melearner database compatibility, migration, backup, restore, and rollback paths are not current behavior and must not be retained.

## Consequences

- Future agents should not keep stale files "just in case" once the repo has a canonical replacement.
- Cleanup is part of the task, not an optional follow-up.
- Agents should proactively scan for stale references after changing behavior, especially in docs, ADRs, package metadata, desktop entries, and install/release packaging.
- Agents should remove proven stale files, code paths, docs, generated outputs, and temporary artifacts without waiting for an extra prompt.
- If a file looks redundant but still has unclear ownership or current product value, leave it in place and name the uncertainty in the final summary.
- Verification should include a targeted artifact scan after release or build work. For iterative local development, remove package files, package staging folders, temp screenshots, logs, and one-off verification files while keeping active build caches. For deliberate cold rebuilds or final source-only cleanup, explicitly state that caches will be removed and expect the next build to be slower.
