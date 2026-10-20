---
version: 1
slug: "prototypes-windows11-shell-index-html"
primary_target: "prototypes/windows11-shell/index.html"
related_targets: []
---

## Scope

Mode: Operate. A browser proof of concept of the whole Windows 11 desktop shell for Mihawk OS, opened locally on the user's PC so they can judge it before the native Wine shell copies it. Code-led (Operate surfaces build code-led).

Surfaces: desktop (icons and shortcuts, drag to rearrange with grid snap, rubber-band selection, context menu, rename), taskbar (centered, pinned and running apps, tray, clock), Start, Search, Task View and Alt+Tab, Quick Settings, notification centre with calendar, File Explorer windows, a Settings app whose Personalization page restyles the shell live (light/dark, accent, wallpaper), window chrome with drag, resize, snap to edges and corners with preview, Snap Layouts on Maximize hover, and open/close/minimize/maximize motion. Responsive from 1280×720 to 2560×1440, and down to small widths without breaking. Mouse and keyboard only.

Constraints: no Microsoft fonts, icons, wallpaper or binaries in the repository; Proton UI/Selawik font, the repository's approved icons plus MIT Fluent System Icons at the pinned revision; numbers from docs/windows11-reference and themes/windows11/appearance.json.

## Direction contract

THESIS: Windows 11 itself, played straight at full fidelity; the user pinned it. Refused: a "Windows-inspired" web dashboard, flattened stand-ins for Mica and Acrylic, generic web controls, motion that is not Windows' own.
OWN-WORLD: Mica windows (wallpaper-tinted, near-opaque), Acrylic flyouts and Start (blurred, noise-tinted), 8 px top-level and 4 px control radii, 1 px hairline strokes, layered soft shadows by elevation (window 128, flyout 32, tooltip 16), WinUI colour resources light and dark, accent #0067C0 light / #60CDFF dark, Proton UI type ramp 12/16 to 28/36, Fluent 20 px glyphs, the project's own Aero Meadow wallpaper (Frutiger Aero sky, soap bubbles and green hills; a night variant).
STORY: The user lands on their desktop and recognises Windows 11 at once; they drag shortcuts, open Start, search, open Explorer and Settings, snap windows, and find every hover, shadow and animation where Windows puts it.
FIRST VIEWPORT: Full-screen Aero Meadow wallpaper; desktop icons in a 75 px column down the left (This PC, Recycle Bin, the Example Game and Demo Game shortcuts, Game Launcher and Notepad shortcuts with arrow badges, a "Read me" text file); a 48 px Mica taskbar along the bottom with centred Start, Search, Task View, pinned Explorer, Settings and apps, running-app pills, and the tray with Wi-Fi/volume/battery and a two-line clock at right. No window open at first load.
FORM: Windows 11 canon, pinned by the user's brief ("a very faithful replica of Windows 11"), so concept-seed was not run (new-work: a user- or brief-pinned direction beats the roll); position 1; seed: none (brief-pinned, no roll). Signature move: Windows 11's own motion grammar (250/167/83 ms, cubic-bezier(0,0,0,1) in, (1,0,1,1) out) carried through every surface.
FINISH: unreviewed and undocumented is unfinished; this build ends with the finish review, the verdict, DESIGN.md, and every shipping raster carrying its provenance
