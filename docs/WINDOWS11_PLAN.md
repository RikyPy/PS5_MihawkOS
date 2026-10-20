# Windows 11 fidelity plan (proposal, 2026-10-12)

Status: **proposal for the user's review**. Only the reference data set (phase 0) is started; nothing else is built. It resumes the Mihawk OS work that was set aside for the
performance and stability fixes ([MIHAWK_OS.md](MIHAWK_OS.md) holds what exists and what it has proved).

## 1. The goal, made measurable

The goal is a desktop that is **as faithful to Windows 11 as we can make it**: the same layout, spacing, proportions, colours, corner
radii, shadows, states and motion, and the same behaviour (what a click, a drag, a right click or a key does). It is *not* the same
glyphs: the user has settled that Microsoft's exact fonts and icons are not needed. Our Proton UI font (a Selawik derivative) and our own
icon set stay, and they are tuned to match Segoe UI Variable's size, weight and spacing as closely as an open font allows.

"As faithful as possible" is only useful if it can be checked, so the plan starts by turning it into numbers (phase 0) and every later
phase ends with a score against them, not with "looks similar".

**What cannot be matched, and the plan does not pretend to:** glyph shapes (fonts, icons), the exact anti-aliasing of text (Wine renders
with FreeType, Windows with DirectWrite), and Microsoft's own artwork. Everything else is in scope.

## 2. How fidelity is measured, with no Windows 11 to compare against

The user has no Windows 11 machine, so the reference is built from what is published. It is already started:
[docs/windows11-reference/](windows11-reference/SOURCES.md) records each number with its source and a trust tier.

| Tier | Source | Trust |
| --- | --- | --- |
| **A** | Microsoft Learn design pages (published numbers) and **MIT-licensed Microsoft source** (WinUI, Windows Terminal) | Exact: a mismatch is our bug |
| **B** | Documented Win32 behaviour and defaults, the user's already-approved shell reference (taskbar 48 px, Start 600 x 566, Explorer sidebar 165 at 1920 x 1080 and 96 DPI), open-source shell tools read for constants | Good |
| **C** | Measured from **official Microsoft images or videos** at 100%, kept as numbers with the URL and date | About 1 px; marked as approximate |

**Already collected (Tier A, 2026-10-12):** corner radii (windows and flyouts 8 px, controls and bars 4 px, none when maximised or snapped), the
title bar (32 px, 48 px with a search box, a 16 px icon, 16 px margins, Mica by default, the caption-glyph code points), the elevation values
(window and dialog 128, flyout 32, tooltip 16, card 8, control 2, layer 1) with 1 px strokes, the animation durations (250, 167 and 83 ms)
and the two easing curves (`0,0,0,1` in and `1,0,1,1` out), the whole type ramp (12/16 up to 68/92), and **83 colour resources per theme,
light and dark**, read from the WinUI source at a pinned revision. Not published, and so still to find: the caption-button width
(46 x 32 is quoted widely, Tier C until measured), the shadow blur behind each elevation, and every shell surface's geometry and motion.

- **Comparison by structure, not by whole-image difference.** The glyphs differ, so a raw pixel diff would always fail on text. The harness
  compares layout rectangles (tolerance 1 px), sampled colours (tolerance 2 per channel, as the existing appearance checks use), corner
  radii and border widths, state colours (hover, pressed, selected, inactive), and for motion the start, end, duration and curve of each
  transition. Text is compared by its box (position, size, baseline), not its pixels.
- **A scorecard per surface**, printed by each gate case: for example `taskbar: 41/43 measures within tolerance (38 Tier A or B, 5 Tier C)`.
  A surface is "done" at a threshold the user sets (proposal: 95% of measures, no layout miss above 2 px).
- **Honest ceiling.** Where only Tier C exists (most shell surfaces, shell animations), the score can say "matches the published images", not
  "matches Windows". If a real Windows 11 is ever available, one verification pass upgrades every Tier C entry.
- It builds on what exists: the pixel oracle tests, the display verdicts in `tools/run-appearance.py` and `tools/run-window-compose.py`,
  and `themes/windows11/appearance.json`.

**Where the reference is strong and where it is weak:**

| Phase | Reference |
| --- | --- |
| 2 Window chrome | Strong (A): radii, title bar, elevation; weak: caption-button width, shadow blur (C) |
| 4 Menus, dialogs, controls | **Strongest**: the WinUI source holds every control's sizes, paddings, states and colours (A) |
| 7 Motion | Controls: A. Shell animations: C (official videos, durations only) |
| 1 Desktop | Behaviour is Windows' documented shell behaviour; icon grid from the Win32 docs and defaults (B) |
| 3 Taskbar, 5 Flyouts, 6 Explorer | Mostly B and C: the user's approved reference plus official images |

