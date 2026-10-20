# The native visual system against the approved prototype (2026-10-16)

The approved Windows 11 shell prototype (PS5_Proton-design `prototypes/windows11-shell`) is the visual reference
for the native shell ([WINDOWS11_PLAN.md](../WINDOWS11_PLAN.md)). This records every difference found between it and
the native tokens and theme (`themes/windows11/appearance.json`, `source/theme.ini` as `tools/build-wine-theme.py`
builds it), what changed, and what still differs, with the host oracle's and the console's results.

## How it was compared

- **Rendered pixels, not CSS values.** The prototype was served read-only and captured in headless Chromium
  154.0.8037.97 at 1920 x 1080, scale 1. Its transparency effects were turned off (a window's bar is then the solid
  Mica fallback, a known colour) and its brightness set to 100 (its screen filter otherwise darkens every pixel by
  2.4%; the first captures read #ededed for #f3f3f3 and #bf291b for #c42b1c).
- **Measured, not modelled.** Skia's antialiasing differs between dark ink on light and light ink on dark (a
  maximize-glyph corner is 0.124 dark, 0.112 light) and its rounding differs from round-to-nearest, so a model of
  "token alpha times coverage" missed the captures by up to 3 levels, and librsvg's rendering of the same glyph
  strokes by up to 66. The theme's bitmaps are therefore fitted to the prototype's own pixels:
  `tools/measure-prototype-caption.mjs` (caption buttons, every state, light and dark) and
  `tools/measure-prototype-controls.mjs` (the controls gallery's check boxes, radio buttons and push buttons, over
  black and over white for exact colour and alpha). `tools/pw_caption_buttons.py` chooses each pixel so that Wine's
  own arithmetic (uxtheme's premultiply, the DIB engine's source-over) lands on the measured value.
- **Oracle.** `tests/theme-parts-probe.c` draws every changed part through Wine's real uxtheme into bitmaps and
  compares each pixel. Pinned Wine on Linux (host oracle, headless), PE64 and PE32: **13/13 checks** (all 64
  caption-button images, 28 check box and radio images, 12 push button and 8 edit border images, the group box and
  the progress colours equal their references to the level); the previous theme fails every one of them (0/32,
  0/20, 0/8). On the console (`gate-2026-10-16-win11-01`, PE32 and PE64) the same 13/13, and the appearance probe's
  captured caption strip equals Wine's composite of the measured bitmaps: 0 wrong of 4322 pixels light, 0 of 4457
  dark, the hovered button included.

## Window frame and caption buttons (pixel-exact)

| | Native before | Prototype | Now |
| --- | --- | --- | --- |
| Visible frame | 4 px drawn (raised edge, sizing border) | 1 px border, nothing outside | invisible resize border, 1 px visible (patch 1139) |
| Border | none from the compositor | SurfaceStrokeColorDefault #66757575 over the window's background | the compositor's default border, alpha 0x66 (server, `src/pw_window_compose.c`) |
| Title bar | 35 rows + a 1 px line beneath | 32 px under the border | 32 (iCaptionHeight 31, no line) |
| Caption buttons | inset 2 px, 1 px gaps, `SM_CYSIZE - 2` tall | 46 x 32, side by side from the top right | 46 x 32 (patch 1139) |
| Rest | opaque plate #f3f3f3 (dark #353535) | none | none |
| Hover | never (Wine had no hot tracking) | SubtleFillColorSecondary; close #C42B1C, white glyph | hot tracking with `TME_NONCLIENT` (patch 1139); plates as measured |
| Pressed | #dcdcdc / close #b02518 | SubtleFillColorTertiary, glyph secondary; close 90% red, 70% white | as measured |
| Inactive | same as active | glyphs TextFillColorDisabled until hovered; bar #f3f3f3 / #202020 | as measured; InactiveCaption #f3f3f3 / #202020 |
| Glyphs | 13 px Fluent Light sprites (low underscore, blue on hover) | 10 x 10 strokes at 18,11 | the measured masks |
| Icon, title | at 2 px and icon + 2 px | 16 px margins: icon at 17, title at 49 | 16 px margins |
| `DWMWA_EXTENDED_FRAME_BOUNDS` | `GetWindowRect` | the visible bounds | the visible bounds |

