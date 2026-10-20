# Windows 11 reference sources

The user has no Windows 11 machine to compare against (2026-10-12), so the reference is built from what is published. This register says
where every number comes from and how far to trust it. Nothing from Microsoft's artwork, fonts or icons is kept here: only numbers, with
their source. Files: [tier-a.json](tier-a.json), [winui-colors.json](winui-colors.json).

## Tiers

| Tier | What it is | Trust |
| --- | --- | --- |
| **A** | Microsoft's own published numbers (Microsoft Learn design pages) and **MIT-licensed Microsoft source** (WinUI, Windows Terminal) | Exact. A mismatch is our bug. |
| **B** | Documented Win32 behaviour and defaults (API docs, registry defaults), the user's own approved reference for the shell, and values read from open-source shell tools | Good; cite two sources where possible. |
| **C** | Measured from official Microsoft images or videos at 100% (support pages, launch material), kept as numbers with the image's URL and date | Approximate (about 1 px); marked as such in the scorecard. |

Rule: a number is Tier A or B before Tier C is used, and every entry names its source. A Tier C number can be upgraded when a real
Windows 11 is ever available; the scorecard shows the tier of each measure, so a pass on a Tier C measure is not mistaken for an official one.

## Read and recorded (2026-10-12)

| Topic | Source | Tier | Where recorded |
| --- | --- | --- | --- |
| Corner radii: windows and flyouts 8, controls 4, none when maximised or snapped | [Geometry in Windows 11](https://learn.microsoft.com/en-us/windows/apps/design/signature-experiences/geometry) | A | tier-a.json `geometry` |
| Title bar 32 (48 with a search box), icon 16, margins, Mica default, glyph code points | [Title bar design](https://learn.microsoft.com/en-us/windows/apps/design/basics/titlebar-design) | A | `title_bar` |
| Elevation values and 1 px strokes | [Layering and elevation](https://learn.microsoft.com/en-us/windows/apps/design/signature-experiences/layering) | A | `elevation` |
| Durations 250, 167, 83 ms and the two easing curves | [Timing and easing](https://learn.microsoft.com/en-us/windows/apps/design/motion/timing-and-easing) | A | `motion` |
| Type ramp (12/16, 14/20, 18/24, 20/28, 28/36, 40/52, 68/92), weights, minimums | [Typography in Windows](https://learn.microsoft.com/en-us/windows/apps/design/signature-experiences/typography) | A | `typography` |
| 83 colour resources per theme, light and dark (text, controls, strokes, layers, backgrounds) | [WinUI `Common_themeresources_any.xaml`](https://github.com/microsoft/microsoft-ui-xaml/blob/4c45fb8ea98afafc5cf1b0bafc7febbe20d67b19/controls/dev/CommonStyles/Common_themeresources_any.xaml), MIT, revision `4c45fb8e` | A | winui-colors.json |

## Known gaps, and where each will be filled

| Gap | Candidate source | Tier |
| --- | --- | --- |
| Per-control sizes and paddings | **Started**: 331 numeric resources over 40 controls are in [winui-controls.json](winui-controls.json) (button padding 11,5,11,6; menu flyout minimum height 32; scroll bar timings; and so on). Fixed heights and minimum widths live in the style templates beside them, still to extract | A |
| Acrylic and Mica recipes (tint opacity, luminosity, noise) | WinUI's material sources and the [Mica](https://learn.microsoft.com/en-us/windows/apps/design/style/mica) and acrylic design pages | A |
| Caption button width, glyph box, hover and pressed colours, transition times | **Recorded** from Windows Terminal's MIT source, which reproduces the system buttons (46 px wide, 10 px glyphs, close hover #C42B1C, 150 / 100 ms): tier-a.json `title_bar.caption_buttons_from_windows_terminal` | A |
| Taskbar height and icon size, Start panel size, Explorer sidebar width | The user's approved reference (taskbar 48 px, Start 600 x 566, sidebar 165 at 1920 x 1080 and 96 DPI, already built into the shell); Microsoft Support images | B / C |
| Desktop icon grid | **Recorded**: 75 x 75 px cells (registry -1125 twips) in [tier-b.json](tier-b.json); icon sizes (96, 48, 32) still to confirm | B |
| Shadow blur and offset behind each elevation value | WinUI's ThemeShadow source, then official images | A / C |
| Shell animations (window open and close, minimise, maximise, Start, flyouts) | Official videos, frame-stepped at 60 fps, durations only | C |
| System tray, clock, Quick Settings, calendar, notifications geometry | Microsoft Support images | C |

## Sources that were looked at and set aside

- Web "Windows 11 clone" repositories: one star, no licence on most, and unmeasured; not a reference.
- Third-party guides that quote 48 px or 640 x 720: they are not Microsoft's figures and disagree with each other.
- The UWP Figma toolkit on Microsoft's download page is for UWP controls (2024) and needs Figma to open; it can be consulted
  for control geometry but does not cover the shell.