## 3. The phases

Sizes are relative effort, not dates: **S** a few console runs, **M** a week of focused work, **L** several weeks. Each phase ends
with new gate cases and an entry in `docs/PLAN.md`.

### Phase 0: reference and harness (S)
Started. Done: the Tier A set and its source register (`docs/windows11-reference/`). Still to do: the per-control metrics from the WinUI source,
the acrylic and Mica recipes, the desktop icon grid defaults from the Win32 docs, the Tier C measurements from official images and videos
(each recorded with its URL), and the scorecard in the runners. **Blocks any claim of fidelity; the first six phases can start in parallel
with it, because they are checked as they land.**

### Phase 1: a real desktop (M): the click-and-drag you asked about
Today the desktop is Wine's `programs/explorer/desktop.c`: an array of launchers it paints itself (patch 1000 added hover, selection, a
double click and a context menu). Windows' desktop is a shell **folder view** on the Desktop folder. Replace the hand-drawn launchers with
a transparent icon view hosted in Progman (the shell already hosts folder views in Explorer windows through `IExplorerBrowser`, patch 0983).
That gives, from the shell's own code rather than ours:
- rubber-band selection, Ctrl and Shift multi-select, keyboard navigation, type-ahead, Select all;
- dragging icons (grid snap, "align icons to grid", "auto arrange", sort by name, size, type, date), positions that persist;
- inline rename (F2 or slow double click), Delete, Cut, Copy, Paste, New (folder, shortcut, text file);
- drag and drop between the desktop, Explorer windows and Start pins, with the copy/move/link cursors;
- icon sizes (small, medium, large) and "show desktop icons";
- the system icons (This PC, Recycle Bin, Network) as the shell's own virtual items;
- the wallpaper showing through (transparent background, label shadow as Windows draws it);
- the game shortcuts we already write to `C:\users\Public\Desktop` appear as ordinary items, unchanged.

**Input for a console with no mouse by default:** the DualSense touchpad already sends a held press with motion as a drag
(`src/proton_touchpad.h`); phase 1 checks every interaction above through it and through a USB mouse, and adds the controller path
(D-pad to move the focus, a button to open, another for the context menu). A decision is asked in section 5.

**Risks:** Wine's `shell32` folder view is simpler than Windows'; any behaviour it lacks (for example some drag cursors, or the view's
background transparency) becomes a patch to `shell32`/`comctl32`, which is the fix every program benefits from. Gate cases: rubber-band
count, drag-to-new-position and persistence, rename, drag a file to an Explorer window (copy), sort and auto-arrange, plus the scorecard.

### Phase 2: window chrome (M)
Title bar height and caption-button sizes (close turns red on hover), the glyphs drawn from our icon set at Windows' sizes, 1 px border and
8 px corner radius (square when maximised), active and inactive colours, shadows and Mica title bars (the effects exist and are
host-judged; they get measured), correct resize and drag hit areas and cursors, the system menu, **snap**: drag to the edges and corners
with the preview overlay, snap layouts on the maximise button and Win+Z.
Gate: scorecard for frame, caption and snap overlays in light and dark.

### Phase 3: taskbar and tray (M-L)
Button states (hover, pressed, active, attention), the running-indicator pill, drag to reorder, pin and unpin, jump lists and the taskbar
context menu, the notification area with its hidden-icons flyout, clock and date, the show-desktop edge, badges and progress, auto-hide,
the Search, Task View and Widgets buttons wired to what exists (or hidden where there is nothing behind them).
Taskbar thumbnails already exist (0994) and get measured.

### Phase 4: menus, dialogs, controls (L)
Windows 11's rounded context menu (icon row, then the list, with "Show more options" opening the classic menu), menu bars and popups,
thin overlay scrollbars, toggles, sliders, check boxes, combo boxes and text boxes in all states and in dark mode, common dialogs (open and
save, message boxes, task dialogs) and focus rectangles. This is mostly the msstyles resource set and a few controls Wine draws
itself; the resource set already has 964 resources and a pixel oracle.

### Phase 5: flyouts (L)
Quick Settings (volume and sound output, network state, power), the calendar and notification centre, toasts, the Search panel, and the
Start details still to measure (pinned and recommended layout, All apps, the user and power buttons). Windows draws these in XAML, which
Wine does not have, so each is a native Win32 surface built to the measured geometry.

### Phase 6: Explorer features (M-L)
Tabs, the command bar, the details and preview panes, Quick access, a breadcrumb address bar, the search box, the view modes, inline
rename, the properties dialog.

### Phase 7: motion (M)
A small animation layer in the compositor using the measured durations and curves for: window open, close, minimise, maximise and restore;
Start and flyout slide and fade; menu fade; hover transitions. Frame pacing is already measured, so the budget (a frame every 16.7 ms
without dropping) is known.