The active bar is the native Mica fallback (#eef2f8 / #212529) because Wine paints the caption opaque; the
prototype's active bar is real Mica, which over Bloom measures #edf2f7 / #212529 (Settings' title bar), within one
level. Over the reference bar (an inactive window) the buttons are the prototype's pixels exactly.

## Wallpaper and taskbar (2026-10-16, console)

Compared with the prototype at 1920 x 1080 with File Explorer open (`taskbar-2026-10-16-03`, capture 3, against the
prototype's own capture with the user's local Bloom):

| | Before | Now | Console against the prototype |
| --- | --- | --- | --- |
| Wallpaper | Bloom resized with Lanczos (up to 27 levels off) | libjpeg's half-scale decode, as Chromium draws it (`tools/build-wine-theme.py`) | 98.4% exact, 99.8% within 1, worst 2 |
| Taskbar layout | Start and open windows only | the prototype's slots (`src/pw_taskbar_layout.h`, equal to its boxes) | every box in place |
| Taskbar chrome | GDI drawing | the prototype's own pixels (`themes/windows11/taskbar`, patch 1140) | Start to Terminal: 91.2% within 1, all within 3; tray icons: all within 1 |
| Bar material | Mica (80% #eef2f8 over the wallpaper: blue) | the prototype's taskbar acrylic (patch 1141, `PW_BACKDROP_TASKBAR`) | 93.8% within 1, worst 4 (the prototype's own noise is random) |
| Top line | opaque | the prototype's 5.9% divider | 93.3% within 1, worst 3 |
| Clock text | GDI, whole-pixel advances (2.9% of ink exact, worst 219) | the prototype's own glyphs on Skia's quarter-pixel grid (`src/pw_text_glyphs.h`) | ink 67.7% exact, 99.4% within 1, worst 2 |

Still different on the taskbar:

- **Clock text** (console `clock-text-2026-10-16-01`, light). GDI placed glyphs on whole-pixel advances, so
  "10/16/2026" was 58 px natively against 61.1 and the tray sat 3 px right of the prototype's; on the console
  (`taskbar-2026-10-16-03`) 2.9% of the clock's 515 ink pixels were exact, 3.7% within a level. Chromium 154 does not
  draw this font through FreeType: web fonts go through Fontations (skrifa's light autohinter, Skia's analytic
  coverage, gamma 1.1875 / contrast 0.2 by paint luminance), and FreeType's light autohinter matches only 48% of its
  ink. Patch 1140 now draws the clock with the prototype's own glyphs on Skia's quarter-pixel grid
  (`src/pw_text_glyphs.h`): host-composed strings equal Chromium's in every channel (light) and to a level where "/"
  overlaps a digit (dark). On the console the clock's ink is now 67.7% exact and 99.4% within a level, worst 2, and
  the tray is back in place (78.1% exact, 99.97% within a level). What is left is a level or two: Chromium draws the
  text into the bar's tinted layer before compositing it, and the acrylic's noise is random. Dark: host only (33.0% of
  ink exact, 99.4% within a level).
- **The notification badge** is drawn only for real notifications; the prototype's fresh state seeds two.
- **Task View** is drawn but opens nothing yet; Search, Quick Settings, the clock and the hidden icons open their
  flyouts (below).
- **Hover and press states** use the measured plates; the prototype's pressed glyph shrink (scale 0.9) and its fades
  are not drawn.

## Start, Search and the taskbar's flyouts (2026-10-16, console)

The native Start (`wine/ps5/pw_shell_start.c`) is the prototype's 600 x 566 panel with its three views (Start, All
apps, Search); Quick Settings, the notification centre with the calendar and the hidden icons are
`wine/ps5/pw_shell_flyouts.c` and patch 1161. Fixed chrome is the prototype's own pixels (`themes/windows11/start`,
`themes/windows11/flyouts`); the materials are the prototype's .surface-flyout (patch 1160 and
`src/pw_window_effects.h`), host-checked with the compositor's C code against the prototype's bare panels over Bloom:

| Material | Prototype | Host check (compositor vs prototype) |
| --- | --- | --- |
| Start, Search | acrylic-base: blur(30px) contrast(0.15) brightness(1.95), 90% #f3f3f3; dark saturate(125%), 92% #202020 | inside 100% within 1 (light), 95.2% within 1 and all within 2 (dark) |
| Quick Settings, notifications, calendar, hidden icons | acrylic: the same chain, 85% #fcfcfc; dark 96% #2c2c2c | inside 100% / 99.9% within 1 |
| Stroke | 1 px SurfaceStrokeColorFlyout over the untinted backdrop (background-clip: padding-box) | light 87% within 1, dark 62% (the blur at the very edge) |
| Shadow | --shadow-28: 0 0 8px 12% and 0 14px 28px 14% (24% / 28% dark), Gaussians of deviation 4 and 14 | 91-100% within 1 outside the 8 px corners |

Two findings made the material match: Chromium's backdrop-filter blurs only what lies under the element (edges
repeated), not the screen around it, and the tint stops inside the stroke.

Console, `shell-surfaces-2026-10-16-04` (`tools/run-shell-surfaces.py`: every surface opened from the taskbar and
captured), against the prototype's capture:

| Region | Light: within 1 / within 2 / worst | Dark |
| --- | --- | --- |
| Start, whole panel and shadow | 92.9% / 96.1% (text and different pins) | 91.6% / 96.0% |
| Search pill | 97.3% / 99.8% / 3 | 72.1% / 98.4% / 4 |
| Pinned heading and All apps | 99.9% / 100% / 7 | 95.6% / 99.9% / 5 |
| File Explorer, Settings, Notepad icons | 99.8%, 95.3%, 98.0% | 99.3%, 90.2%, 95.0% |
| Empty material | 100% / 100% / 2 | 94.7% / 100% / 2 |
| Footer (account and power row) | 62.8-77.2% / 95.6-98.6% | 94.1-94.3% / 99.7-99.9% |
| Power glyph, avatar | 69.8%, 80.0% / 96.3%, 98.2% | 97.8%, 87.1% / 99.4%, 99.8% |
| Shadow above, left, below | 96.4%, 98.5%, 98.0% | 92.3%, 99.5%, 97.6% |
| Search, whole panel and shadow | 96.5% / 97.4% | - |
| Quick Settings tile, chevron | 99.9%, 100% | - |
| Quick Settings footer (no battery or edit button) | 76.1% / 97.5% | - |
| Notification centre header, empty state | 98.9%, 99.6% | - |
| Calendar weekday row, toggle and paging | 93.3% (all within 2), 98.2% | - |
| Calendar day numbers (text) | 85.0% / 90.6% | - |

Still different in Start and its flyouts:

- **Text.** Names, dates, the user name and labels are GDI text in the shell's font; the prototype's are Skia's. The
  glyph atlas the taskbar clock now uses (src/pw_text_glyphs.h, another session) is the way to identical labels.
- **What is pinned and recent** is the prefix's own: the built-in apps in the prototype's order (it has a Calculator
  this runtime does not), then the desktop's shortcuts; Recommended appears only when the shell has recent files.
- **Quick Settings** shows what the console has: the network tile (on while an adapter is up with an address; Wi-Fi's
  glyph for a wireless adapter, the prototype's globe for the console's wired one) and Settings. The prototype's other tiles, its brightness and volume sliders and battery are not drawn: Wine's
  IAudioEndpointVolume is a stub and the title has no master gain yet, so a volume slider would move nothing.
- **The notification centre** shows the prototype's empty state (no program posts notifications to this shell); its
  do-not-disturb bell and the calendar's Focus timer are not drawn.
- **Hover and press plates** are drawn natively (ControlFillColorSecondary/Tertiary in a hairline), not measured.
- **Task View** still opens nothing.

## File Explorer (2026-10-16, console)

