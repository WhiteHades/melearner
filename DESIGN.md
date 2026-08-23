# Design: melearner

A locked visual system for the all-C++ Qt Widgets application. Every visual change preserves the product interaction contract and applies this system through native Qt controls and custom accessible widgets.

## Scope

- The final application uses Qt 6.11 Widgets and C++23. It uses no QML, WebView, browser layout, or JavaScript UI.
- The supported content floor is 560x400 logical pixels. Compact is 560-767 px, standard is 768-1279 px, and wide is 1280 px or greater.
- Qt standard widgets provide familiar platform interaction. Custom drawing is limited to Course artwork, Progress traces, activity, document pages, and the in-window Player surface, each with explicit accessibility semantics.

## Register

Product UI. Design serves a focused learning task: scan a root folder, resume a lesson, and move through local videos, audio, and documents without distraction.

## Genre

Restrained editorial product UI.

## Theme

Inspired by the Kami paper system: warm parchment surfaces, low-chroma ink, one restrained ink-blue accent in light mode, and a warmer amber accent in cozy mode. No cool gray surfaces, no hard shadows, no glass effects, no decorative gradients that compete with course content.

## Typography

- Use the packaged melearner interface font where available and platform UI fallback fonts otherwise.
- Keep headings roman, never italic.
- Keep product labels compact and readable.
- Use tabular numbers for progress, versions, counts, and time.

## Surface Rules

- App background is warm paper, not pure white or pure black.
- Cards lift one shade above the page with a 1px warm border.
- Elevation should be a whisper shadow only; if a shadow is obvious, it is too strong.
- Rounded corners use one compact Qt style token set. Native dialogs and menus retain platform conventions.

## Accent Rules

- Accent is for primary actions, current selection, links, and progress only.
- Accent fill must always pair with the existing foreground token for contrast.
- Avoid large accent floods. Course thumbnails may provide visual richness; chrome should stay quiet.

## Layout Rules

- Preserve product routes, content, and information architecture.
- Improve rhythm through spacing, borders, and surface contrast rather than reordering UI.
- Avoid nested-card feeling by letting parent surfaces and child controls differ subtly.
- Keep behavior stable at 560x400, 768-wide, 1280-wide, and representative desktop and ultrawide viewports. Widths below 560 are outside the support contract.

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
