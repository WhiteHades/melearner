# Domain Docs

How the engineering skills should consume this repo's domain documentation when exploring the codebase.

## Before exploring, read these

- `CONTEXT.md` at the repo root.
- `docs/adr/` for architectural decisions that touch the area you are about to work in.
- `DESIGN.md` before UI, layout, motion, visual-system, or component changes.
- `docs/stats-and-identity-plan.md` before changing stats, heatmaps, storage breakdowns, or learning activity.
- `docs/adr/0008-durable-course-identity-uses-local-fingerprints.md` before changing durable course identity behavior.
- `docs/adr/0009-remove-stale-and-redundant-artifacts.md` before build, release, documentation, or cleanup work.

If either path is missing, proceed silently. The `/domain-modeling` skill creates or extends them when terms or decisions get resolved.

## File structure

This is a single-context repo:

```text
/
├── CONTEXT.md
├── DESIGN.md
├── docs/stats-and-identity-plan.md
├── docs/adr/
└── src-tauri/
```

## Use the glossary's vocabulary

When output names a domain concept in an issue title, refactor proposal, hypothesis, or test name, use the term as defined in `CONTEXT.md`.

If the concept is missing from the glossary, either the term is invented language the project does not use, or there is a real gap to add through `/domain-modeling`.

## Flag ADR conflicts

If output contradicts an existing ADR, surface it explicitly rather than silently overriding it.
