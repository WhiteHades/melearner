# Domain Docs

How the engineering skills should consume this repo's domain documentation when exploring the codebase.

## Before exploring, read these

- `CONTEXT.md` at the repo root.
- `PRODUCT.md` for users, purpose, product principles, and accessibility intent.
- `docs/adr/` for architectural decisions that touch the area you are about to work in.
- `DESIGN.md` before UI, layout, motion, visual-system, or component changes.
- `docs/stats-and-identity-plan.md` before changing stats, heatmaps, storage breakdowns, or learning activity.
- `docs/adr/0010-embedded-libmpv-native-playback.md` before changing playback, subtitles, player controls, or native player behavior.
- `docs/adr/0012-all-cpp23-qt-widgets-application.md`, `docs/specs/fully-native-melearner.md`, `docs/specs/cpp23-implementation-plan.md`, and `docs/research/cpp23-qt-overhaul.md` before changing the final application, module seams, cutover, performance, or packages.
- `docs/adr/0007-course-cards-do-not-generate-runtime-video-thumbnails.md` before changing course artwork, cards, or thumbnail behavior.
- `docs/adr/0008-durable-course-identity-uses-local-fingerprints.md` before changing durable course identity behavior.
- `docs/adr/0009-remove-stale-and-redundant-artifacts.md` before build, release, documentation, or cleanup work.

If a listed path is missing, proceed silently. The `/domain-modeling` skill creates or extends domain docs when terms or decisions get resolved.

## File structure

This is a single-context repo:

```text
/
|-- CONTEXT.md
|-- PRODUCT.md
|-- DESIGN.md
|-- docs/specs/
|   |-- fully-native-melearner.md
|   `-- cpp23-implementation-plan.md
|-- docs/stats-and-identity-plan.md
|-- docs/adr/
|-- cpp-app/
|-- crates/melearner-core/  (superseded oracle before cutover)
|-- native-app/             (superseded oracle before cutover)
`-- src-tauri/              (transitional production before cutover)
```

Three implementation scopes coexist until the ADR 0012 cutover. `src-tauri/` is the transitional production shell. `crates/melearner-core/` and `native-app/` are frozen parity oracles from the superseded Native SDK line. `cpp-app/` is the unreleased final line. Keep the old scopes runnable only as required by parity gates. Do not route production through the C++ line before installed-package acceptance, and do not add compatibility adapters between data paths.

## Use the glossary's vocabulary

When output names a domain concept in an issue title, refactor proposal, hypothesis, or test name, use the term as defined in `CONTEXT.md`.

If the concept is missing from the glossary, either the term is invented language the project does not use, or there is a real gap to add through `/domain-modeling`.

## Flag ADR conflicts

If output contradicts an existing ADR, surface it explicitly rather than silently overriding it.
