# Module contract: Windows 11 shell POC

Every surface is one module that owns its own files and talks to the others only through the
`Shell` object. A module defines its public API **synchronously when its script loads** (so
other modules can call it at any time after boot) and mounts its DOM in `Shell.on('boot', ...)`.
Scripts load in the order of `index.html`; `Shell.boot()` runs last.

Fidelity rule for every module: do what Windows 11 does. Sizes, colours, radii, shadows, states
and motion come from `css/tokens.css` (WinUI resource names kept verbatim, e.g.
`var(--SubtleFillColorSecondary)`), never ad-hoc hex values, so light/dark and the user's accent
restyle everything live. Use the materials in `css/base.css` (`.mica`, `.mica-alt`,
`.acrylic`, `.acrylic-base`, `.surface-flyout`) instead of re-deriving them. Motion uses
`Shell.animate` or CSS with `var(--dur-normal|fast|faster)` and `var(--ease-in|ease-out)`.
Respect `Shell.reducedMotion()`.

Responsiveness: every surface works from 360×640 up to 2560×1440 (the target is 1920×1080),
recomputes on `resize`, keeps itself inside the viewport above the taskbar, and never causes
page scroll.

## Foundation (written, do not edit): `js/core.js`, `css/tokens.css`, `css/base.css`, `index.html`

| API | Meaning |
| --- | --- |
| `Shell.on(name, fn)` → unsubscribe; `Shell.emit(name, detail)` | Event bus. |
| `Shell.store.get(key, fallback)`, `.set(key, value)` | localStorage, namespaced, safe in private windows. |
| `Shell.settings` (object), `Shell.set(key, value)` | Persisted settings; emits `setting` `{key, value}`. Keys: `theme` (`light`/`dark`), `accent` (hex), `wallpaper` (id in `Shell.wallpapers`), `transparency`, `animations`, `showDesktopIcons`, `iconSize` (`small`/`medium`/`large`), `autoArrange`, `alignToGrid`, `taskbarAlignment` (`center`/`left`), `volume` (0–100), `brightness` (0–100), `wifi`, `bluetooth`, `airplane`, `nightLight`, `energySaver`, `doNotDisturb`. A module may add keys it owns (document them below). |
| `Shell.wallpapers` | `[{id, name, image?, css, mica?, solid?}]`; an entry without `image` is drawn from `css` (and Mica from `mica`, else `css`). The core sets `--wallpaper` and `--mica-image` and emits `wallpaper`. `meadow-light` and `meadow-dark` are Aero Meadow and its night variant, which `serve.sh` draws into `local/`; until a picture loads its `css` gradient stands in. |
| `Shell.animate(el, keyframes, opts)` → Promise | WAAPI with Windows' curves; duration and delays 0 when motion is reduced (CSS transitions and animations too). `Shell.ease.in/out/standard`. |
| `Shell.icon(name, {size, className})` → `<span class="icon">` | `fluent/<name>` = 20 px line glyph in `currentColor`; `object/<name>` = colour object icon. Names in the table below. |
| `Shell.iconUrl(name)` | URL of an icon file. |
| `Shell.apps.register({id, name, icon, pinned?, startPinned?, description?, open(args) → win})`, `.get(id)`, `.list()`, `.launch(id, args)` | App registry; `launch` emits `app-launch`. |
| `Shell.fs` | Virtual file system: `get(id)`, `children(parent)`, `path(id)`, `search(q)`, `uniqueName(parent, base, ext)`, `create(parent, props)`, `rename(id, name)`, `move(id, parent)`, `copy(id, parent)` → new item (folders with their contents; a taken name gets " - Copy"), `remove(id)` (to Recycle Bin), `restore(id)`, `emptyRecycleBin()`, `open(id)`, `formatSize(bytes)`, `reset()`. Emits `fs` `{type, item}`. Folder ids: `root` (This PC), `home`, `recycle`, `desktop`, `documents`, `downloads`, `pictures`, `music`, `videos`, `games`, `c`, `d`. Items: `{id, name, kind: folder/file/shortcut/drive/system, parent, icon, ext?, size?, free?, modified, target?, args?, content?}`. |
| `Shell.keys.on(combo, fn)` | Global shortcut. `combo` like `Win+E`, `Alt+F4`, `Ctrl+Shift+Escape`, `Win+ArrowLeft`. Every `Win+X` also answers `Ctrl+Alt+X`, and `Ctrl+Win+X` / `Alt+Win+X` / `Shift+Win+X` answer `Ctrl+Alt+Shift+X` (`Shell.keys.fallbackOf(combo)`); an explicit registration beats a derived fallback. Return `false` from `fn` to let the event through. The bare Win key emits `win-key`. |
| `Shell.layer(name)` | Layer container: `wallpaper`, `desktop`, `windows`, `taskbar`, `flyouts`, `overlays`, `menus`, `tooltips`. |
| `Shell.transient.open(id, {close, elements, group})`, `.close(id)`, `.closeAll(group?)`, `.isOpen(id)` | One open transient per `group` (`flyout` for Start/Search/Quick Settings/Notifications/Task View, `menu` for context menus). A pointerdown outside every element in `elements` (elements or `(event) => bool` predicates) light-dismisses it. Include your own trigger button in `elements` so clicking it toggles rather than closes-then-reopens. |
| `Shell.clipboard.set(ids, {cut})`, `.get()` → `{ids, cut}` or null, `.clear()`, `.canPaste(parent)`, `.paste(parent)` → items | One file clipboard for the desktop and Explorer. Cut, Copy, Paste and Ctrl+X/C/V go through it: cut items move on paste and the clipboard empties, copied items are duplicated (`Shell.fs.copy`) and stay on it; Esc after Cut calls `clear()`. Paste is enabled where `canPaste(folderId)` is true (folders and drives, not the Recycle Bin or This PC). Emits `clipboard` `{ids, cut}`: every surface dims the icons of cut items while `cut` is true. |
| `Shell.drag.registerTarget(el, {accepts(ids), onEnter?, onLeave?, onDrop(ids, {x, y, copy})})`, `.hover(x, y, ids, exclude)`, `.drop(x, y, ids, {exclude, copy})` | Cross-surface drops of file-system item ids (desktop ↔ Explorer ↔ Recycle Bin). |
| `Shell.now()`, event `tick` every second | Clock. |
| `Shell.resetAll()` | Clears all stored state and reloads. |
| `Shell.debugOpen` | Names from `?open=a,b` in the URL. After boot, a module whose name is listed opens its surface (`start`, `search`, `quicksettings`, `notifications`, `taskview`, `lock`, `explorer`, `settings`, `notepad`, `calculator`, `terminal`, `snaplayouts`, `menu-desktop`), so captures need no clicks. `?dark` / `?light` force the theme for that page load without saving it, and `?fresh` starts from default settings and the seed file system (nothing is saved), so every capture starts from the same state. |