File Explorer is the prototype's Windows 11 Explorer (Wine patches 1150-1151, `wine/ps5/pw_shell_explorer.h`): its
layout (`src/pw_explorer_layout.h`, equal to the prototype's boxes, fractions kept) and its own pixels
(`themes/windows11/explorer`), on the prototype's Mica Alt (`src/pw_window_effects.h`). Console,
`explorer-2026-10-16-04` (desktop-session: Explorer at the prototype's 410,166, 1100 x 700), against the prototype's
captures at brightness 100 (equal to the Design session's reference set within a level):

| Region | Home: exact / within 1 / worst | Documents |
| --- | --- | --- |
| Whole window | 93.4% / 98.6% / 228 (was 5.3% within 1) | 93.7% / 98.7% / 228 |
| Title bar's Mica Alt | 16.7% / 100% / 1 | 17.7% / 100% / 1 |
| Tab | 94.6% / 99.95% / 2 | 91.3% / 97.0% (its name is GDI text) |
| Caption buttons | 26.8% / 99.6% / 16 (the rounded corner) | 19.2% / 99.6% / 16 |
| Address row | 99.7% / 99.99% / 3 | 97.1% (the crumbs and "Search Documents" are text) |
| Command bar | 99.4% / 100% / 2 | 99.7% within 1 (Filter is drawn disabled) |
| Navigation pane | 99.6% / 99.99% / 5 | 99.99% within 1 |
| Home above Recent / the column header | 99.5% / 100% / 1 | the rows are the prefix's files |
| Status bar's buttons | 100% / 100% / 0 | 100% within 1 |

Still different in File Explorer:

- **Text that is not fixed**: a folder's name in the tab, the crumbs, "Search <folder>", file names and dates, the item
  count are GDI text in the shell's font.
- **Content**: Home's Recent lists the newest files of the user's folders (none in the probe prefix, so the
  prototype's own empty note shows); folders show the prefix's files.
- **Tabs** (`explorer-tabs-2026-10-16-02`, capture 8: Ctrl+T beside Documents): one browser per tab with its own
  location and history, + and Ctrl+T, close (button, middle click, Ctrl+W), Ctrl+Tab; the strip's equal widths
  (`pw_explorer_tab_strip`, host-tested at 1 to 9 tabs) and the prototype's unselected tab (separator, plate under the
  pointer). Title bar with two tabs 99.3% within a level of the prototype's (the tab region 96.6%: the names are GDI
  text; an unselected tab's name is drawn with real alpha on the glass).
- **Filter and the details pane** (`explorer-pane-2026-10-16-02`, captures 8 and 9): Filter's kinds (the prototype's
  FILTERS, by attributes and extension) through the browser's site (`ICommDlgBrowser::IncludeObject`), checked while on,
  ended when the tab goes elsewhere, with the prototype's note in an empty list; the 280 px details pane (Details,
  Alt+Shift+P) with the location or the selection's icon, name, type, Contains or Size, Date modified, Path, or "N items
  selected" and the files' total size. Against the prototype's captures: Filter's whole window 99.2% within a level,
  column header 99.98%; the pane on a folder: column header 100%, command bar 99.97%. Names and values are GDI text;
  the classic horizontal scroll bar shows when the columns are wider than the narrowed items area.
- **Navigations** (patch 1152): the new folder's view is held from drawing until it is styled, so Wine's classic list
  no longer shows for a frame between folders (the owner saw it on the console); explorer-repaint-2026-10-16-02 and
  explorer-dark-2026-10-16-02: every new view invisible when created. Up from the navigation pane's Documents goes to
  the Desktop (Wine's namespace), the prototype's to This PC. Dark on the current tree: Home 98.6% and Documents 98.7% of
  the window within a level of the prototype's, command bar 100% (Filter now enabled, as the prototype's).
- **Not wired**: the search box, Gallery opens Pictures, This PC's expand chevron.
- **Icon views**: large icons, small icons and list are the prototype's tiles (`explorer-views-2026-10-16-01`, Ctrl+Shift+2,
  4 and 5 on Documents): its grid (`pw_explorer_tiles`: 116 px tiles 118 apart, rows as tall as their tallest name;
  240 x 32 in 244 px columns; 32 px rows down columns of at least 220 px), its 96 and 16 px folder and document icons
  (every tile's icon equal to the prototype's pixels, worst 0; the details view's icons too). Names are GDI text; other
  icons (known folders, programs) are the system's, 48 px centred in large icons; no picture thumbnails.
- **The top corners**: the compositor's ring is 16-17 levels off Chromium's anti-aliasing at a few pixels.

Dark, on the console (`explorer-dark-2026-10-16-01`: desktop-session with `--theme dark`, the probe applies the managed
theme's Dark variant as the appearance app does and puts the prior theme back), against the prototype's dark captures:

| Region | Home: exact / within 1 / worst | Documents |
| --- | --- | --- |
| Whole window | 92.0% / 98.6% / 227 | 92.8% / 98.7% / 232 |
| Title bar's Mica Alt | 13.0% / 100% / 1 | 12.8% / 100% / 1 |
| Tab | 93.2% / 99.99% / 2 | 89.8% / 97.1% (its name is GDI text) |
| Caption buttons | 69.8% / 99.8% / 44 (the rounded corner) | 67.5% / 99.8% / 44 |
| Address row | 95.7% / 99.99% / 3 | 93.1% / 97.1% (the crumbs and "Search Documents") |
| Command bar | 98.9% / 100% / 1 | 98.7% / 99.7% (Filter is drawn disabled) |
| Navigation pane | 92.9% / 99.96% / 6 | 93.1% / 99.9% / 6 |
| Home above Recent / the column header | 98.5% / 100% / 2 | 98.7% / 100% / 1 |
| Status bar's buttons | 95.0% / 100% / 1 | 98.3% / 100% / 1 |

The verdict's own check (the dark sprites over their surfaces) passes both: 538,650 and 215,050 pixels within a level.
Nothing dark-specific is left in what was compared; the rest is the same text, content and Filter as in light.

## Shell palette (`appearance.json` light / dark)

| Role | Before | Prototype (source) | Max diff |
| --- | --- | --- | --- |
| pane (Explorer content) | #ffffff / #202020 | #f9f9f9 / #1b1b1b (sampled) | 6 / 5 |
| text | #1b1b1b / #f7f7f7 | #1a1a1a / #ffffff (TextFillColorPrimary over the pane) | 1 / 8 |
| secondary | #595959 / #c1c1c1 | #5f5f5f / #cbcbcb (TextFillColorSecondary) | 6 / 10 |
| line | #d6d9df / #45484c | #e4e4e4 / #3b3b3b (dividers, sampled) | 15 / 16 |
| hover | #e7ebf1 / #383c43 | #e6e9ef / #2e3236 (SubtleFillColorSecondary over Mica) | 2 / 13 |
| accent | #0067c0 / #60cdff | **#005fb8** / #60cdff (the user chose the prototype's, 2026-10-16) | 8 / 0 |
| selected | #e4edf7 / #323f4e | #eaeaea / #2c2c2c (Explorer navigation, sampled) | 13 / 34 |
| button | #ffffff / #353535 | #fbfbfb / #2d2d2d (ControlFillColorDefault) | 4 / 8 |
| accent_text | #ffffff / #002b42 | #ffffff / #000000 (TextOnAccentFillColorPrimary) | 0 / 66 |
| mica_fallback | #eef2f8 / #22262d | #eef2f8 / #212529 (Mica over Bloom, sampled) | 0 / 4 |

## Classic programs (system colours and msstyles parts)

The controls gallery says what Windows 11 draws in a classic program: GetSysColor's defaults and static aero
bitmaps, which ignore the accent and stay light in dark mode. They were derived from the shell palette by a role
guess; they now come from `appearance.json` `classic` (light from the gallery, dark from what the dark Explorer
themes draw where the gallery shows it).

| | Built before | Gallery | Now |
| --- | --- | --- | --- |
| COLOR_WINDOW, BTNFACE | #ffffff, #f7f9fc | #ffffff, #f0f0f0 | as the gallery |
| WINDOWTEXT, BTNTEXT, MENUTEXT | #1b1b1b | #000000 | #000000 |
| GRAYTEXT | #595959 | #6d6d6d | #6d6d6d |
| HIGHLIGHT, MENUHILIGHT | #0067c0 | #0078d7 | #0078d7 |
| HOTLIGHT | #e7ebf1 | #0066cc | #0066cc |
| WINDOWFRAME, BTNSHADOW, 3DDKSHADOW, 3DLIGHT | #d6d9df (all) | #646464, #a0a0a0, #696969, #e3e3e3 | as Windows' defaults |
| MENU, MENUBAR | #ffffff | popup #f9f9f9, bar #ffffff | as the gallery |
| Progress fill, paused, error | **#ffffff (all three: the role guess made bars invisible)** | #06b025, #dada00, #da2626 | as the gallery |
| Combo box highlighted item | white on white (fill guessed as pane) | #0078d7, white text | as the gallery |
| Hot popup menu item text | dark (guessed as text) | highlight text | white |
| Command link, task dialog instruction, balloon title | grey (guessed as text) | #003399 / #0066cc | as the gallery |
| Check box | dark mark on white, blue border when hot | blue #005fb8 fill, white mark; hot #196ebf, pressed #3381c6, disabled #c5c5c5 | measured sprites |
| Radio button | dark dot, blue only when hot | blue ring, white dot | measured sprites |
| Push button | rest #ffffff/#d1d1d1, hot #f9f9f9, pressed #f0f0f0 | rest #fdfdfd/#d0d0d0, hot #e0eef9/#0078d4, pressed #cce4f7/#005499, default 2 px #0078d4 | measured sprites, light and dark |
| Edit border | #aeaeae, hot #0067c0, focus #00549e | #7a7a7a, hot #171717, focus #0078d7, disabled #cccccc | as the gallery, light and dark |
| Group box | rounded 3 px #aeaeae | square 1 px #dcdcdc | as the gallery |
| List view selection (classic) | #0067c0 | COLOR_HIGHLIGHT #0078d7 | #0078d7 |

## Pointers and monospace face

- The prototype's pointer scheme (original artwork) is built into `.cur`/`.ani` by `tools/build-cursors.py`, staged
  in `C:\windows\Cursors`, named in `HKCU\Control Panel\Cursors` by the session root, and loaded by user32 for every
  system cursor (patches 1137, 1138). Host Wine (unpatched) shows the registry half passing and the four user32
  checks failing, as it should without the patch; `tests/cursor-scheme-probe.c` is the console check.
- Cascadia Mono (Regular, Bold; OFL-1.1, Microsoft's v2407.24 release, unmodified) is staged in `C:\windows\Fonts`
  and made the console's face and the substitute for Consolas by the session root. Host Wine: a program started
  afterwards selects the same font file for "Consolas" as for "Cascadia Mono" (`tests/monospace-probe.c`, 8/8).

## Still different (not in this change)

- **Menus.** Wine draws popup menus itself with system colours, square, so the gallery's themed menu (8 px radius,
  #cccccc border, hot item #91c9f7 with dark text, shadow) is not drawn; the compositor rounds menus at 4 px. Themed
  menus are phase 4.
- **Explorer-theme list and tree items.** comctl32's list view never draws `LVP_LISTITEM`, so the gallery's
  #cce8ff/#99d1ff selection needs a comctl32 change; classic selection is #0078d7.
- **Parts not yet re-derived from the gallery:** scroll bars (classic 17 px and the overlay style), tabs, trackbar,
  up-down, header, toolbar and rebar, status bar, tooltips (4 px radius, #767676 border), date picker and month
  calendar.
- **Materials and shadows.** Captions are an opaque Mica fallback, not Mica; the compositor's single shadow falloff
  is not the prototype's three-layer box shadow, so the 8 x 8 rounded corners are judged apart.
- **Dark classic controls.** Check boxes, radio buttons and the group box keep the dark transform of their previous
  sprites (the gallery draws them light, as Windows does).
- **The single-surface route.** A program started outside the desktop session (no `WINE_PS5_WINDOW_SURFACES`, as
  the appearance probe and games run) is shown as one flattened surface that no compositor dresses, so its window
  has no border, rounded corners or shadow: the row above its caption is Wine's frame in the bar colour. The desktop
  session's route draws them; the console measured the default border there as #c1c1c1 over the #f3f3f3 inactive bar
  (`(0xf3*153 + 0x75*102 + 127) / 255 = 0xc1`, window-effects capture 2 of `win11-visual-2026-10-16-01`).
- **Larger DPIs.** Only 96 DPI is measured; 125-400% images are scaled from it (phase 8).
- **Text.** Proton UI through FreeType, not Segoe UI Variable through DirectWrite: boxes and positions match, glyph
  pixels do not.
