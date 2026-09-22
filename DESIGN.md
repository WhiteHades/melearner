---
name: melearner
description: A cream-and-red native interface for learning from local course files.
colors:
  paper: "#faf7f0"
  surface: "#fffdf8"
  ink: "#302a26"
  border: "#d9cfc2"
  hover: "#f1e7da"
  accent: "#b82e35"
  on-accent: "#fffdf8"
  dark-paper: "#211d1b"
  dark-surface: "#2a2522"
  dark-ink: "#f5ede1"
  dark-border: "#574a43"
  dark-hover: "#3b302c"
  dark-accent: "#f19b9d"
  dark-on-accent: "#211d1b"
  cozy-paper: "#fff2d5"
  cozy-surface: "#fff8e8"
rounded:
  control: "6px"
  item: "4px"
spacing:
  compact: "8px"
  group: "12px"
  section: "16px"
  shell-inline: "20px"
---

# melearner design

## Overview

The user chose a modern, minimal, shadcn-inspired interface with a cream-and-red logo. The implementation uses C++23 and Qt Widgets. Native controls provide keyboard navigation, focus, menus, and accessibility semantics.

Course content leads the window. The interface supports choosing a root folder, opening a course, and resuming a lesson with little navigation overhead.

## Colors

Warm paper and ink form the light appearance. Red identifies primary actions, selection, and activity. Dark appearance uses a lighter red with dark text on selected controls. Cozy appearance warms the paper and surface while retaining the same red accent.

The palette and widget styles are defined in `MainWindow::applyAppearance`. Activity cells derive their colors from that palette and choose the higher-contrast foreground. System high contrast uses the native black-and-white palette and removes custom widget styling.

## Typography

Use the platform UI font and honor user text sizing. Route headings use 1.5 times the base size, metric values use 1.25 times, and section headings use bold weight. Code blocks use the platform fixed-width font. There is no bundled interface font.

Long course and lesson titles elide in navigation, with their full text available through tooltips and accessibility names. Descriptions wrap. Button labels remain on one line.

## Layout

The content floor is 560 by 400 logical pixels. Below 768 pixels, the course outline and lesson occupy separate panes with an explicit switch. Increased text size raises that threshold. Standard windows show the resizable outline beside the lesson. Wide windows can show notes alongside the lesson when the text size leaves enough room. Document and lesson navigation buttons stack when their labels no longer fit side by side.

The video widget stays attached to its rendering context while the layout changes. Lists use bounded pages and native item views.

Library tabs contain Courses and Stats. Stats totals use four columns when space permits and two on narrower windows. Breakdown tables stack when needed. The page scrolls vertically; wide tables scroll within their own bounds. Layout thresholds also account for increased text size.

## Elevation & Depth

Warm surface colors and fine borders distinguish controls. The application does not add decorative shadows. Stats sections use headings and spacing without enclosing cards.

## Shapes

Controls use compact rounded corners. Borders keep their width when focus changes, so text and neighboring controls stay in place. Native window decoration remains under desktop control.

## Components

Primary playback and resume buttons use the accent and its paired foreground. Secondary actions use the surface color. Hover, pressed, focused, and disabled states remain distinct.

Library tabs use a red underline for the active destination. Menus, dialogs, sliders, and tables retain native interaction behavior.

Activity cells expose the date and exact values to assistive technology. Arrow-key selection also displays those values below the grid. Progress time is derived from lesson position, not elapsed viewing time.

App-authored transitions are currently off. Navigation, seeking, selection, and focus update immediately. Any future animation must have a specific feedback purpose and honor reduced-motion preferences.

## Do's and Don'ts

- Preserve native keyboard interaction and system text sizing.
- Check light, dark, cozy, and high-contrast appearances before changing palette behavior.
- Check compact and desktop layouts with normal and doubled text.
- Keep the cream-and-red logo and reserve solid accent fills for actions and state.
- Do not introduce a browser runtime, QML, or JavaScript UI.
- Do not add decorative motion, gradients, or nested cards around course content.
