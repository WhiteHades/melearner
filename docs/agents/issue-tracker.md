# Issue tracker: GitHub

Issues and PRDs for this repo live as GitHub issues in `WhiteHades/melearner`. Use the `gh` CLI for all operations.

## Conventions

- Create an issue: `gh issue create --repo WhiteHades/melearner --title "..." --body "..."`. Use a heredoc for multi-line bodies.
- Read an issue: `gh issue view <number> --repo WhiteHades/melearner --comments`, including labels and comments when triaging.
- List issues: `gh issue list --repo WhiteHades/melearner --state open --json number,title,body,labels,comments --jq '[.[] | {number, title, body, labels: [.labels[].name], comments: [.comments[].body]}]'` with appropriate `--label` and `--state` filters.
- Comment on an issue: `gh issue comment <number> --repo WhiteHades/melearner --body "..."`.
- Apply or remove labels: `gh issue edit <number> --repo WhiteHades/melearner --add-label "..."` or `--remove-label "..."`.
- Close an issue: `gh issue close <number> --repo WhiteHades/melearner --comment "..."`.

Prefer passing `--repo WhiteHades/melearner` so commands still target the right issue tracker when run outside the clone.

## When a skill says "publish to the issue tracker"

Create a GitHub issue in `WhiteHades/melearner`.

## When a skill says "fetch the relevant ticket"

Run `gh issue view <number> --comments`.
