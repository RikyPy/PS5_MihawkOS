# Mihawk OS

The `mihawk-os` branch adds a Windows desktop session on the existing Wine
runtime. It is a shell and shared application environment, not another kernel
or VM. The existing owner retains wineserver, GPU/display, controller and audio
ownership. The title runs the session itself: Explorer is its root, and the
title unloads and restarts Explorer around full-screen games (game mode, below);
programs launched by the shell are ordinary Windows children.
Closing an application must leave the shell alive. Closing the shell ends the
session through normal owner cleanup.

## Prefix selection

`src/pw_wine_launch` owns the folder-ID policy. The catalog and native owner
use the same selection through `proton_game_prefix`:

| Entry | Prefix folder under `/data/ps5proton/prefixes` |
| --- | --- |
| Mihawk OS desktop | `mihawk` |
| Every catalog game | `mihawk` |
| `game.json` with `"prefix": "work"` (test harnesses' throwaway prefixes) | `work` |
| Bare diagnostic executable path | `probe` |

IDs allow up to 64 ASCII letters, digits, underscores and hyphens. Paths and
traversal are refused. A per-game folder no longer pins a game to itself: on
2026-10-10 every game moved into Mihawk OS (below), and the per-game prefixes
were retired. The fresh-prefix, baseline and TLS harnesses name their marked
throwaway prefix explicitly.

## Every game in Mihawk OS (2026-10-10)

The user's direction: no prefix per game; every game lives inside Mihawk OS.
A one-time migration tool (since removed from the tree) moved the library on the
console, journalled step by step with its backup in
`/data/ps5proton/migration/<stamp>/`:

- Each game's folder moved from `/data/ps5proton/games/<id>` onto Mihawk OS's
  C: drive, by a rename on the same filesystem: `C:\Games\<name>`, and a game
  launcher under `C:\Program Files (x86)` where Windows puts it. The library
  entry stays at `games/<id>/game.json`, naming the new executable with
  `"prefix": "mihawk"`.
- Each game prefix's own user data (AppData, Documents, Saved Games,
  ProgramData; caches and Temp left out) was copied into `mihawk`; where `mihawk`
  held a file with other bytes (from the frame-time test), the game's copy won
  and the other went to the backup. The launcher's registry values and its
  embedded browser's keys were merged with their paths rewritten; no other
  prefix held game-specific registry.