### Phase 8: scaling (M-L)
100, 125 and 150 percent: per-monitor DPI, scaled shell metrics, icons at the right sizes, text scaling. Today everything is 96 DPI.

### Phase 9: Task View, virtual desktops, Alt+Tab, Snap Assist, Settings (L)
The system-level surfaces. Several are large (a Settings app is a product of its own); the proposal is to do Task View, the new Alt+Tab
and Snap Assist, and decide Settings later.

## 4. Order, and why

0, then **1 first** (your complaint, and it is the smallest step that changes how the system feels), then 2 and 3 (what is on screen
all the time), then 4, 5, 6, 7, 8, 9. Phase 7 (motion) can move earlier if animation matters more to you than the flyouts.

Cross-cutting rules, as for the rest of the project: every phase is proved on the console and recorded in `evidence/`; a code commit
needs the gate; Microsoft assets never enter the repository; the desktop stays out of the way of games (game mode unloads it, and nothing
here may slow a game); and the performance budget is a measured number per surface (the compositor's cost is already recorded).

## 5. Decisions asked of the user

1. **Scope:** the whole OS, or the surfaces a PS5 user touches (desktop, windows, taskbar, Start, Explorer, menus, dialogs, quick
   flyouts)? The proposal is the second, with phase 9 optional.
2. **Order:** is "desktop first" right, or should motion or the taskbar come earlier?
3. **Touchpad drag:** keep a held touchpad click as the drag (as it is now), or add a dedicated button for "hold the left mouse button"?
4. **Controller on the desktop:** should D-pad and buttons drive the desktop, or is it pointer-only?
5. **DPI:** is 100% enough for now, or is 125/150% needed early (a TV at 4K output would want it)?
6. **Threshold:** is 95% of measures within tolerance the right bar for "done" on a surface?
7. **Tier C:** is measuring official Microsoft images and videos acceptable as the reference where nothing is published (the number is kept, the image is not)?

## 5b. Decisions taken (2026-10-12, branch `windows11`)

The user asked for the whole plan to be implemented while they are away, without waiting for them. So each question in section 5 was answered
with the default below; any of them can be reversed later.

1. **Scope:** the surfaces a PS5 user touches: phases 0 to 8. Phase 9 (Task View, Alt+Tab, Snap Assist, Settings) only after the rest.
2. **Order:** as written (desktop first).
3. **Touchpad drag:** a held touchpad click stays the drag (it already sends press, motion, release); nothing new is added until it proves insufficient.
4. **Controller on the desktop:** yes, D-pad moves the focus, Cross opens, Square opens the context menu, once the folder view works.
5. **DPI:** 100% first; 125 and 150% are phase 8.
6. **Threshold:** 95% of measures, no layout miss above 2 px.
7. **Tier C (official images and videos):** acceptable, recorded as numbers with the URL.

## 5b'. Visual system from the approved prototype (2026-10-16)

The browser prototype the user approved (PS5_Proton-design `prototypes/windows11-shell`) is now the reference the
native shell is measured against, above the published numbers: its rendered pixels are measured and the native
theme is fitted to them. Done first, because it applies to every surface: tokens, the classic theme parts, the
window frame and caption buttons (phase 2's chrome, pixel-exact, patch 1139), the pointer scheme and the monospace
face. What changed and what still differs: [windows11-reference/PROTOTYPE_DIFF.md](windows11-reference/PROTOTYPE_DIFF.md).

## 5c. Performance and memory budgets (apply to every phase)

The user's standing requirement: the best performance and the lowest RAM use we can reach. Each phase therefore records, in its evidence,
before and after numbers for: the shell helper's flexible resident memory (the 1 GiB pool, `vm resident` in the helper's census), its direct
memory, the compositor's cost per frame (`gpu-windows compose_avg_us`), and frames missed. Rules: no polling where an event exists; no per-frame
allocation; icons and bitmaps are created on demand and shared (one system image list, not one per view); caches are bounded and released when
a view is hidden; nothing runs while a full-screen game holds the display (game mode unloads the shell). A phase that raises the shell's
flexible memory by more than 8 MiB, or the compositor's average frame cost by more than 10%, needs a reason written next to its numbers.

## 6. What this plan needs from the user

Nothing to capture. Only the decisions in section 5. If a Windows 11 machine ever becomes available (a friend's, a virtual machine),
one short session with the scorecard open would upgrade the Tier C entries; it is not required.

## 7. What this plan does not promise

Microsoft's glyphs and artwork, byte-identical text, or every behaviour of every Windows 11 surface. It promises a measured, per-surface
fidelity score that rises phase by phase, and a desktop that behaves like Windows' where the user touches it.
