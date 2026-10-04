# Accessibility checks

Linux checks used Qt 6.11.2 on Omarchy, with a private display and AT-SPI bus.
The platform tree exposed course and lesson names, selection, focus, expanded
sections, button actions, Stats table cells and chart descriptions.

The Stats check found accessible names that hid numeric values. Those names
now follow the visible labels. Native UI tests check the values and keyboard
navigation. PDF tests check current page text, Unicode, page changes and
requested character bounds. Only current page text is retained, with a bounded
character geometry cache. PDF point lookup covers cached character bounds only.

Actual Orca reading remains unverified. Orca started, but the isolated session
could not deliver app utterances through its accessibility registry. Speech
Dispatcher was also unavailable. NVDA on Windows and VoiceOver on macOS still
need testing on those desktops. See [issue 57](https://github.com/WhiteHades/melearner/issues/57).