- Files that named the old folder (a Java launcher's instance files, an
  application's settings file) were rewritten; logs keep their history.
- The per-game prefixes moved to `/data/ps5proton/retired-prefixes/<id>`; nothing
  was deleted.

After the move every library game gets a desktop shortcut; the Java file test
is a diagnostic (`"desktop": false`).

**Desktop shortcuts.** At each desktop session start, before Explorer, the
title writes one `.lnk` per library game in the session's prefix into
`C:\users\Public\Desktop` (`src/pw_shell_link.h`, host-tested and read back by
Wine's own `IShellLink` in `tools/check-shell-link.sh`). It owns those files
through a manifest: the user's own shortcuts are never touched, and a game
added to the library gets its shortcut at the next session.

**Each game's own settings in the session.** A game started from the desktop is
Explorer's child; its helper took Explorer's environment, so a .NET (MonoGame) game had no
OpenGL (`WINE_PS5_OPENGL`). The new process's executable and command line now
reach the title, which finds its `game.json` entry (`src/pw_app_environment.h`;
entries that share an executable are told apart by their arguments) and gives
the helper those settings on its Unix side and, through patch 0998, its Windows
side. Settings that are the title's own (`PROTON_*`, `MIHAWK_*`,
`WINE_PS5_NTSYNC`) stay out: in particular a Java game's 120 Hz output is not
available from the desktop session, which opens the display once.

The desktop is a built-in library choice whose entry is
`C:\windows\system32\mihawk.exe`; no such Windows program runs. The title
starts Explorer at 1920×1080 as the session root with `MIHAWK_SESSION=1`, and
Explorer enables its shell on the standard `WinSta0\Default` desktop and makes
its desktop a Windows-style Progman window (Wine patch 0996). Wine
intentionally registers `GetShellWindow` only on `Default`; a named virtual
desktop is not the intended Windows shell. The native launcher remains
the entry point during qualification. A new prefix uses the same wineboot and
[runtime-owned baseline](SHARED_PREFIX.md) as every other application.

The appearance app’s label checkbox currently controls its icon previews only.
Light/dark selection persists through Wine’s ThemeManager; desktop-wide label
preferences are not implemented or qualified.

## Application environment contract

The qualified initial-environment increment separates two contracts. Existing
bootstrap runtime settings remain an explicit allowlist. Catalog and one-shot
application overrides form a separate block of at most 4 KiB, including double
NUL termination. Catalog order comes first and one-shot order last. The matching
loader/runtime protocol refuses old layouts; malformed or oversized blocks fail
before entering the application. The title's entire host environment is never
forwarded.

The initial Windows environment applies those UTF-8 entries after wineboot and
registry setup, using Wine's Unicode, case-insensitive replacement. Empty values,
`=` in values, caller-selected PATH and last-override precedence are preserved.
CreateProcess children use their ordinary inherited or explicit Windows block;
root settings are not reapplied. This does not yet qualify propagation of every
Windows-changed runtime setting back into descendant Unix libraries.

Pinned Linux Wine passes the paired headless value/inheritance oracle and the
portable transport passes sanitized host tests. The focused console pair also passes
all 39 checks per architecture, with normal root returns in 32.2 seconds for the
pair. The complete 44/44 exact-source gate passes in 1,408.5 seconds, with zero
recorded fatal CPU/GPU faults and zero process-protocol errors. This qualifies
application environment and preserves the existing desktop scope; it does not
qualify independent cross-process window composition. See
[focused record](../evidence/mihawk-environment-focused-checkpoint-2026-10-09-03.json) and
[host record](../evidence/mihawk-environment-host-2026-10-09.json) and
[complete gate](../evidence/mihawk-environment-gate-2026-10-09-03.json).

## Everyday shell batch

The approved taskbar, Start and Explorer increment is implemented together in
patches 0981–0987 and the owned `pw_shell_*` helpers. It extends the existing
native Wine shell and shared appearance system under `MIHAWK_DESKTOP=1`.
Ordinary non-Mihawk Explorer sessions keep their existing route. It does not introduce a browser
frontend, a parallel filesystem model or a replacement source of window state.
The implementation is qualified by the exact-source 58-case gate below and is
committed locally as `a966406`.

| Surface | Actual behavior and owner | Current limits |
| --- | --- | --- |
| Taskbar | Wine's real window list drives centered native buttons, foreground indicators, activation, keyboard navigation, window-title tooltips and an overflow menu. The reserved work area, notification area and live clock remain native. | Same-process window icons are queried normally. Foreign processes use a cached icon from their real executable because Wine cannot marshal foreign `WM_GETICON`; dynamic foreign per-window icon changes are unimplemented. |
| Start | A fresh local snapshot reads user/common Programs and Desktop entries, plus installed built-in helpers. Search filters actual entries; launching uses shell execution. Pins persist under `HKCU\Software\Mihawk\Shell\StartPins`; recent files come from Wine's shell folder. | Bounded to 1,024 apps, 256 directories, depth eight, 18 pins and six recent files; directory reparse points are not followed. Incomplete enumeration is reported. This is local application search, not a system-wide file or web index. |
| Explorer | The native address box, Back/Forward/Up and known-folder sidebar navigate `IExplorerBrowser`. New folder, Cut, Copy, Paste, Rename, Delete, View and Refresh act on its real shell view. Browser travel state owns navigation availability; real selection and clipboard state control commands. | Qualification covers the named probe interactions, not every file-operation failure or shell extension. No separate history or file model is maintained. |

`themes/windows11/appearance.json` owns the shared light/dark palette and layout
metrics; the managed Proton UI family and existing original/Fluent icon assets
remain authoritative. Live theme changes refresh the shell, captions, header
text and implicit list-view colours while preserving explicit application
background colours. Explorer's public executable icon matches its runtime
folder resources. Disabled glyphs use the standard alpha-aware image-list path
at half opacity, with a capture ink-count check for missing strokes.

The approved reference and native implementation share the centered 48-pixel
taskbar, 600×566 Start panel and 165-pixel Explorer sidebar at 1920×1080, 96 DPI.
Square opaque fallback surfaces are accepted for this batch. Rounded outer
clipping, shadows, blur, transitions and previews remain separate work; their
absence is not a new token or design rule. The HTML supplies visual direction
only; its embedded private wallpaper/font are not imported. Missing
`PRODUCT.md`, `DESIGN.md` and `.impeccable/design.json` are pre-existing drift,
left unchanged while extending the incumbent system.

### Evidence and proof limits

The [first focused pair](../evidence/mihawk-shell-focus-2026-10-09-01.json)
passed 56 checks per architecture in 71.7 seconds, but visual review found dark
file-pane, text, caption and state/layout gaps. Stronger checks retained the
failures in [02](../evidence/mihawk-shell-focus-2026-10-09-02.json),
[03](../evidence/mihawk-shell-focus-2026-10-09-03.json),
[04](../evidence/mihawk-shell-focus-2026-10-09-04.json) and
[05](../evidence/mihawk-shell-focus-2026-10-09-05.json); they are not acceptance
records. They all returned to the launcher with zero recorded CPU/GPU fatal
faults and process-protocol errors.

The [sixth focused pair](../evidence/mihawk-shell-focus-2026-10-09-06.json)
passes both client architectures and all captures in 72.2 seconds, with normal
returns and zero recorded CPU/GPU fatal faults or process-protocol errors.
Review of the preceding valid 1920×1080 captures resolved the original six
surface/state findings, while fragmented disabled arrows remained a partial
seventh finding. The final alpha-aware correction's
[build08 focused pair](../evidence/mihawk-shell-focus-2026-10-09-08.json)
passes **70 checks per architecture and all eight pixel captures in 70.7
seconds**, with clean returns and zero recorded CPU/GPU fatal faults or
process-protocol errors. The finish reviewer scores the disabled-arrow correction
resolved in all four Explorer captures; all seven original findings are resolved. Its **ship** verdict
covers the scored fixes. Captures remain ignored in `klog/`; no qualification
or commit is inferred from a focused pass. The design workflow recorder could not persist `ship` because this native
extension has no `.impeccable/build/state.json`; no missing phase state was
fabricated. The reviewer verdict is recorded here.

The complete [build08 exact-source gate](../evidence/mihawk-shell-gate-2026-10-09-08.json)
passes **58/58 in 1,748.1 seconds (29 min 8.1 s)**, with zero recorded CPU/GPU
fatal faults and zero process-protocol errors, on fingerprint
`7b415f582b48d3c8aaead8052a90bf825ed8614eae3a6af4913cf2b761fc9305`.
This qualifies the integrated everyday shell batch for a local commit; the
remaining scope limits below still apply.

`tests/shell-probe.c` and `tools/run-shell.py` exercise PE32 and PE64 clients
against the real shell, including persistent pins, actual child launch,
keyboard/address navigation, Copy/Paste with byte comparison, New folder,
light/dark captures and cleanup. The qualification clipboard must initially be
empty; arbitrary existing user clipboard preservation is not claimed. A native
Windows 11 reference is unavailable, so these are native Wine behavior and
approved-design comparisons, not universal shell support or Microsoft pixel
equivalence. Broader window behavior and materials remain later batches.

## Earlier session qualification

The initial diagnostic capture shows a 1920×1080 Wine shell, wallpaper and Start
bar. Its bounded stop did not close Explorer naturally: the runner eventually
terminated server clients and recorded root status 1. That is not clean logout
evidence. Static-desktop screenshot requests wait for a later displayed frame;
the second run captured one image and timed out waiting for the next.

The first shared-prefix session run stopped during first initialization after
123.8 seconds. A repeat reached the desktop but failed shell checks because
the probe expected `GetShellWindow` on the named virtual desktop. Environment,
shared-file access and the child window's own painting passed; no displayed
child-window correctness was established. The root exited with status 0, but
the session failed. The corrected `Default` session is qualified below.

`tests/desktop-session-probe.c`, built in both architectures, checks shell
residency, child windows, inherited environment, shared C-drive visibility,
child exit codes and real File Explorer launching/closure. It then posts the
desktop's ordinary close message. Console results must qualify all these
boundaries before changing the boot default.

The corrected `Default` session passes all 16 parent checks and both children's
six checks. Each child sees the shell, inherited environment and the same
prefix file, paints and exits with code 23. File Explorer starts/closes and the
shell survives all three applications. Ordinary desktop closure ends Explorer
and the session starter with code 0; the owner/broker return is clean, without
watchdog intervention, in 39.1 seconds. See
[session record](../evidence/mihawk-session-2026-10-09-03.json).

That first lifecycle pass did not qualify the displayed windows. All three captures
showed the shell instead of the foreground application. The broker log records
the correct foreground PID but allows a shell repaint to take the screen through
its 300 ms idle rule. The pinned local graphics overlay rejects background
sources while a foreground process exists and re-presents retained GDI frames
on focus change. `tools/run-desktop-session.py` adds actual display-pixel checks
for distinct PE64/PE32 window colors and substantial File Explorer content; it
correctly rejects the preceding shell-only captures. The exact-build repeat
passes all three captures: PE64 blue, PE32 green, and the actual File Explorer
C: window. Normal logout and cleanup pass in 39.1 seconds; see
[display qualification](../evidence/mihawk-display-2026-10-09-01.json).

A second session also passes, alongside graphics-child teardown and 32/64-bit
input checks. The overlap regression's first three captures pass, but its final
capture still shows the front window: the request at eight seconds precedes the
probe's close at ten seconds and can consume an earlier queued repaint. That
failed record is preserved. The harness now requests the last capture after
closure and moves the pointer to redraw retained pixels, without repainting the
background or changing its expected colors. Its exact-build repeat passes all four captures, followed by another successful
desktop session; both cases complete in 75.5 seconds with clean normal shutdown.
See [original focused results](../evidence/mihawk-focused-2026-10-09-01.json)
and [corrected repeat](../evidence/mihawk-final-focused-2026-10-09-01.json).

Later sections record the subsequent qualified opt-in composition milestones;
the everyday shell batch above records the current taskbar, Start and Explorer
work. Catalog-to-shell shortcuts preserving arguments, working directory and
per-application environment, plus explicit shared-prefix migration and selection,
remain separate work. Incompatible registries must not be blindly combined.

One owner currently hosts one active prefix. Switching to another prefix ends
that session normally; concurrent independent prefix sessions are not proven.
Composition across process windows, persistent input focus and game graphics
handoff remain acceptance checks. No complete desktop or game launcher compatibility
claim follows from the first wallpaper capture.

## Approved appearance implementation (2026-10-09)

The user approved the existing Windows 11-style light/dark design and mixed
icon set, and authorized autonomous implementation with local gated commits.
The HTML remains a visual reference; none of its embedded wallpaper/font or
browser implementation is imported. `themes/windows11/appearance.json` owns
the palette and shared metrics at 1920×1080, 96 logical DPI, text scale 1.0.
The approved original objects and pinned MIT Fluent command SVGs are imported
with all forty-eight input hashes verified. Their independent SVG sources,
licences and provenance remain editable in this repository.

The first appearance slice adds resource-only icons, Wine shell32 object
resources and a real appearance selector using existing uxtheme application
and theme notifications. It packages both colour variants in the existing
managed default; startup does not change user-selected alternate themes.
Explorer creates an Appearance shortcut only when that name does
not already exist. The diagnostic checks both variants, decoded icon sizes,
actual displayed panes and alpha icon regions, then restores its prior theme.
Host resource/pixel-verdict checks pass. The complete 40-case console gate passes on fingerprint `dc021394024cd0186060e3c8e67d396e7ed9efd014691df7f705697e8bd2d36f`. Both appearance architectures now pass 1,001 API checks and both light/dark pixel captures; the subsequent font-unit correction passes all 1,009 checks in each architecture.

| Feature | Owner | Reused behavior | New work / limit | Host proof | Console proof | Windows reference |
| --- | --- | --- | --- | --- | --- | --- |
| Persistent session | Starter / Wine Explorer | Default desktop, normal processes/logout | Native launcher remains boot default | Prefix/UI checks pass | Session and actual child/Explorer pixels pass | Unavailable |
| Shared prefix | Catalog / native owner | Existing managed baseline | Explicit migration and selection UI pending | Policy tests pass | Shared session file/environment pass | Not applicable |
| Approved icon inputs | Theme builder / shell32 | Existing IDs, actual folder/bin state | Preserve app-provided icons; new command glyph resource package | 48 hashes; 82 icons at 6 sizes; resource-only DLL verified | Both variants decoded and displayed in PE32/PE64 | Original approved assets differ deliberately |
| Light/dark controls | Theme / uxtheme | Existing part/state geometry and notifications | Opaque dark sprite fallback; no blur/material claim | 964 resources, 956 resolved bitmap references; pixel oracle tests pass | Light/dark API and displayed icons pass; updated font checks pass | Unavailable |
| Appearance preference | Appearance app / uxtheme | ThemeManager persistence and broadcast | Appearance shortcut; alternate themes retained | Host GUI unavailable; resource tests pass | Light/dark API and displayed icons pass; updated font checks pass | Unavailable |
| Taskbar / Start | Wine Explorer shell | Actual window/application state and shell launching | Centered native layout, local search and persistent pins qualified within the named checks | Paired host interaction checks; production theme differs from host | 58-case exact-source gate; focused pair: 70 checks per architecture, all captures pass | Unavailable |
| Cross-process composition | Broker / Wine | Authoritative HWND lifetime/order, hardware input | Opt-in opaque GDI/GPU qualified; shaped/layered content pending | 50 sanitized suites; pinned DXVK Linux reproduction/fix | 54-case exact-source gate; four mixed GPU/GDI cases pass, 23 checks and five captures each | Unavailable |
| Explorer | Wine shell/file APIs | Actual navigation, history, selection and operations | Approved native navigation/command layout qualified; dynamic foreign window icons unsupported | Paired host interaction checks | 58-case exact-source gate; real navigation, Copy/Paste, New folder and captures pass | Unavailable |
| Materials / previews | Server DWM state / compositor / uxtheme / shell | Public DWM attributes, frame margins and thumbnails; Wine regions and hit-testing unchanged | Rounded corners, borders, shadows, Mica/acrylic glass, live thumbnails; Mica taskbar, acrylic Start, taskbar previews. Transitions/animations, blur-behind transparency, iconic thumbnails and DWM-drawn captions are not implemented | Effects/materials/thumbnail suites; two differential compositor-vs-float-reference tests | Paired effects probes: all API checks and 54 displayed samples within one count; host-judged acrylic Start, corner, border and preview thumbnail | Unavailable |

A native Windows 11 reference environment is unavailable. No Windows pixel
equivalence or measured Microsoft animation-timing claim is made. The initial appearance checkpoint used forty cases. The current mandatory gate
contains **58 cases**, including paired GPU/DXVK windows and everyday shell probes. A focused pass never
qualifies a code commit; `tools/run-battery.py` is the case-list authority.

The initial appearance console batch did not reach either appearance executable:
both were missing from the staged diagnostic prefix. The session repeat also
failed shell residency because the starter's static GUI imports created an
implicit desktop before its registry setup. Patch 0968 makes Explorer own
shortcut installation after its desktop exists; the starter is headless again.
The failed batch is preserved, with no appearance acceptance claimed. The Linux
Wine GUI comparison is unavailable in the existing headless host configuration;
resource checks and negative image-verdict checks pass, but they do not qualify
a displayed GUI.

## Retained GDI composition contract

This contract shaped the now-qualified opt-in opaque GDI implementation. The
legacy default transport shares one composed frame per process; that cannot
represent three windows whose stacking interleaves across two processes.
Reusing process foreground arbitration alone would also erase the taskbar
behind applications. The contract below records the intended ownership; the
qualification checkpoints describe what the actual runs prove.

The existing packet/descriptor channel can carry more than one frame object
per process. Extend the pinned frame format with an explicit window identity
and lifecycle, while retaining the legacy whole-frame path. The title must
acknowledge attachment and report capacity/allocation failures; a successful
send alone is not proof that a surface was adopted. Associate sources with
the owner-bound spawned native child PID and Wine window handle (not kernel-authenticated peer identity). Publish retained
content only through the existing shared application-memory allocator.

Wine remains authoritative for order, visibility and geometry. Its server
thread should publish a bounded coherent snapshot; the display thread reads
that snapshot, never traverses mutable server objects directly. Compose window
content in that order, with stable sequence checks around shared pixels. Keep
unchanged content after focus changes and background paints. Hide/destroy
removes a surface, handle reuse replaces its prior generation, and child exit
releases every owned surface. Preserve the independent window-view and Vulkan
presentation paths while qualifying the GDI slice; Vulkan/OpenGL client surfaces
need their own HWND association before claiming full desktop composition.

The permanent probe must show shell pixels plus simultaneous PE64/PE32 windows,
including interleaved order, an obscured background repaint, focus change, move,
hide/show, close and normal session logout/relaunch. Actual captured pixels,
input routing and bounded cleanup are required. This first slice does not
claim shaped-window clipping, alpha materials, GPU thumbnails or blur.

The complete [40-case checkpoint](../evidence/mihawk-gate-2026-10-09-01.json)
passes with zero CPU/GPU fault records and zero process-protocol errors. This
qualifies the shared-runtime/session/appearance foundation for a local commit.
It does not qualify the pending full shell design or per-window composition.

### Composition inspection checkpoint

The production owner already resolves `pw_wineserver_input` and prefers it to
the helper-local input queue. Its server thread injects hardware input through
Wine's authoritative desktop routing and publishes the resulting cursor. The
new compositor must preserve this path, not introduce another input dispatcher
or a competing focus registry. The helper driver's local `shown_motion` clamp
is not evidence that the production shared input path needs replacement.

The patched driver retains ordinary local top-level GDI surfaces, while
`SetDesktopWindow` currently publishes only desktop geometry/display setup.
Its root desktop surface is absent from the local retained-window table.
Independent shell/background pixels are therefore a prerequisite alongside
the HWND surface transport and coherent server-owned stacking snapshot.
The broker's current frame adoption has no acknowledgement; the new transport
must return explicit ownership/capacity status before reporting adoption.
These are inspected boundaries, not console-qualified new composition.

## Shared appearance checkpoint

The exact [42-case gate](../evidence/mihawk-theme-gate-2026-10-09-14.json) passes
in **1,382.5 s**, with zero recorded fatal CPU/GPU faults and process-protocol
errors. PE32 and PE64 appearance each pass **1,029 checks**, including 1,320
pixels of equality for each selected light/dark close/maximize/minimize part.
Independent console images match each pane and alpha icon region, plus eight
distinct central glyph pixels per part. Capture readiness now uses a bounded
displayed-image rendezvous, so static windows do not need pointer motion.

A separately running process can change appearance while existing PE32/PE64
windows stay alive: both receiver probes pass **14/14 checks**. New theme
handles and system brushes refresh; previously retained theme handles remain
owned until closed. Alternate themes leave the managed preset radios unchecked
and use pane luminance for legible command icons.

This is a qualified normal-caption and notification slice. Hover/pressed/
disabled/inactive behavior, white classic frame edges in dark mode, restored
rounded clipping, taskbar/Start styling and HWND composition remain work.
A Windows 11 reference environment remains unavailable; no Microsoft pixel
identity is claimed. The next shared contract is explicit initial application
environment forwarding, followed by the retained-window composition slice.

### Qualified retained-window metadata prerequisite

Wine's input desktop owns the HWND order and lifetime. Patch 0975 copies its
128-row bounded window list after mutations and asynchronous destruction. Each
row includes full HWND, lifetime generation, native/Windows PID, owner HWND,
visible rectangle and separate visibility/desktop/region flags. An overflow is
explicit; the consumer must never treat it as a complete scene. Shaped/layered
content is identified, not silently rendered as an opaque rectangle.

The native observer reads only a coherent copied record under an independent
mutex, without dereferencing Wine objects. A marked, prefix-local file rendezvous
lets actual PE32/PE64 probes compare that record with their Win32 windows. It is
diagnostic transport, not a second source of window state or a production window
API. Pixel surfaces, GPU association and composition remain separate next steps.

The first metadata run that reaches the probes (build05) passes 23/24 checks
in both architectures. All copied state, ownership, geometry, visibility,
lifetime and overflow checks pass; the first GetShellWindow assertion races
Explorer registration. The later shell-survival assertion passes. The diagnostic
now waits for the actual shell window under a five-second deadline. Root rows
have a live Explorer native owner, allowing a real retained-root surface rather
than attributing a desktop image to an arbitrary helper. This is partial evidence;
the corrected repeat and complete gate results follow.

The bounded readiness repeat (build06) passes all 24 checks in both
architectures in 50.1 seconds for the pair, with normal shell logout and owner
cleanup and no recorded fatal CPU/GPU fault or process-protocol error. See
[focused checkpoint](../evidence/mihawk-window-focused-checkpoint-2026-10-09-06.json).
The complete **46/46 exact-source gate passes in 1,422.9 seconds**, with
zero recorded CPU/GPU fatal faults or process-protocol errors.
[Full gate](../evidence/mihawk-window-gate-2026-10-09-06.json).
Independent retained pixels and mixed rendering remain unqualified.


### Independent retained GDI pixels (qualified opt-in)

The exact [50-case gate](../evidence/mihawk-repaint-gate-2026-10-09-02.json) passes **50/50 in 1,554.2 s**
on `932c68c32a631a32e2a46c154e0ed7051afbd22bc7a7dcc64576873393f6a27a`, with no recorded fatal CPU/GPU faults
or process-protocol errors. Both mixed-process probes pass 19/19 checks and five
actual captures each, including canonical application-set cursor coordinates
and displayed arrow pixels. Both wallpaper probes pass 13/13. Appearance now
paints alpha artwork idempotently: 1,035 checks each and 576/576 matching pixels
after three redraws in both variants. Phase-driven session captures pass the
actual PE64, PE32 and Explorer images, with 18 parent/eight child checks and
normal logout. The 47 sanitized host suites pass.

The route remains global opt-in: GPU HWND planes, shaped/layered windows, cursor
shape/visibility parity and complete Windows 11 shell behavior remain open.
An intermittent pre-marker helper EOF occurred in earlier gates; added close and
receive observations do not establish a fix merely because this gate passes.

The following earlier runs preserve failures that shaped the qualification.
The opt-in route uses a helper-owned, variable-extent physical page per
HWND lifetime. Every descriptor adoption is acknowledged after native PID,
server generation, extent and capacity checks; failed replacement preserves the
prior page. Fields except pixels/sequence are immutable. The broker copies only
stable new images and composes them using the server's order and visible bounds.
The actual local Explorer root can publish its own surface; a flattened process
frame is never treated as a background. Native exit and window destruction
release mappings. Shared pages and retained images are each bounded to 64 MiB;
Wine's 128-pixel surface padding is preserved and clipped to 1920x1080 output.

`WINE_PS5_WINDOW_SURFACES=1` selects this route globally for all helpers and
ordinary descendants in the session. The production default stays on its
qualified path while GDI/GPU association is unfinished. Shaped/layered content
and GPU process images explicitly cannot substitute opaque rectangles in this
slice. The paired probe reads the actual broker-written PPMs and an independent
host verdict reads the fetched captures. Interleaving, static reorder,
move/resize, minimize/restore, modal ownership and actual hardware-queue mouse
messages are checked. Build03 passes 14/14 checks per architecture, all ten
captures pass independent pixel/background checks, and the paired wallpaper
probes pass 9/9. The four-case focused batch takes 80.1 seconds with no recorded
fatal/protocol/exit faults. The full gate finishes 49/50: appearance64 ends before its marker with owner
disconnection after worker start and initial status 1. The composition cases
pass. No code commit is allowed; terminal-receive diagnosis and the unchanged
paired appearance repeat are next.
That earlier build had not qualified application-driven visual cursor movement;
patch 0977 and the complete gate above now qualify its coherent coordinates and
actual arrow pixels. GPU HWND association and layered/region content remain
unqualified. See the [focused batch](../evidence/mihawk-composition-focused-2026-10-09-03.json).

The real Explorer root paints Wine's desktop colour unless its wallpaper is
configured. The headless session starter now initializes the approved managed
BMP through the normal registry once per prefix. Existing nonempty choices and
subsequent explicit empty choices are preserved. Paired probes restore their
original registry state; displayed background checks compare the captured root
with the packaged image, rather than painting it into the compositor.

A follow-up review found that the current initializer treats an absent local
wallpaper file as a startup error. All displayed qualification above uses the
provided staged BMP; the no-asset case is unqualified. A global correction and
paired missing-file probes are prepared for the next increment, together with
canonical native cursor publication. No private wallpaper will be vendored.


### Qualified opt-in mixed GDI and GPU HWND route (2026-10-09)

The candidate uses snapshot v2 with real client rectangles, independent graphics
children and Wine's canonical visible-region clips. The owner binds a GPU slot
to its actual spawned native PID before bootstrap; INIT must match. Each native
WSI surface retains full HWND and generation. Fence-completed GPU pixels are
copied into bounded private storage before presentation acknowledgement.
Opaque GDI backing remains available beside GPU clients; canonical clips are
not replaced by guessed rectangles.

The first black Direct3D images were already black at the completed-backing
boundary. Readback made them correct, but was diagnostic only. The same
unbound-clear/Present failure reproduced on Linux in both architectures with
our pinned DXVK. Its source overlay prepares deferred clears and ordinary
shader-readable layouts before external rendering. All ten DLLs are built from
checksum-pinned archives and checked during managed-prefix packaging.

[Production build07's focused Pro run](../evidence/mihawk-gpu-window-focus-2026-10-09-07.json)
passes four OpenGL/DXVK architecture pairs in 168.5 seconds: 23 checks and all
five display captures each, hardware input, modal/GDI overlap, resize, later
colour changes, hidden/restored child and normal child/session exit. No D3D
readback or binding workaround is used. The entire [exact-source gate](../evidence/mihawk-gpu-window-gate-2026-10-09-07.json)
passes **54/54 in 1,720.2 seconds**, with zero recorded fatal CPU/GPU faults or
process-protocol errors, on
`e90c3d885030ed489dcafa9a769a3398ed8c9c05daf18fe5c6793f8a1610c5be`.
Legacy presentation remains the default; frame pacing and shaped/layered surfaces
are not qualified.

Shaped/layered windows, arbitrary cursor shapes/visibility, materials, previews,
complete shell appearance and compositor frame-pacing qualification remain
separate work. This milestone does not establish full Windows equivalence.


### Qualified shaped and layered GDI (2026-10-09)

The opt-in retained-window route now carries server-authored whole-window
regions separately from GPU-client clipping. Empty regions draw nothing;
removing a region restores rectangular drawing. The shared page format declares
opaque or premultiplied pixels, with acknowledged replacement when that format
changes. Wine supplies colour-key and per-pixel coverage. The producer applies
constant opacity once; the compositor blends retained cross-process pixels.

Input remains authoritative in Wine. The owner publishes the surface's binary
coverage to the matching HWND generation, independently of `GetWindowRgn`.
The server validates dimensions, ownership, lifetime and bounded storage before
replacing coverage, and releases it at destruction. Its ordinary hit-test skips
transparent pixels. This applies globally to the supported window route rather
than to named applications.

The [first alpha run](../evidence/mihawk-alpha-focus-2026-10-09-02.json) passes
all 13 captures per architecture but fails transparent-pixel click-through.
The [input correction](../evidence/mihawk-alpha-focus-2026-10-09-03.json) passes
47 checks and 13 captures per architecture, 2/2 cases in 112.6 seconds, with
normal returns and no recorded fatal faults or process-protocol errors.
The [expanded lifecycle run](../evidence/mihawk-alpha-focus-2026-10-09-04.json)
finds stale 50% opacity after removing the layered style, in both architectures.
Patch0991 resets the driver-owned factor through Wine's style-change callback.
The [corrected pair](../evidence/mihawk-alpha-focus-2026-10-09-05.json) passes
**55 checks and 14 captures per architecture**, **2/2 in 140.4 seconds**, with
normal returns and zero recorded fatal faults/protocol errors. It covers
visible-pixel input, movement, transparency removal and the previous cases.
The first complete gate exposed a surviving native helper; later lifecycle
probes found telemetry blocking the loader, late control-endpoint release and
an observer spinning on unread packets. These corrections and a stale-brush
fix in Appearance are qualified together by the
[final gate](../evidence/mihawk-alpha-gate-2026-10-09-12.json): **59/59 in
1,893.9 s**, exact fingerprint `fc1dc2db483b895d8325c826304065a1fc1f649aa71275646ee64cec237ab7b3`.
Both composition clients repeat all **55 checks and 14 captures each**. Eight
children, descendants, graphics cleanup and held-telemetry teardown pass;
all **591** recorded native closes finish within **305 ms**. There are zero
recorded fatal CPU/GPU faults, process-protocol errors or native close timeouts.
See [PLAN.md](PLAN.md) for the failures and limits of the outage diagnosis.

This qualification covers top-level GDI layered surfaces. Layered GPU and child
windows are explicitly unsupported. Blur, shadows, DWM thumbnails, animations,
full cursor shape/visibility and frame-pacing qualification remain separate
work. Passing these probes does not establish universal Windows 11 parity.


## DWM surfaces: rounded corners, materials and previews (2026-10-10)

The audit (PLAN.md) found the DWM API storing nothing and the compositor
drawing only square opaque or alpha surfaces. The batch keeps every owner where
it was: applications and the shell declare effects through the public DWM API;
Wine's server owns the per-window state and the policy; the compositor draws.

| Effect | Declared by | Server policy | Drawn by |
| --- | --- | --- | --- |
| Rounded corners | `DWMWA_WINDOW_CORNER_PREFERENCE` | Frames (`WS_CAPTION`/`WS_THICKFRAME`) 8 px by default, standard menus 4 px, `ROUND` 8, `ROUNDSMALL` 4; maximized, regioned and layered windows never | Supersampled coverage of both planes; hit-testing unchanged |
| Border | `DWMWA_BORDER_COLOR` | Explicit colour only; default/none draw no extra stroke (Wine draws frames) | 1-pixel ring |
| Shadow | Frames, rounded pop-ups, `CS_DROPSHADOW` | Window shadow for frames and `ROUND`, small for `ROUNDSMALL`/drop-shadow classes | Quadratic falloff from the rounded shape, offset down |
| Mica / Mica Alt | `DWMWA_SYSTEMBACKDROP_TYPE` + frame margins | Glass = client area outside the margins; dark tint from `DWMWA_USE_IMMERSIVE_DARK_MODE` | Retained desktop root blurred once per root image, tinted |
| Acrylic | `DWMSBT_TRANSIENTWINDOW` + margins | As above | Frame already composed beneath, blurred each frame, tinted |
| Thumbnails | `DwmRegisterThumbnail` / `DwmUpdateThumbnailProperties` | Bounded (32) objects owned by the destination HWND; released with it | Source window's retained planes, area-scaled and cached by image sequence |

Material tints, blur parameters and shadow sizes are `appearance.json`
design tokens (`materials`), mirrored in `src/pw_window_effects.h` and checked
by a host test; they are not measured Windows values. Glass interprets alpha
only for windows that select a backdrop, so applications that merely extend
the frame (CEF launchers) keep their opaque pixels. Thumbnail destination
rectangles are client-relative; source rectangles default to the whole window.
Minimized sources are not thumbnailed yet (no restored-size image exists).

uxtheme now implements the documented content-over-glass recipe: buffered
paint honours `BPPF_ERASE`, the excluded rectangle and the blend function;
`BufferedPaintClear`/`SetAlpha` work in either DIB orientation;
`DrawThemeTextEx(DTT_COMPOSITED)` draws premultiplied grayscale coverage into
the top-down DIB. The shell's helpers in `pw_shell_style.h` paint chrome with
it and make standard controls opaque after they paint (alpha-0 GDI pixels with
colour become opaque; transparent black remains glass). The Mihawk taskbar is a
square Mica surface; Start is a rounded bordered acrylic panel; hovering an
application button shows a rounded acrylic preview with the window's live
thumbnail (icon while minimized), and clicking it activates the window.

Evidence and limits: [first focused batch](../evidence/mihawk-effects-focus-2026-10-10-01.json)
found the composited-text DIB orientation error; the
[second](../evidence/mihawk-effects-focus-2026-10-10-02.json) passes 5/5 with every
displayed sample matching its independent reference within one count. The
exact-source [gate04](../evidence/mihawk-effects-gate-2026-10-10-04.json) passes
**61/61 in 1,883.3 s** after two environmental/intermittent failures recorded in
PLAN.md (TLS fixture ports; the known bootstrap refusal). A follow-up keeps
results bit-identical while expanding material a row at a time, skips the blend
for fully transparent or opaque glass, and re-presents the last composition for
pointer-only motion. On the Pro, composition averages about 5 ms with acrylic
Start, the Mica taskbar and a preview (maximum about 17 ms) and 1.2 ms without
effects; gate05 qualifies it. No frame-pacing claim is made. Transitions/animations, blur-behind
transparency, cloaking, iconic/live-preview bitmaps, DWM-drawn dark captions,
inactive Mica fallback, layered GPU/child windows and frame pacing remain
unimplemented or unqualified. No Windows 11 pixel equivalence is claimed.

## Game mode (2026-10-10)

Target (user): every game lives in Mihawk OS with a desktop shortcut, and
starting one makes the desktop step aside so the game runs as if launched
directly. Steps 1 and 2 are qualified; step 3 (unload Explorer and the touch
keyboard for every full-screen and borderless full-screen game) follows.

**Step 1, direct flip.** While a GPU window's frame is exactly what composing
would show, the broker flips it to the display as the launcher does. The
conditions are in `src/pw_window_direct.h`: a complete snapshot; the client
area is the whole screen; a screen-sized frame; no rounding, border, glass,
region or thumbnail; clips covering the screen; nothing visible above it,
shadows included. Nothing is copied into the scene or composed, and the
compositor thread does not look at the desktop while the game keeps
presenting. Other windows' frames update the scene behind it. A quiet game is
re-checked every 100 ms. If game mode ends without a new frame, the window's
image in the scene is read back from the display's last flip, so composing
never shows a frame from before game mode. That image includes the pointer if
one was drawn. The host test checks every "yes" against the compositor itself
(composing the same scene gives the frame exactly), and each refusal on the
composed screen.

**Step 2, idle shell** (Wine patch 0995). The Mihawk taskbar follows window
events (foreground, show/hide, minimize/restore, title; out-of-context
WinEvents) instead of rescanning every window each second. It repaints only
buttons that changed, and its clock wakes at each minute. Explorer starts
`tabtip.exe` only where it has work (upstream's own environment condition).

**Measured** with `tools/run-game-routes.py`, a .NET (MonoGame) game idle at its title
screen, full-screen 1920x1080, about 85 s per route:

| Route | fps | worst p99 | max | over 20 ms | Explorer server calls / 10 s | extra system memory |
| --- | --- | --- | --- | --- | --- | --- |
| Launcher (direct) | 59.9 | 25.0 ms | 27.0 ms | 0.9 % | - | - |
| Desktop before (composed) | 58.8 | 33.5 ms | 40.3 ms | 3.2 % | 1,457 | +82 MiB |
| Desktop, game mode | 59.8 | 23.0 ms | 25.0 ms | 0.9 % | 41 | +47 MiB |

All 4,883 desktop frames went directly. The 2026-10-10 comparison (record removed from the tree; in git history)
says not slower. Explorer (38 MiB) and the old `mihawk.exe` (9 MiB) stayed
resident until step 3, below. Explorer still wakes about once a second (about
4 server calls) from a source not yet identified. A paused full-screen
game shows pointer motion only when it presents again. Windowed games, a game
that is not exactly screen-sized, and input latency are not qualified.

**Step 3, unload the desktop.** The title runs the session itself
(`src/pw_desktop_session.h`, host-tested): no Windows `mihawk.exe`; Explorer
is the session root. After 2 s of unbroken game mode (graphics patch 0007
publishes the game-mode process without the display lock) the title ends
Explorer's process; the session goes on. Explorer starts again 1.5 s after
game mode ends, or at once when the game's process is gone, and its taskbar
adopts the windows already open. A stop with Explorer unloaded ends the session
once the game has closed. Explorer is unloaded, never frozen: a frozen Explorer
would hang any game that broadcasts a window message.

The first attempt failed: a restarted Explorer exited at once, because the
first Explorer had created the desktop window itself and Wine refuses a second
one (step3-diag-2026-10-10-01, "aborted by WM_NCCREATE"). Windows separates
them, and so does the session now: the system owns the desktop window (the
PS5 server creates it with no thread), and Explorer's desktop is Progman, a
bottom-most window that paints the wallpaper and launchers and is the shell
window (patch 0996). The server keeps the shell window bottom-most
(`HWND_BOTTOM` lands just above it) and marks it as the snapshot's desktop
background for the compositor and Mica (patch 0997). The taskbar follows
window creation and destruction through window events, since parent
notifications go to the system's desktop window. Probes log off through the
shell window, as Windows' Start does; `WM_CLOSE` to the desktop window does
nothing on Windows either.

Measured: `tests/game-mode-probe.c` (`game-mode64` OpenGL and
`game-mode-dxvk64` Direct3D 11, both in the gate) goes full-screen, sees
the taskbar go, checks six capture samples (corners, centre, the taskbar's
place) and the pointer sample against its frame, leaves, and sees the taskbar
come back with the
button of a window that was already open. The same game in game mode from the
desktop: 59.7 fps, worst p99 25.0 ms, against the launcher's 59.7 fps and
25.25 ms; at the end of the run the console's system memory is 251 MiB with 9
helpers, against the launcher's 256 MiB with 9
(2026-10-10; record removed from the tree; in git history).

