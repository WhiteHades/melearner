# Design: melearner

A locked visual system for the app. Every visual change should preserve the current product structure and component ownership while applying this system.

## Register

Product UI. Design serves a focused learning task: scan a root folder, resume a lesson, and move through local videos, audio, and documents without distraction.

## Genre

Restrained editorial product UI.

## Theme

Inspired by the Kami paper system: warm parchment surfaces, low-chroma ink, one restrained ink-blue accent in light mode, and a warmer amber accent in cozy mode. No cool gray surfaces, no hard shadows, no glass effects, no decorative gradients that compete with course content.

## Typography

- Use each shell's committed interface face: the existing web font stack during the transition and `native-app/src/fonts/melearner-ui.ttf` in the native shell.
- Keep headings roman, never italic.
- Keep product labels compact and readable.
- Use tabular numbers for progress, versions, counts, and time.

## Surface Rules

- App background is warm paper, not pure white or pure black.
- Cards lift one shade above the page with a 1px warm border.
- Elevation should be a whisper shadow only; if a shadow is obvious, it is too strong.
- Rounded corners should stay consistent with existing shadcn/Radix primitives.

## Accent Rules

- Accent is for primary actions, current selection, links, and progress only.
- Accent fill must always pair with the existing foreground token for contrast.
- Avoid large accent floods. Course thumbnails may provide visual richness; chrome should stay quiet.

## Layout Rules

- Preserve existing routes, components, content, and information architecture.
- Improve rhythm through spacing, borders, and surface contrast rather than reordering UI.
- Avoid nested-card feeling by letting parent surfaces and child controls differ subtly.
- Keep responsive behavior stable at 320, 375, 414, 768, and desktop widths.

## Motion

- Motion exists only for feedback: hover, focus, loading, opening menus, and thumbnail fade-in.
- Prefer 150-250ms transitions.
- Do not animate layout properties.

## Anti-Slop Checks

- No gradient text.
- No purple-blue SaaS glow palette.
- No identical icon-card feature rows.
- No side-stripe card accents.
- No fake browser, phone, terminal, or IDE chrome.
- No emoji as primary feature icons.