Calls into another module must tolerate it being absent (`Shell.taskbar?.buttonRect?.(id)`), so each module runs on its own while the others are being built.

## Module APIs (each owner implements exactly these; others may call them)

### ui: `js/ui.js`, `css/ui.css` (primitives shared by every surface)
- `Shell.menu.show({x, y, items, anchorRect?, placement?: 'bottom-start'|'top-start'|'top'|..., minWidth?, onClose?})` → `{close()}`. Windows 11 context menu: acrylic, 8 px radius, 4 px inner padding, 32 px items (4 px radius hover plate), 16 px icons in a left column, right-aligned shortcut text, submenus on hover (with chevron), check and radio marks, separators, disabled items, full keyboard (arrows, Enter, Esc, Left/Right for submenus, access by first letter), opening motion (fade + 8 px slide from the anchor side, 167 ms ease-in), stays inside the viewport. Item shape: `{label, icon?, shortcut?, disabled?, checked?, radio?, submenu?: items, action?(), separator?: true}`, or `{header: 'Pinned'}` for a non-interactive group heading (jump lists). `Shell.menu.close()` and `.isOpen()` close or query the open menu. Windows 11's command row: an item `{commandBar: [{icon, label, action, disabled?}]}` renders as the row of icon buttons at the top (Cut, Copy, Rename, Share, Delete). Group `menu` in `Shell.transient`.
- Tooltips: any element with `data-tooltip="text"` gets a Windows tooltip (500 ms hover delay, 4 px radius, acrylic, 12 px Caption text, above the pointer for elements in the taskbar, below otherwise; hides on pointerdown). `Shell.tooltip.show(el, text, {placement})`, `Shell.tooltip.hide()`.
- `Shell.ui.position(el, anchorRect, placement, {gap = 4, margin = 8})`: places a fixed element next to a rect, flipping and clamping to stay on screen.
- Controls as CSS classes (WinUI metrics and states: rest, hover, pressed, disabled, focus-visible), with tiny behaviours in `Shell.ui`:
  - `.btn` (standard), `.btn.accent`, `.btn.subtle`, `.btn.icon-only`; `.hyperlink`.
  - `.toggle`: `<label class="toggle"><input type="checkbox" role="switch"><span class="toggle-track"><span class="toggle-thumb"></span></span><span class="toggle-text"></span></label>` (thumb grows on hover, stretches when pressed, slides 167 ms).
  - `.slider`: `<input type="range" class="slider">` with accent fill (set `--fill` from value; `Shell.ui.slider(input)` wires it) and thumb that grows on hover.
  - `.checkbox`, `.radio` (native inputs restyled), `.textbox` (`<input class="textbox">`, accent bottom border on focus, clear button optional via `Shell.ui.textbox(input, {clear: true})`), `.searchbox` (textbox with search glyph).
  - `Shell.ui.combo(el, {options: [{value, label}], value, onChange})`: WinUI ComboBox, its list opens over the control with the selected item aligned.
  - `.card` (settings card: 4 px radius, CardBackgroundFillColorDefault, CardStrokeColorDefault), `.card-group`, `.expander` (`Shell.ui.expander(el)` animates open/close).
  - `.nav-item` (NavigationView item with the 3×16 px accent selection pill that slides between items: `Shell.ui.navList(container, {onSelect})`).
  - `.progress-ring`, `.progress-bar` (indeterminate as Windows draws them).
  - `.segmented` (Windows 11 segmented/SelectorBar), `.badge`.

