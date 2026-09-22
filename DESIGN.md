---
name: melearner
description: A cream-and-red native interface for learning from local course files.
colors:
  paper: "#faf7f2"
  surface: "#fffdfa"
  ink: "#2c211e"
  border: "#e7e0d8"
  hover: "#f3e7e1"
  accent: "#a72c23"
  on-accent: "#fffefa"
  dark-paper: "#1b1917"
  dark-surface: "#24211e"
  dark-ink: "#f5f0e9"
  dark-border: "#403a34"
  dark-hover: "#382b28"
  dark-accent: "#efaaa1"
  dark-on-accent: "#181817"
  cozy-paper: "#f8f0e3"
  cozy-surface: "#fff9ed"
typography:
  display:
    fontFamily: "Liberation Serif"
    fontWeight: 700
rounded:
  control: "7px"
  panel: "10px"
  item: "4px"
spacing:
  compact: "8px"
  group: "12px"
  section: "16px"
  shell-inline: "24px"
---

# melearner design

## Overview

Cream surfaces, deep red actions, serif headings, and outline icons define the learning workspace. The implementation uses C++23 and Qt Widgets. Native controls provide keyboard navigation, focus, menus, and accessibility semantics.

Course content leads the window. The interface supports choosing a root folder, opening a course, and resuming a lesson with little navigation overhead.

## Colors

Warm paper and ink form the light appearance. Red identifies primary actions, keyboard focus, and activity. Selected rows and menus use a pale red fill. Dark appearance uses a lighter red. Cozy appearance warms the paper and surface while retaining the same red accent.

The palette and widget styles are defined in `MainWindow::applyAppearance`. Activity cells derive their colors from that palette and choose the higher-contrast foreground. System high contrast uses the native black-and-white palette and removes custom widget styling.

## Typography

Liberation Serif regular and bold are embedded for editorial headings. Route headings use 2.3 times the base font size, reduced to 1.5 times in compact windows. Lesson headings use 1.6 times, and metric values use 1.65 times. Controls, tables, and body text retain the platform UI font and user text sizing. Code blocks use the platform fixed-width font. The bundled fonts include their SIL Open Font License.

Long course and lesson titles elide in navigation, with their full text available through tooltips and accessibility names. Descriptions wrap. Button labels remain on one line.

## Layout

The content floor is 560 by 400 logical pixels. Below 768 pixels, the course outline and lesson occupy separate panes with an explicit switch. Increased text size raises that threshold. Standard windows show the lesson on the left and its resizable outline on the right. Notes open on request, alongside the lesson in wide windows or in a dialog when space is limited. Document and lesson navigation buttons stack when their labels no longer fit side by side.

At wide sizes a left navigation rail contains Courses, Learning activity, lesson notes, keyboard help, and Settings. The rail collapses below a text-aware threshold of at least 1040 pixels. Compact windows retain library tabs and header actions. Search sits above the route heading. A static plant, books, and mug illustration appears beside library headings when space permits. Folder changes and rescanning live in Settings. The initial empty Library exposes Choose root folder directly. The library shows the root folder name; its tooltip and accessibility description retain the full path.

The video widget stays attached to its rendering context while the layout changes. Lists use bounded pages and native item views.

Library tabs contain Courses and Stats. Stats totals use four columns when space permits and two on narrower windows. Breakdown tables stack when needed. The page scrolls vertically; wide tables scroll within their own bounds. Layout thresholds also account for increased text size.

## Elevation & Depth

Warm surface colors and fine borders distinguish controls and panels. The application does not add decorative widget shadows. The small header artwork retains its soft contact shadow.

## Shapes

Controls use compact rounded corners; course rows, the resume panel, statistics, and the outline use larger panel corners. Borders keep their width when focus changes, so text and neighboring controls stay in place. Native window decoration remains under desktop control.

## Components

### Keyboard

Keyboard users are first-class alongside pointer users. Native Qt focus and
editing behavior remain authoritative; Vim-style motions are scoped to lists,
the course outline, the player, and read-only lesson content. `j`/`k`, `gg`/`G`,
`Ctrl+D`/`Ctrl+U`, lesson stepping, completion, notes, and player controls are
available without capturing text-field or IME input. `?`/`F1` opens a searchable
shortcut list and `:`/`Ctrl+Space` opens the same action registry as a command
palette. Menus and that popup are generated from the same `QAction` commands so
labels and bindings do not drift.

### Actions and content

The resume button uses the accent and its paired foreground. Secondary actions use the surface color. Hover, pressed, focused, and disabled states remain distinct.

Course rows use a book outline icon, course title, exact completed-lesson count, and a completion bar. They remain native paged list items, not one widget per course. The resume panel uses the same real course completion count. There are no generated video thumbnails.

Video controls sit over the bottom of the video on a dark, high-contrast strip. The timeline, play button, time, volume, Settings, and fullscreen stay together. Speed, audio tracks, subtitles, and chapters use native menus under Settings. Narrow windows and larger text split the controls into two rows without shrinking text.

Library tabs use a red underline for the active destination. Menus, dialogs, sliders, and tables retain native interaction behavior.

Icons are drawn with QPainter at the requested display scale. Transparent pixmaps, one rounded stroke weight, and accessible text labels keep them consistent without an icon font or SVG runtime dependency.

Activity cells expose the date and exact values to assistive technology. Arrow-key selection also displays those values below the grid. Progress time is derived from lesson position, not elapsed viewing time.

Player controls disappear after 2.5 seconds of inactivity during video playback. Mouse movement or keyboard focus reveals them immediately. Paused playback, audio lessons, focused controls, and open menus keep them visible. The exit fade uses the native widget animation duration, capped at 160 ms, and is immediate when that duration is zero or high contrast is enabled. Navigation, seeking, selection, and focus update immediately.

## Do's and Don'ts

- Preserve native keyboard interaction and system text sizing.
- Check light, dark, cozy, and high-contrast appearances before changing palette behavior.
- Check compact and desktop layouts with normal and doubled text.
- Keep the cream-and-red logo and reserve solid accent fills for actions and state.
- Do not introduce a browser runtime, QML, or JavaScript UI.
- Do not add decorative motion, gradients, or nested cards around course content.
