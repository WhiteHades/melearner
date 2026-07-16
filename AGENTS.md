# melearner Agent Guide

## Native cutover policy

- The fully native line uses one current schema at a time. Do not add import, migration, backup, restore, rollback, or compatibility paths for pre-native databases, obsolete schemas, or obsolete artifacts.
- An unchanged current native database survives native-to-native package replacement. A future schema replacement uses a new fresh data path and decision instead of compatibility code. Current `.melearner-course.json` markers are domain inputs, not previous-version artifacts.
- Delete deprecated versions, artifacts, features, functions, and fallback paths in the change that supersedes them. Do not retain legacy behavior as a fallback.

## Agent skills

### Issue tracker

Issues and PRDs for this repo live in GitHub Issues for `WhiteHades/melearner`; external PRs are not a triage request surface by default. See `docs/agents/issue-tracker.md`.

### Triage labels

Use the canonical triage label vocabulary. See `docs/agents/triage-labels.md`.

### Domain docs

This is a single-context repo with one root `CONTEXT.md` and ADRs under `docs/adr/`. See `docs/agents/domain.md`.

### Product plans

Stats, heatmaps, learning activity, and durable course identity behavior are tracked in `docs/stats-and-identity-plan.md`. Durable course identity decisions are recorded in `docs/adr/0008-durable-course-identity-uses-local-fingerprints.md`.

### Cleanup discipline

This repo does not keep stale generated artifacts, completed temporary plans, redundant docs, or obsolete code. See `docs/adr/0009-remove-stale-and-redundant-artifacts.md` before build, release, documentation, or cleanup work.
