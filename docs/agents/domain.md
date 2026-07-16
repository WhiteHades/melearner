# Domain Docs

How the engineering skills should consume this repo's domain documentation when exploring the codebase.

## Before exploring, read these

- `CONTEXT.md` at the repo root.
- `docs/adr/` for architectural decisions that touch the area you are about to work in.
- `DESIGN.md` before UI, layout, motion, visual-system, or component changes.
- `docs/stats-and-identity-plan.md` before changing stats, heatmaps, storage breakdowns, or learning activity.
- `docs/adr/0010-embedded-libmpv-native-playback.md` before changing playback, subtitles, player controls, or native player behavior.
- `docs/adr/0011-native-sdk-zig-shell-with-rust-static-core.md`, `docs/specs/fully-native-melearner.md`, and `docs/research/native-sdk-overhaul.md` before changing the final native shell, core boundary, cutover, or packages.
- `docs/adr/0007-course-cards-do-not-generate-runtime-video-thumbnails.md` before changing course artwork, cards, or thumbnail behavior.
- `docs/adr/0008-durable-course-identity-uses-local-fingerprints.md` before changing durable course identity behavior.
- `docs/adr/0009-remove-stale-and-redundant-artifacts.md` before build, release, documentation, or cleanup work.

If a listed path is missing, proceed silently. The `/domain-modeling` skill creates or extends domain docs when terms or decisions get resolved.

## File structure

This is a single-context repo:

```text
/
|-- CONTEXT.md
|-- DESIGN.md
|-- docs/stats-and-identity-plan.md
|-- docs/adr/
|-- crates/melearner-core/
|-- native-app/
`-- src-tauri/
```

Two implementation scopes coexist until the ADR 0011 cutover. `src-tauri/` is the transitional production shell. `crates/melearner-core/` and `native-app/` contain the shared Rust crate and unreleased final-native line. The Tauri Cargo crate already directly reuses the core scanner; preserve and regression-test that frozen Rust reuse without requiring decoupling. Do not redirect the transitional shell through the C ABI, native database ownership, Native SDK effects, or Native SDK UI, and do not introduce another adapter. Do not apply React/WebView details as final-native contracts or final-native-only package rules to the transitional shell.

## Use the glossary's vocabulary

When output names a domain concept in an issue title, refactor proposal, hypothesis, or test name, use the term as defined in `CONTEXT.md`.

If the concept is missing from the glossary, either the term is invented language the project does not use, or there is a real gap to add through `/domain-modeling`.

## Flag ADR conflicts

If output contradicts an existing ADR, surface it explicitly rather than silently overriding it.