### wm: `js/wm.js`, `css/window.css`
- `Shell.wm.open({appId, title, icon, width, height, minWidth?, minHeight?, x?, y?, content: HTMLElement, titlebar?: 'standard'|'tall'|'none', titlebarContent?: HTMLElement, resizable = true, maximizable = true, minimizable = true, background?: 'mica'|'mica-alt'|'solid'|'none', onClose?() → false to veto})` → window handle.
- Handle: `{id, appId, el, body, title, icon, desktop, setTitle(t), setIcon(name), focus(), close(), minimize(), restore(), maximize(), toggleMaximize(), isMaximized(), isMinimized(), isFocused(), snapTo(zone), snapshot(scale) → HTMLElement}`. `snapshot` returns a static, scaled visual copy for previews (taskbar thumbnails, Task View, Alt+Tab).
- `Shell.wm.list()` (bottom-to-top z-order, current virtual desktop), `Shell.wm.all()` (every desktop), `Shell.wm.focused()`, `Shell.wm.byApp(appId)`, `Shell.wm.showDesktop()` (toggle), `Shell.wm.currentDesktop()`, `Shell.wm.setDesktop(index)`, `Shell.wm.desktops()` → `[{index, name}]`, `Shell.wm.addDesktop()`, `Shell.wm.removeDesktop(index)`, `Shell.wm.moveToDesktop(win, index)`, `Shell.wm.workArea()` → rect above the taskbar.
- Emits `window` `{type: 'open'|'close'|'focus'|'blur'|'minimize'|'restore'|'maximize'|'title'|'icon'|'desktop', win}`.
- Owns: title bar (32 px; icon 16 px at 16 px margin; Caption title; tall 48 px variant), caption buttons 46×32 (close hover `#C42B1C`; min/max/restore hover SubtleFillColorSecondary; pressed tertiary; inactive window dims title bar text and glyphs), window corner radius 8 (square when maximized or snapped), 1 px border, elevation shadow (active vs inactive), Mica background that shows the wallpaper, drag (empty title bar space), double-click maximize, title bar right-click system menu (via `Shell.menu`), resize from all edges and corners with the right cursors and min size, keep a title bar reachable, focus/z-order on pointerdown, active/inactive states. Snap: drag to left/right edge (halves), corners (quarters), top (maximize), with the Windows 11 translucent snap preview that grows from the pointer; dragging a maximized window down restores it under the pointer; Snap Layouts flyout when hovering Maximize for ~500 ms and on `Win+Z` (layouts for the screen's aspect, zones highlight on hover, click snaps); Snap Assist after snapping (other windows offered as thumbnails for the remaining zone); `Win+ArrowLeft/Right/Up/Down`; `Win+D` show desktop; `Win+M` minimize all; `Alt+F4` closes the focused window (when none is focused, emits `alt-f4-desktop`). Motion: open (scale .9→1 + fade, 250 ms ease-in), close (scale →.95 + fade, 167 ms ease-out), minimize toward and restore from `Shell.taskbar.buttonRect(appId)`, maximize/restore animate the geometry, snap animates to the zone.

### desktop: `js/desktop.js`, `css/desktop.css`
- Desktop items are `Shell.fs.children('desktop')` plus system icons This PC (`root`) and Recycle Bin (`recycle`, full/empty icon). Large/medium/small icon sizes (96/48/32 px; Tier B), 75 px grid (scaled for size), labels with Windows' text shadow on the wallpaper, up to two lines (full text when selected), shortcut arrow badge on shortcuts.
- Hover plate, selected plate, selected+hover, focus rectangle, exactly as Windows draws them (translucent white plates with a 1 px lighter border).
- Click select, Ctrl/Shift multi-select, rubber-band selection (Windows' accent-tinted rectangle) from empty space, drag icons to any grid cell (snaps when "Align icons to grid" is on; free placement when off; swap/nudge on collision as Windows does), positions persist (`Shell.store` key `desktop-positions`), Auto arrange reflows. Drag to Recycle Bin deletes; drop on a folder icon or onto an Explorer window moves (via `Shell.drag`). Double-click/Enter opens (`Shell.fs.open`). F2 / slow second click renames inline. Delete key. Ctrl+A. Arrow keys move focus. Type-ahead.
- Context menu on empty space (Windows 11 style): View (Large/Medium/Small icons, Auto arrange, Align to grid, Show desktop icons), Sort by (Name/Size/Item type/Date modified), Refresh, New (Folder, Shortcut, Text Document), Display settings, Personalize, and "Show more options". On an item: command row (Cut, Copy, Rename, Share, Delete) + Open, Open file location (shortcuts), Pin to Start, Properties. Recycle Bin: Empty Recycle Bin.
- Columns reflow when the viewport height changes; icons never sit under the taskbar.
- API: `Shell.desktop.refresh()`, `Shell.desktop.select(ids)`.

### taskbar: `js/taskbar.js`, `css/taskbar.css`
- 48 px acrylic taskbar (`--acrylic-taskbar`) with a 1 px top stroke; items centered (or left per `taskbarAlignment`): Start button (the project's Mihawk mark, `themes/windows11/start.svg`, 22 px; it animates its glyph on press), Search (setting `taskbarSearch`: `box` default = Windows 11 search pill with glyph and "Search" text, `icon`, `hidden`), Task View, then pinned apps (`Shell.apps.list().filter(a => a.pinned)`, order persisted under `taskbar-order`) and running apps not pinned. Each app button 40×40 in a 48 px slot with the 4 px-radius hover plate; running indicator: 6×3 px grey pill, 16×3 px accent pill when focused, animated width change; multiple windows of one app show the same button (count dot not needed). Pressed state shrinks the glyph (scale .9). Tooltip with the app name.
- Click: launches, focuses, or minimizes as Windows does. Hover over a running app (~400 ms): window thumbnail flyout(s) above the button with title, icon, close button on hover, click to focus (use `win.snapshot()`). Right-click on an app: jump list menu (app name, Pin/Unpin from taskbar, Close window / Close all windows). Drag app buttons to reorder.
- Tray (right): chevron (hidden icons flyout), language "ENG" stack?, then the network/volume/battery group as one button (opens Quick Settings), then the clock button (two lines: time, date; tooltip with full date; opens notification centre; shows a notification count badge or the DND bell), then the 8 px "Show desktop" sliver at the far right edge (hover peek optional, click `Shell.wm.showDesktop()`).
- Empty-space right-click: "Taskbar settings" menu → Settings. `Shell.taskbar.buttonRect(appId)` → DOMRect (Start button rect when the app has no button). `Shell.taskbar.flash(appId)`. Settings keys owned: `taskbarSearch`.
- Below ~700 px wide, the tray collapses to what fits and the app list overflows into a chevron flyout as Windows does.

### start: `js/start.js`, `js/search.js`, `css/start.css`
- `Shell.start.open()`, `.close()`, `.toggle()`, `.isOpen()`; bare Win key and `Ctrl+Escape` toggle. Windows 11 Start (600×566 at 1080p, clamped to the viewport): acrylic, 8 px radius; search box at the top (focus moves typing to the search results inside Start); "Pinned" header with "All apps >" button; 6-column grid of pinned apps (icon 32 px, label) with pagination dots when more than 18; "Recommended" header with "More >" and recent files from `Shell.fs` (two columns, icon + name + relative time); bottom bar (mica-alt tinted) with the user (avatar + name "Mihawk") and the power button (menu: Lock, Sleep, Shut down, Restart → `Shell.session`). All apps view slides in (alphabetical list with letter headers; clicking a letter shows the letter grid). Right-click a pin: Pin to top / Unpin / Pin to taskbar / Open file location. Pins persist under `start-pins`. Opening motion: rises 40 px + fades in (250 ms ease-in), closing reverses with ease-out.
- `Shell.search.open({query?})`, `.close()`, `.toggle()`: Windows 11 Search flyout (same footprint as Start): search box, filter tabs (All, Apps, Documents, Settings, Folders), top-apps row and "Search highlights"-free quick searches when empty, result list with a best-match card on the right showing the item's actions (Open, Open file location, Pin to Start…). Sources: `Shell.apps.list()`, `Shell.fs.search(q)`, `Shell.settingsIndex` (provided by the settings module). Keyboard: arrows move the selection, Enter opens.

### flyouts: `js/quicksettings.js`, `js/notifications.js`, `css/flyouts.css`
- `Shell.quickSettings.toggle()/open()/close()` and `Win+A`: Windows 11 Quick Settings (acrylic, 360 px wide, anchored bottom-right above the taskbar with 12 px gaps): tile grid of split buttons (Wi-Fi with chevron to a network list subpage, Bluetooth, Airplane mode, Energy saver, Night light, Accessibility, Cast, Mobile hotspot), brightness and volume sliders, bottom bar with battery %, edit pencil, settings gear. Tiles toggle `Shell.set(...)`. Subpage slides in.
- Owns the screen filter: brightness dims and night light warms the whole screen through an overlay in `#overlays`, driven by `setting` events.
- `Shell.notifications.toggle()/open()/close()` and `Win+N`: notification centre (stacked cards grouped by app, "Clear all", per-notification dismiss with slide-out) above a calendar flyout (month grid, today in accent circle, header with month/year and up/down arrows, focus session strip "Focus" with minus/plus and start button). Both anchored bottom-right.
- `Shell.notify({app, title, body, icon?, actions?: [{label, action}]})`: toast sliding in at bottom-right above the taskbar (shown 5 s, hover pauses), then kept in the centre; suppressed (banner-less) when `doNotDisturb`. Emits `notifications` `{count}` for the clock badge.

### taskview: `js/taskview.js`, `css/taskview.css`
- `Shell.taskView.toggle()/open()/close()` and `Win+Tab`: full-screen Task View over a dimmed, blurred desktop: window thumbnails laid out in rows (title + icon above each, close button on hover, click focuses), virtual desktops strip at the bottom (thumbnails of each desktop, "New desktop" tile, rename, close, drag a window onto a desktop to move it). Motion: windows fly from their positions to the grid.
- Alt+Tab switcher (`Alt+Tab`, and `Alt+\`` because desktop OSes often keep Alt+Tab): appears while Alt is held, Tab/Shift+Tab cycle, release focuses; thumbnails on an acrylic plate centred on screen.

### session: `js/session.js`, `css/session.css`
- `Shell.session.lock()` (and `Win+L`): Windows 11 lock screen (wallpaper, large clock and date centre-top, network/battery icons bottom-right); any key/click/drag up raises it to the sign-in screen (blurred wallpaper, avatar, "Mihawk", PIN-less "Sign in" button); signing in fades back to the desktop.
- `Shell.session.sleep()` (screen fades to black; any input wakes to the lock screen), `.restart()` and `.shutdown()` (the "Restarting"/"Shutting down" screen with Windows' spinning-dots ring; restart boots back to the lock screen; shut down ends on black with "Click to start"), plus the classic Alt+F4-on-desktop "Shut Down Windows" dialog (listens to `alt-f4-desktop`).
- Boot: on first load plays nothing (desktop shows immediately); `?lock` in the URL starts at the lock screen.

### apps: `js/apps/explorer.js` + `css/apps/explorer.css`, `js/apps/settings.js` + `css/apps/settings.css`, `js/apps/small.js` + `css/apps/small.css`
- **File Explorer** (`id: 'explorer'`, pinned, `Win+E`): tabs in the title bar (new tab +, close, drag-free), command bar (New ▾, Cut, Copy, Paste, Rename, Share, Delete | Sort ▾, View ▾, Filter, … ), address bar with breadcrumbs (clickable segments, chevron folder menus, click empty space to edit the path) and Back/Forward/Up/Refresh, search box ("Search <folder>"), navigation pane (Home, Gallery, Desktop, Downloads, Documents, Pictures, Music, Videos pinned with pin glyph; This PC tree with drives; Recycle Bin), content views (Home with Quick access cards and Recent list; details view with sortable columns Name/Date modified/Type/Size and column resize; large/medium icons; tiles; This PC with drive usage bars), selection/rubber band/keyboard/rename/delete/new folder/drag items to folders, desktop or Recycle Bin (`Shell.drag`), details pane toggle, status bar ("7 items | 1 item selected"). Responsive: nav pane hides below ~640 px window width; command bar labels collapse into "…".
- **Settings** (`id: 'settings'`, pinned, `Win+I`; `open({page})`): Windows 11 Settings: account header (avatar, name, "Local account"), "Find a setting" search, NavigationView (System, Bluetooth & devices, Network & internet, Personalization, Apps, Accounts, Time & language, Gaming, Accessibility, Privacy & security, Windows Update) with icons; page title breadcrumb (Personalization > Colors), cards. Working pages: Personalization (Background: wallpaper picker grid + solid colours; Colors: Light/Dark mode combo, accent colour grid of Windows' 48 colours + custom, transparency effects toggle; Themes; Taskbar: alignment and search style), System > Display (brightness slider, night light toggle, scale combo showing 100%), System > Sound (volume), System > Notifications (Do not disturb), Accessibility > Visual effects (Animation effects, Transparency effects), Windows Update (static "You're up to date" card). Other pages show their real Windows 11 card layout with working toggles. Every control is bound to `Shell.settings`. Provides `Shell.settingsIndex = [{title, page, keywords, icon}]`. Responsive: below ~1000 px the nav pane becomes a hamburger overlay, as Windows does.
- **small apps** (`js/apps/small.js`): Notepad (`notepad`: Windows 11 Notepad with tabs, menu bar File/Edit/View, status bar Ln/Col, opens `{file}` text from `Shell.fs` and saves back), Calculator (`calculator`: Standard mode, working arithmetic, history), Terminal (`terminal`: Windows Terminal look with a tab and a tiny shell: `help`, `dir`, `cd`, `echo`, `cls`, `ver`), and `game` (`open({title})`: a toast "Starting <title>" via `Shell.notify` plus a short splash window that closes itself; games really run on the PS5). Registers each with icons; Notepad, Calculator and Terminal are start-pinned, Notepad and Terminal taskbar-pinned.

## Round 2 modules (added 2026-10-15 at the user's request)

### dialogs: `js/dialogs.js`, `css/dialogs.css`
Shell dialogs, each a real window through `Shell.wm.open` (modal to its owner where Windows makes it modal: clicking the owner flashes the dialog's frame and title bar). All return Promises.
- `Shell.dialogs.messageBox({title, text, icon?: 'info'|'warning'|'error'|'question', buttons?: ['OK'], defaultButton?, owner?})` → clicked label (or `null` on close). The Win32 MessageBox as Windows 11 draws it: white content with a 32 px icon and text, a grey footer strip with right-aligned buttons.
- `Shell.dialogs.taskDialog({title, instruction, content, icon?, commandLinks?: [{label, note}], buttons?, checkbox?, owner?})` → `{button, checked}`.
- `Shell.dialogs.run()` (Win+R): the Run dialog with history combo, Browse, OK/Cancel; runs app ids, app names, folder paths (`C:\`, `Documents`), `explorer`, `notepad`, `calc`, `taskmgr`, `control`, `ms-settings:`.
- `Shell.dialogs.properties(id)`: property sheet (General / Details; shortcut: Shortcut tab; drive: General with used/free pie; Recycle Bin), Apply/OK/Cancel, rename from the name box.
- `Shell.dialogs.openWith(id)` → app id or `null`: Windows 11 "Select an app to open this .<ext> file" dialog (Just once / Always).
- `Shell.dialogs.fileOpen({title, filters?: [{label, exts}], folder?, owner?})` and `fileSave({title, name, ext, folder?, owner?})` → `{parent, name, id?}` or `null`: the Windows 11 common file dialog (Explorer-style address bar, nav pane, item list, file name box, type combo, Open/Save and Cancel; overwrite confirmation).
- `Shell.dialogs.uac({app, publisher?, verified?})` → boolean: the secure-desktop consent prompt (dimmed frozen desktop, centred card).
- `Shell.dialogs.fileOperation({op: 'copy'|'move'|'delete', ids, to})` → `{done: ids}`: performs the operation through `Shell.fs`, shows the progress dialog for operations that take time (more than ~1 s simulated for large items; "x% complete", Pause/Cancel, "More details" with the speed graph) and Windows' "Replace or Skip Files" conflict dialog when names collide (Replace / Skip / Let me decide).

### taskmgr: `js/apps/taskmgr.js`, `css/apps/taskmgr.css`, and the not-responding part of `js/wm.js` / `css/window.css`
- App `taskmgr` ("Task Manager", Ctrl+Shift+Escape, Win+X): Windows 11 22H2+ Task Manager: title bar search box, hamburger NavigationView (Processes, Performance, App history, Startup apps, Users, Details, Services, Settings), Processes with Apps (one row per running app with expandable windows) and Background processes groups, CPU/Memory/Disk/Network columns with Windows' heat shading, "Run new task", "End task", "Efficiency mode"; Performance with live CPU/Memory/Disk/GPU graphs. Values are simulated and plausible; running apps come from `Shell.wm.all()`.
- wm gains `win.setNotResponding(bool)` and `win.isNotResponding()`; emits `window` types `hung` and `responding`. A hung window keeps its frame but its title reads "<title> (Not Responding)" after ~5 s; its content washes white as Windows' ghost window does; the pointer over it is the busy cursor; clicking it shows "<App> is not responding" with "Close the program" and "Wait for the program to respond". End task on a hung app closes it.

### controls: `js/apps/controls.js`, `css/apps/controls.css`
- App `controls` ("Windows controls", in Start's All apps, not pinned): the visual target for the Wine theme (`themes/windows11/source`, the msstyles that classic programs draw with). One window with tabs showing every Win32 common control as Windows 11 draws it in a classic (non-WinUI) program: push buttons (default, normal, disabled), check boxes and radio buttons (all states), group box, edit and multi-line edit, combo boxes (drop-down and drop-down list, open), list box, list view (report view with header and selection, icon view), tree view, tab control, trackbar, progress bars (normal, paused, error, marquee), up-down, scroll bars (Windows 11's thin overlay style and the classic arrows style), status bar, toolbar/rebar, menu bar with an open Windows 11 rounded menu, tooltips, date picker and month calendar, and a sample "game launcher" dialog that combines them. A Light/Dark switch shows what Windows 11 does in dark mode for classic programs (most Win32 controls stay light; only controls that opt into the dark Explorer theme, such as scroll bars, list and tree views, follow it), so the page is truthful about where Wine's dark theme goes beyond Windows.

### popups: `js/popups.js`, `css/popups.css`
- `Shell.osd.show('volume'|'brightness'|'mute', value)`: Windows 11's hardware-key flyout (small acrylic pill above the taskbar, centred: glyph, bar, value), shown for AudioVolumeUp/Down/Mute and BrightnessUp/Down keys.
- `Shell.clipboardPanel.open('clipboard'|'emoji'|'kaomoji'|'symbols')` and Win+V / Win+. / Win+;: the combined Windows 11 panel (tabs Emoji, GIF (offline state), Kaomoji, Symbols, Clipboard history), placed near the text caret when a field has focus, otherwise near the pointer. Clipboard history records text copies (`copy` events) and file copies (`clipboard` events), with pin, delete and "Clear all"; choosing an entry inserts text into the focused field. Emoji cells draw Fluent Emoji art from `assets/emoji/` (MIT, Color style, provenance in `provenance.json`); the inserted text is the character. Setting `clipboardHistory` (default on; off shows Windows' "Turn on" state).

### browser: `js/apps/browser.js`, `css/apps/browser.css`
- App `browser` ("MihawkBrowser", pinned after Terminal by load order, Start-pinned, icon `object/app-browser`): the native chrome of `src/pw_browser.c` for approval (docs/MIHAWK_BROWSER.md §4). Standard 32 px title bar ("<host> - MihawkBrowser" once a page loads) on the solid base; a 40 px command row with Back, Forward and Reload/Stop as 32×32 subtle buttons at 4 px gaps, the 32 px address box ("Search DuckDuckGo or type an address", lock glyph on a loaded page) and Downloads (a 16 px ProgressBar under its glyph while a download runs); the start view (two centred lines 40 px down, four 200×40 launcher buttons in three columns at 8 px gaps, 120 px down, the fourth alone on row 2); an error panel; the 360 px Downloads flyout under its button (rows: icon, name, ProgressBar or size, Cancel or Open (accent for an installer) and Show in folder). Pages are stand-ins (Chromium draws them on the console); launcher downloads are simulated and land in `Shell.fs` Downloads. Debug captures: `?open=browser`, `browser-page`, `browser-loading`, `browser-error`, `browser-downloads`.

### taskbar additions (taskbar owner)
`Shell.taskbar.flash(appId)` (Windows' attention flash: the button flashes orange, then stays highlighted with an orange pill until focused), `.setProgress(appId, value 0–1 | null, state 'normal'|'paused'|'error'|'indeterminate')` (the bar under the icon), `.setBadge(appId, text | null)`; settings `taskbarAutoHide` (slides away; sets `--taskbar-reserve: 0px` on the root so the work area grows; reveals at the bottom edge and on Win+T), `taskbarPeek` (hover over the far-right sliver turns windows into glass outlines), `taskbarFlashing`, `taskbarBadges`; mouse wheel over the volume icon changes volume; Win+X's Task Manager and Run now work.

### cursors: `css/cursors.css`, `assets/cursors/**`
Windows-style cursors authored as SVG (arrow, help, working-in-background, busy, precision, text, unavailable, resize ns/ew/nwse/nesw, move, link hand, plus drag badges copy/move/link and the "+ Copy to <folder>" drag label style). Owns every `cursor:` declaration in the POC's CSS and JS: it may change only `cursor` values in other modules' files. Windows uses the arrow, not the hand, over buttons and menu items.

## Icons

New icons in round 2: a module may ADD new files under `assets/icons/object/` (original artwork, prefixed by what they are, e.g. `dialog-warning.svg`, `app-taskmgr.svg`) but never edits existing icon files or the JSON manifests; the integration pass records them in `provenance.json` and `manifest.json`.

`fluent/<name>`: 20 px Fluent System Icons (MIT, microsoft/fluentui-system-icons @ cd332ec1f90cda2fad16f86754f8da52f1e9ab30), in `assets/icons/fluent/`, monochrome `currentColor`. `<name>-filled` is the filled variant where listed. Use only these names:

add, alert, alert-off, apps, apps-list, arrow-clockwise, arrow-down, arrow-left, arrow-right, arrow-sort, arrow-sync, arrow-up, accessibility, airplane, battery, battery-charge, battery-saver, bluetooth, brightness, brightness-low, calculator, calendar, camera, cast, checkmark, chevron-down, chevron-left, chevron-right, chevron-up, clipboard-paste, clock, cloud, color, copy, cut, delete, desktop, dismiss, document, document-add, edit, filter, flashlight, folder, folder-add, folder-open, games, globe, grid, hard-drive, history, home, home-filled, image, image-multiple, info, keyboard, lock-closed, location, mail, maximize, mic, more-horizontal, music-note, navigation, open, options, paint-brush, person, phone, pin, pin-off, play, power, print, question-circle, rename, restore, save, search, select-all, settings, settings-filled, share, shield, sign-out, speaker-0, speaker-1, speaker-2, speaker-mute, square-multiple, star, subtract, tab-desktop, text-bullet-list, text-font, timer, task-view, video, weather-moon, weather-sunny, wifi, wifi-off, window, window-new, wrench, zoom-in, zoom-out.

`object/<name>`: colour object icons in `assets/icons/object/`. The repository's approved originals (copied with their hashes): folder, folder-open, documents, downloads, pictures, music, videos, desktop, games, this-pc, recycle-empty, recycle-full, drive, document. Originals authored for this POC in the same style: app-explorer, app-settings, app-notepad, app-calculator, app-terminal, app-store, app-game, app-generic, image, text, user-avatar, shortcut-arrow (the 16 px overlay badge), network-wired.

## Ownership

| Owner | Files |
| --- | --- |
| foundation | `index.html`, `js/core.js`, `css/tokens.css`, `css/base.css`, `tools/gen-tokens.py`, `CONTRACT.md` |
| icons | `assets/icons/**` |
| ui | `js/ui.js`, `css/ui.css` |
| wm | `js/wm.js`, `css/window.css` |
| desktop | `js/desktop.js`, `css/desktop.css` |
| taskbar | `js/taskbar.js`, `css/taskbar.css` |
| start | `js/start.js`, `js/search.js`, `css/start.css` |
| flyouts | `js/quicksettings.js`, `js/notifications.js`, `css/flyouts.css` |
| taskview | `js/taskview.js`, `css/taskview.css` |
| session | `js/session.js`, `css/session.css` |
| explorer | `js/apps/explorer.js`, `css/apps/explorer.css` |
| settings | `js/apps/settings.js`, `css/apps/settings.css` |
| small-apps | `js/apps/small.js`, `css/apps/small.css` |
| dialogs | `js/dialogs.js`, `css/dialogs.css` |
| taskmgr | `js/apps/taskmgr.js`, `css/apps/taskmgr.css` (and the not-responding additions in `js/wm.js`, `css/window.css`) |
| popups | `js/popups.js`, `css/popups.css` |
| controls | `js/apps/controls.js`, `css/apps/controls.css` |
| cursors | `css/cursors.css`, `assets/cursors/**`, and `cursor` values anywhere |

A module that needs something another module does not offer works around it inside its own files and records the gap at the end of its report; it never edits another owner's file.
