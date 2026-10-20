> Historical snapshot of `docs/PLAN.md`, archived on 2026-10-07 from `9b07ebb`.
> Defaults, next steps, counts and failure classifications below belong to their original runs.
> Use the [current document](../../../PLAN.md) for present guidance; see the [archive index](../README.md).

# Plan

What is being built, in what order, how each step is proved, and what is still open.
Nothing here is claimed as working until the *Proved by* column has a console run behind
it.

## The shape of the thing

```text
launcher title (this repo, PS5 Vulkan Template foundation)
  ImGui menu over a Vulkan/RADV display, game list from /data, DualSense
  choosing a game  -> sceSystemServiceLoadExec(title, game args)     one game per process
  closing a game   -> the same call, back into the launcher
game process (same title, Wine loaded)
  ntdll.prx / win32u.prx / wineserver.prx   Wine's Unix side, native PS5 (PRX, in-process server)
  PE x86_64 modules + the game's PE64 exe   Wine's Windows side, run natively (no translator)
  d3d9/10/11 = DXVK, d3d12 = VKD3D-Proton   -> winevulkan -> RADV -> VideoOut
  pad = XInput patch (DualSense), audio = wineps5 driver, files = /data, /app0
```

## Decisions

**Payload application memory, 2026-10-07:** helpers use Sony's
`sceKernelAllocateDirectMemoryForApp` through a callback into Wine's existing
direct-memory mapper. They retain budget 0 and their existing payload lifecycle;
no kernel budget patch is needed. The reported 11,712 MiB pool is shared by the
title, graphics and all helpers. Two Windows helpers held 2 GiB each simultaneously
and repeated the allocation, readback, zero-filled recommit and release cycle;
their peak direct backing was 2,075 MiB each with 14 MiB of system residency.
Windows executable protection and sparse 16 GiB reservations also passed.
See [WINDOWS_PROCESSES.md](WINDOWS_PROCESSES.md#application-memory-in-payload-helpers-2026-10-07).
All nine mandatory gate cases passed on build `cf48858aa8d43bcd`. Full shell
closure with a live 2 GiB graphics helper left no recorded native helper alive
and no CPU/GPU fault. A normal game launcher run stayed up for its 120-second budget,
closed on request with status 0 and peaked at 721 MiB of system memory, while
the .NET game rendered a world with 1,024 MiB of direct backing and a 246 MiB system
peak. These are bounded lifecycle results. The game launcher's unusual UI, also reported
on the person's Wine host, and a nonfatal `wbemprox.dll` protection warning
still require qualification; complete game launcher operation is not claimed.

**Shared graphics ownership, 2026-10-06:** PS5_Vulkan now owns the optional
GPU broker runtime and PE64 WGL/Zink build; PS5_Mesa owns the ordered RADV
broker overlays and Zink presentation fix. Proton consumes pinned exports and
keeps Wine/process/DLL integration. This is a foundation extraction with
build/host validation, not a new console pass. See
[GRAPHICS_PROCESS_BROKER.md](GRAPHICS_PROCESS_BROKER.md#foundation-extraction-2026-10-06).

**Current acceptance work, 2026-10-06:** payload spawning is merged and the
production default. The FNA game's OpenGL build, the FNA game's DirectX build and full game launcher operation
remain unproved. Both supplied FNA game installations require x86 execution and
contain x86 native dependencies; the PE64-only runtime needs a legitimate
x64-capable installation to test them. No CLR-header edit or game-specific
workaround is used.

The shared executable-protection fix removed the initial game launcher TLS write fault
and let Mesa WGL/Zink pass the offscreen vertex/shader probe, reporting OpenGL
4.6 on PS5 RADV. Visible presentation subsequently passed through Zink and the
broker (`zink-present-2026-10-06-05`). The first .NET game runs faulted because
persistently mapped descriptor buffers were device-local; the pinned Mesa fix
keeps them host-visible. Mesa WGL/Zink and helper audio now default on in the
owner/broker layout. With no graphics or audio override, the .NET game reached its
title screen, and the person at the console heard music
(`manual-default-audio-2026-10-06-02`). The loading screen stayed up for 16.2 s
until the game's presenter opened VideoOut, then released its memory. The person
also confirmed loading, the game menu and music through a subsequent manual
menu launch. These
are the .NET game and focused probe results, not FNA game or game launcher acceptance. See
[WINDOWS_PROCESSES.md](WINDOWS_PROCESSES.md#manual-launch-defaults-2026-10-06).

The game launcher's browser repeatedly faults in `chrome_elf.dll` initialization. The
third run observed 34 payload spawns, then FTP/control commands stopped
answering during the close/terminate sequence. Ports still accepted connections;
neither a kernel panic nor a shutdown was confirmed. Native cleanup could not be
verified. Console tests must resume after a real restart and service restoration;
the drop guard must not be cleared merely because a TCP connection succeeds.
See [WINDOWS_PROCESSES.md](WINDOWS_PROCESSES.md#shared-memory-and-opengl-qualification-2026-10-06)
for the evidence and the next tests.
Subsequent local work built anonymous backing for fresh payload reservations
and patch 0936 for zeroed partial-page recommits. The final memory probe passes
on ordinary 4 KiB and simulated 16 KiB host bookkeeping, preserving live
neighbors. These changes are staged but await console qualification. Normal
Game launcher acceptance must also clear the installed profile's existing sandbox and
bootstrap bypass arguments; the generic one-shot argument override now supports
an explicitly empty command line.

The dedicated payload-process experiment now executes real PE64 children through
production's packet bootstrap; see [WINDOWS_PROCESSES.md](WINDOWS_PROCESSES.md#production-bootstrap-through-a-payload-rendezvous-2026-10-06).
`payload-eight-02` held eight Windows children alive simultaneously and confirmed
all ten native payload PIDs absent afterward. `payload-descendants-02` passed 12
descendant nodes across four rounds; `payload-owner-close-02` reaped all ten native
payloads when the headless owner replaced itself with eight children alive.
The route requires resident websrv/elfldr and is now the production default.
`WINE_PS5_PAYLOAD_CHILDREN=0` retains Sony spawning for diagnostics. Every
executable defaults to the owner/broker layout, independent of its name.
Its process/GPU/frame tables hold 32 entries; this is
not yet a 32-Windows-child capacity claim. The final RX loader passed `payload-graphics-10`: two DXVK children and two
rendering descendants, 96 submissions, 27 display flips, correct pixels and input,
6/6 native PIDs absent, no GPU wedge/fault. The legacy Sony route also passed.
Every final run restored signed production and prior synthetic files; the restored
production launcher passed. These are synthetic workload proofs; the eight-child
and graphics tests ran separately.


Native child-process investigation and the 2026-10-04 panic/recovery evidence are
recorded in [CHILD_PROCESSES.md](CHILD_PROCESSES.md). The ordinary-libkernel
`rfork_thread` binding exists, but the bare elevated-title spawn caused a kernel
panic; that direct probe remains disabled.

**Sony-managed headless native children now work in the tested setup.**
[SERVICE_SPAWN.md](SERVICE_SPAWN.md) records `service-native-07`, `-08` and `-09`:
fresh executable, identity/native-PID handshake over transferred descriptor 3,
completion message, service close status zero, child absent afterward,
12/12 parent descriptors, zero fatal signals and normal parent shell exit.
`service-native-missing-01` returned `0x80020002` for a synthetic missing image,
with the parent still listed, one list entry before/after and 12/12 descriptors.
Signed production was restored after each run. No new spawning payload was used.
The final restored-launcher check rendered eight library entries and 120 frames
at 59.9 FPS, then closed normally with zero fault or driver-error records
(`service-native-restored-02`).

The service request needs an explicit headless preload policy; zero caused
VideoOut startup failure even in a kernel-only helper. The tested mask is
`0x8000000000000002` at public options `0x18`, with a matching helper image
preload pointer. The need for both controls has not been isolated. Public `0x8`
is crash reporting; Sony supplies the source PID from its IPC caller.

**Experimental headless PE64 Windows children now pass focused lifecycle checks.**
[WINDOWS_PROCESSES.md](WINDOWS_PROCESSES.md) records the shared-wineserver
bootstrap, inherited pipe/event and non-inheritable handle checks, quoted
arguments/environment, full exit codes, suspended creation/resume, termination,
four cycles and a child continuing after its Windows parent exits. The native
host stays alive to serve that child; Sony still closes children when the entire
application closes. Native parent-application closure and four-cycle 1 MiB
allocation reuse also passed. The integration requires `WINE_PS5_CHILDREN=1`;
production defaults and the installed signed runtime remain restored.

**Headless descendants now spawn through the owning title.** The additional
Windows-environment opt-in `WINE_PS5_DESCENDANTS=1` routes new server sockets
back to the original native title, while Wine preserves immediate Windows
parent relationships. `wine-descendants-chain-01`,
`wine-descendants-parent-exit-01` and `wine-descendants-repeat-01` passed three
generations, spawning after an immediate parent exits, and four repeated chains
(12 node verdicts, eight broker requests, all 13 native children deleted).
Repeated Windows handles balanced at 12/12 then 14/14; native descriptor counts
before each first-generation launch stayed at 100. Each run restored production
and captured zero native fatal/GPU-fault records. All eight corresponding host
Wine cases passed. [WINDOWS_PROCESSES.md](WINDOWS_PROCESSES.md) records hashes,
gating and remaining failure-path limits. Child graphics and broader process
conformance are the next boundaries.

**Java ProcessBuilder descendants pass; a Windows graphics child now renders
using the original title's GPU context.** The earlier JVM runs remain headless
`-Xint`, 64 MiB evidence. Direct GPU initialization in a Sony-managed helper
still fails: AGCDriver cannot open GPU VA space (`0x804c0003`), and a transferred
owner GPU descriptor's query returns EPERM in the helper despite succeeding in
the owner. No new child GPU VM or kernel patch has been established.

The experimental arrangement starts only wineserver in the original title,
runs the initial Windows launcher in a headless helper, and assigns its designated
Windows graphics child to the original title's unused Wine client. Wine retains
the real Windows parent, startup parameters, handles and separate address spaces.
`graphics-owner-render-05` and `-06` each presented 180 DXVK/Vulkan frames in
about 3.0 s, matched the triangle readback, handed off the loading-screen display,
spawned a separate headless grandchild while the GPU device was live, explicitly
tore down graphics and exited cleanly. A second graphics request returned
ERROR_BUSY; parent handles balanced at 14/14 and all three helpers were deleted.
Both restored production, with no fatal/GPU-fault records. One graphics client
per fresh title session is the current limit; concurrent graphics processes,
OpenGL/JVM graphics, audio/input and the Java launcher → Java → the Java game remain unproved.
Keep the direct-launch workaround until that real chain passes.
[WINDOWS_PROCESSES.md](WINDOWS_PROCESSES.md#graphics-child-in-the-original-title-2026-10-04)
records the implementation, failures, limits and reproducible probes.

**A multi-client GPU broker now has console evidence for shared memory and
submission.** Two separate Sony-managed helpers each prepared three GPU
submissions and observed their own GPU-written values through the original
title's context. A malformed packet was rejected before submission. The
larger run used two 32 MiB shared allocations in RADV's shader-address window,
including GPU writes near their ends. Both aliases and services were removed,
descriptors balanced at 12/12 and direct memory returned to the warmed
baseline, with zero fault records. Two separate native children now also create/recreate their own Vulkan
devices and pass four complete 64×64 image-clear/readback checks. Descriptors
and both direct/flexible memory counters return exactly to baseline, with zero
fault records (`graphics-child-gpu-broker-vulkan-03`). A second run (`graphics-child-gpu-broker-draw-01`) passed four shader-pipeline
triangle draws and full-image pixel checks in two separate native processes,
including device recreation and balanced cleanup. Owner presentation now also passes:
four child images, four confirmed display flips and complete scanout pixel
checks, with descriptors 14/14 after display initialization and unchanged
direct/flexible memory (`graphics-child-gpu-broker-present-03`). The opt-in
Wine/DXVK broker now also passes two rendering children and two rendering
descendants, 32 confirmed flips, 16 sampled scanout captures and four guest
readbacks (`graphics-child-gpu-broker-dxvk-descendants-06`). Windows parentage,
inherited pipes, exit codes, explicit GPU teardown and production restoration
pass. This uses 640×360 synthetic renderers, a CPU-copy presenter and a bounded
flexible-memory allocator. Game-scale GPU backing, input/audio routing, other
APIs and compatibility testing are next. [GRAPHICS_PROCESS_BROKER.md](GRAPHICS_PROCESS_BROKER.md)
records the rejected direct-memory route, successful descriptor route and
the sequence for removing the single graphics-client restriction.

| # | Decision | State |
| --- | --- | --- |
| D1 | **64-bit first, 32-bit deferred — not because Proton cannot do it.** Proton runs 32-bit games on Linux because the kernel offers the CPU's 32-bit compatibility mode; the console refuses that mode from a title (prospero-win's build doc: Wine's stock `wow64cpu.dll` "needs 32-bit compatibility mode, which the console refuses"). A 32-bit Windows program therefore needs its instructions translated to x86-64 — prospero-win's `wowprospero.dll`, Wine's WoW64 CPU backend — and that is a separate workstream, adoptable from theirs (LGPL-2.1-or-later) rather than written here. It stays out of M2/M3 so the path that needs no translator lands first. | decided |
| D2 | **Derive the PS5 patch series from prospero-win** (LGPL-2.1-or-later, attributed in `NOTICE.md`) rather than re-porting Wine from nothing. | decided, sign-off wanted |
| D3 | **Pin the runtime from my GE-Proton fork** (`mihawk-99/proton-ge-custom`, `GE-Proton11-7`), patches kept in this repository, vendored tree never hand-edited. | decided |
| D4 | **Proton's `bash`/`python` runtime is not ported.** Its effects (environment, prefix, DLL selection) are reimplemented in C as part of the launcher's platform side. | decided |
| D5 | **Licence: LGPL-2.1-or-later** — the same reasoning as prospero-win's `LICENSING.md`. | sign-off wanted |
| D6 | **Launcher on the PS5 Vulkan Template foundation** (base class, tools, harness, ImGui), so the launcher is testable with the same run/verdict tooling as my other titles. | decided |
| D7 | **Title id**: `PPSA99170`. | decided |
| D8 | **The remote is private.** On 2026-10-02 the commits on GitHub were removed at my instruction and work stayed local. On 2026-10-04 I allowed pushes to the repository, which is private: `main` was pushed as a fast-forward after the history was scanned and one host baseline's captured environment (a session token, account ids, home paths) was redacted. Before a push: the repository is still private, the push is a fast-forward, and no console address, host environment or token is in the commits. Tags, releases and making anything public stay my decision. `AGENTS.md` rule 10 still says never push and is mine to change. | decided, revised 2026-10-04 |

## Milestones

| # | What | Proved by |
| --- | --- | --- |
| M1 | **Launcher title.** Foundation from the template, ImGui menu listing games found under `/data/ps5proton/games`, DualSense navigation, per-game `sceSystemServiceLoadExec` restart, and back on exit. No Wine. **Done: proven on the console.** | `ps5/tools/run.sh` PASS, the menu picture with the .NET game listed (2026-10-02), klog: `elevation: status=0`, `data: granted`, `launcher: 1 game(s); searched: /data/ps5proton/games (1 entry)`. The restart self-test still wants one run with a pad or the hook file |
| M2 | **Wine's Unix side on PS5, x86_64.** Rebase the PS5 patch series onto the pinned Proton Wine revision, build the PRX modules and the PE x86_64 runtime, run a trivial 64-bit PE (Wine's own echo/`cmd`, or a mingw-built hello) and get its output in klog. | **Passed:** `klog/wine-probe-02.log`, PE64 thread/event PASS, exit code 0, launcher restart and normal shell exit |
| M3 | **Direct3D on RADV.** DXVK `d3d11`/`dxgi` in the prefix and a licence-clean 64-bit test app on screen (MIT-licensed DirectX-Graphics-Samples for D3D12/VKD3D-Proton; Wine's own d3d11 tests for correctness). | **Passed:** `klog/d3d-probe-01.log`, 180 frames, exit code 0; `klog/d3d-probe-01.png` is a verified 1280×720 GPU readback of the RGB triangle. DXVK reports missing EDID metadata and uses defaults. |
| M4 | **A real 64-bit game**: the .NET game, end to end — prefix created on first run, shader caches under `/data`, saves, pad, audio. | frame rate and picture recorded; the game's own menu navigable with the pad |
| M5 | **Prefix management and profiles.** Per-game prefixes, per-game environment (DXVK/VKD3D switches, DLL overrides), a `profiles.json` the launcher reads, and a published compatibility table. | two games with different profiles run from the same install |

## M2: runtime integration in progress

The active series has **81 patches** (54 when the rebase finished on 2026-10-02; the rest came with the console runs since, seven of them checked on the host only so far), replayed from Proton Wine
`1c78596625336a73dbfc035dbbcf0dbdd95b06eb`. The original OpenGL patches remain in
`wine/deferred-opengl/` as reference; `0820` rebases the EGL integration onto
Proton and `0830` adds opt-in presentation capture. Diagnostic-only `0690` remains
deferred because its thread-data assumptions differ from Proton.

`0750`–`0770` restore non-Linux compilation: Linux synchronization APIs and glibc
options are guarded, GS setup follows the host platform, the no-EGL build keeps
its fallback types, and the PS5 registry calls use Proton's pointer interface.
The rebase also preserves Proton's Vulkan client-surface ownership in `0460`.

The runtime builders now live in `tools/`: `build-wine-runtime.sh` builds PE64
modules and host Wine; `build-wine-ps5.sh` cross-builds the PRXs and links RADV
with the sibling project's recipe. The PRX converter is pinned by
`fetch-prx-tools.sh`. Source adapters are imported from prospero-win at
`c8a2f00137fb0a95f88541252b4e47f53f9677e6`; see `NOTICE.md`.

The title loads ntdll on a 16 MiB thread before creating any launcher Vulkan
device. Audio uses Proton's mmdevapi main-loop thread, and DualSense state feeds
XInput. A one-shot `wine-run.txt` selects a profile or executable for console
validation. A `stop-game` file under `/data/ps5proton` asks the game to close
(WM_CLOSE to its window, then NtTerminateProcess after 10 s); the game ends
through Wine's exit, which closes its presenter (patch `0892`), and returns to
the launcher. Output stays at 59.94 Hz: the title no longer declares 120 Hz
(`attribute3` 0), so switching between the launcher and a game never changes the
TV's mode (`docs/OPTIMIZATION_LEDGER.md`, R4).

**Host evidence:** PE64 Wine builds; an isolated prefix initializes; the small
`tests/wine-probe.c` executable creates a thread, signals an event and reports
`PS5_PROTON_PE64_PROBE PASS`. PRX parsing, Wine startup, argument splitting and
mixing tests pass. The title and all 12 PRXs cross-build, with zero unresolved imports or detected
raw syscalls. **M2 is now proven on the console.** `klog/wine-probe-02.log` records the
PE64 thread/event PASS marker, exit code 0, restart to launcher and normal shell
exit. The first run lost its last log lines during LoadExec; a 500 ms log-drain
period preserves them. The launcher restart drops the cycle argument on this
console (observed argc 1); returning to the launcher still works.

**M3 evidence:** the D3D11 probe rendered 180 frames on the console and saved
a 1280×720 readback. Inspection shows the expected RGB triangle, with center
RGB `(127,64,64)` and background `(6,13,26)`. It exited with code 0 and returned
through the launcher. DXVK reports missing EDID metadata and uses defaults.

**Further console evidence:** `wine-probe-04.log` passes fixed-address reserve /
commit / JIT protection changes and a handled access violation preserving MXCSR,
XMM15 and YMM15. `0800` corrects the PS5 signal frame's XSAVE offset (320 bytes).
`d3d-probe-03.log` presents 180 frames; `d3d-swapchain-03.png` verifies aspect-fit
scaling and color in the actual 3840×2160 presentation image.

**M4 in progress:** the installed game uses DesktopGL, verified from its framework
API markers and the `MonoGame.OpenGL.GL.LoadExtensions` failure in run 05. The
previous WindowsDX assumption was incorrect. Fixed-address direct-memory backing
allows CLR startup; `DOTNET_SYSTEM_GLOBALIZATION_USENLS=1` avoids missing ICU.
Run 06 reached a reservation tracking limit. Tracking now rounds to PS5 pages,
handles 4096 disjoint regions, and merges adjacent ranges even at capacity;
4096-region and 20000-operation host regressions pass. Run 07 launched the title screen and complete menu at 1920×1080, near 5 FPS.
`0840` switches the final format-converting copy to Wine's existing GPU quad;
run 08 then sustained about 60 FPS for 90 seconds with a visually correct menu
(a run 08 capture in `klog/`). `0850` adds the missing PE audio driver shell; once
installed into the preserved prefix, run 09 submitted non-silent 48 kHz stereo
and retained 60 FPS. `0860` fixes an off-by-one in extensionless Unix-library
names which truncated `xinput1_3` to `xinput1_`. The console XInput probe now
finds XInput slot 0 and correctly reports slot 1 absent. This proves the
API bridge, not game recognition: that build forced the connected flag. The full PE64
regression passes again (`wine-probe-05.log`). Run 10 retained 60 FPS and non-silent audio until the shell closed it after
42.7 seconds; the harness reports an external exit rather than a normal test end.
I found that the game did not detect the controller and asked for
Windows DualSense identification plus touchpad mouse movement and left-click.
`0870` now starts Wine's existing HID/PnP stack in the game process and feeds
DualSense USB reports from the native pad. The console HID probe passes
(`klog/hid-probe-05-wine.log`): VID:PID `054c:0ce6`, usage `1:5`, parsed 64-byte
input report, live stick values `131,127`. Wine normalizes the product string to
`Wireless Controller`; SDL identifies this VID:PID as DualSense. Touchpad motion
and press/release events feed Wine's existing mouse queue, and connection state
comes from native samples. Host checks cover report encoding, finger changes and
disconnect release. I confirmed controller navigation, touchpad movement and touchpad-click
behavior during the input run. Its inspected 1920×1080 capture shows the
character-creation screen; presentation holds about 60 FPS, and all 1743 posted
mouse events were delivered with zero dropped. The 96.8-second harness run
returned through the launcher and exited normally. SDL uses HID for this profile
(`SDL_XINPUT_ENABLED=0`) to avoid exposing the bridge as a second controller.
A manual launcher test then exposed a separate restart-argument issue: the
restarted title parsed no profile. The launcher now saves an atomic, flushed
one-shot request before LoadExec; startup consumes it whether argv survives or
not. `launcher-cross-01.log` records Cross (`reason=chosen`), profile 1 restored,
and 302 rendered frames in 5012 ms. I confirmed that Cross launches the
game. The game was left running after this manual check.

I then heard choppy sound. Audio run 01 found that AudioOut returns
the accepted frame count (`256`), while the Wine sink expects zero on success.
Every successful block therefore entered the error-delay path: 1875 false
failures per ten seconds, with 10 seconds of audio taking about 10.23 seconds
to submit. The native callback now maps nonnegative AudioOut results to zero.
Run 02 records zero false failures, an average blocking submission of 5.316 ms,
and zero missing mixer frames in the three measured intervals after startup.
Presentation remains near 60 FPS (293–302 frames per roughly 5 seconds).
The first interval had four short grains; a later interval had one 100 ms
scheduling gap, so these counters do not establish completely clean sound.
The run ended externally after 52.8 seconds rather than through the planned
60-second stop; no crash signature was recorded. I subsequently
confirmed that the audio is now very smooth. Low-rate `ps5audio` diagnostics retain format, starvation and timing
measurements for follow-up. The host format/resampling/mixing tests pass.

`wine-probe-06-wine.log` passed all PE64 assertions but stalled in its
immediate ExitProcess while HID initialization was still logging. The cause
(2026-10-03, `docs/OPTIMIZATION_LEDGER.md` R1–R2): ExitProcess kills the other
threads with SIGQUIT, Wine ended them wherever they were, and the PnP thread
died holding the title's output-sink mutex. `0885` now ends a thread killed
inside the Unix side at its return to user mode, as Windows does, and the sink
shares no lock; `exit-probe.exe` returns in 2–3 s, 3 of 3. `0886` makes
registry saves durable (a power loss had left a prefix with empty `.reg`
files) and has the title save before every LoadExec.
Actual gameplay and saves are not yet independently verified; audible output
is confirmed by my listening test above.
Prefix deployment preserves existing game state and adds the runtime-owned
`wineps5.drv` only when that new loader file is missing. DNS resolution remains unavailable
and 32-bit is out of scope.

## What "Proton" means here, piece by piece

Proton is Wine *plus* a stack and a runtime. Every piece is accounted for, so this is
Proton on the console and not a Wine port wearing the name:

| Proton piece | Here |
| --- | --- |
| Valve's Wine (Proton's tree) | **kept**: pinned at `ValveSoftware/wine` `1c78596…`, GE-Proton11-7's revision — the PS5 patch series is rebased *onto Proton's Wine*, not onto vanilla Wine |
| wine-staging | **kept**, pinned `6cc805ea…` |
| GE-Proton's own patch groups (17: game patches, DXVK patches, video rework, low-latency layer, discordrpc…) | **kept**: they live in the pinned fork and travel with it |
| DXVK — `d3d9`, `d3d10core`, `d3d11`, `dxgi` | **kept**: built for the console's PE side and staged into the prefix (M3) |
| VKD3D-Proton — `d3d12` | **kept** (M3/M4) |
| Proton's prefix shape: the DLL set, the registry, `drive_c`, `dosdevices` | **reimplemented in C** (M4) — the launcher creates it; same shape, so a profile is portable between the two |
| The `proton` **bash** runtime: verbs, environment | **reimplemented in C** (M4). The console has no shell, so the script cannot run; its *effects* keep their names — `PROTON_*`, `DXVK_*`, `VKDLLOVERRIDES`, `WINEDLLOVERRIDES` |
| `protonfixes` **Python** | **reimplemented as data** (M4): per-game profile files the launcher reads, instead of a Python module tree |
| esync / fsync | kept **where the console's kernel allows** (M4, after a probe: they need eventfd-like and futex primitives) |
| DXVK state cache, RADV shader cache, VKD3D pipeline caches | **kept**: per-game directories under `/data` (M4) |
| The launcher's Linux runtime container (soldier/sniper) | **out**: the console's platform layer is what Proton's container provides on Linux |
| User-supplied Windows game launcher client | **compatibility target**: payload processes and brokered graphics; never bundled. Full client operation and launching library games remain unproved. |

The one honest rewrite is the runtime *scripts* — a shell script and a Python package.
Everything that makes Proton Proton (the Wine tree, its patches, the D3D translation
layers, the prefix, the per-game fixes) is either pinned from GE-Proton or reimplemented
with the same semantics.

## The first real game, as it sits on the console (2026-10-02)

Audited over FTP: a 64-bit .NET 6 MonoGame game (PE machine `0x8664`, runs natively under Wine's x86_64 PE modules,
no translator) with its self-contained runtime beside it (`hostfxr.dll`, `coreclr.dll`: coreclr's JIT needs
executable memory, the platform layer's `exec.h` route), SDL2 for windowing/input and OpenAL Soft for audio; graphics
MonoGame **DesktopGL** → SDL/WGL → Wine EGL → PS5 OpenGL SDK (verified by framework API markers and run 05); saves
and config under `%AppData%`, so they live in the prefix (M4). The launcher checks that a `Z:` executable is really
there, so a `.exe.exe` typo in its `game.json` showed on screen instead of surfacing in M4.

## 32-bit games, precisely

| | |
| --- | --- |
| Proton on Linux / a handheld PC | runs 32-bit games: the kernel provides the 32-bit compatibility mode, and on ARM hosts Proton uses FEX as the CPU backend |
| Windows 32-bit program on this console | The console refuses 32-bit compatibility mode to a title, so the CPU cannot execute the code directly |
| What that requires | A CPU backend that produces x86-64 code: `wowprospero.dll`, prospero-win's IA-32 translator, selected by the prefix's `HKLM\Software\Microsoft\Wow64\x86` |
| What this project does about it | nothing yet, by decision D1: M2/M3 are 64-bit. Adopting `wine/wowprospero` the way the patch series is being adopted is the later workstream, and it brings its own milestones (the DBT, the prefix key, its accuracy and speed story) |
| What it means for a game choice | A 32-bit game is not a dead end, it is a *different* workstream — and if it is also OpenGL, it needs the GL path too, which is where prospero-win's roadmap already is |

## Where a game goes (the /data contract)

A title starts able to see only its own sandbox: **`/data` does not exist for it**. The
FTP server sees `/data` regardless, because it runs outside the sandbox — a folder that
looks correct over FTP can be invisible to the title. Two mechanisms exist in this
ecosystem, and the first is the one this console uses:

The title asks the **Lapy owned-root daemon** (PS5-Lapy-JB-Daemon, mpereiraesaa), through the platform
layer's `ps5_elevation_request` (PS5_PayloadSDK, `platform/docs/ELEVATION.md`; derived from the client
ps5-native-app-boilerplate adopted in its pull request 4). It makes the process's credentials the shape the
daemon needs (`seteuid(geteuid())`), renames a complete `{"PID":<pid>}` request into
`/download0/elevate_proc`, and polls for `/data` to work with a real write and read, which is the proof;
the request's disappearance is not taken as success. A daemon must be running: either one-shot (built for this
title's id and sent to elfldr before each launch) or resident. The title starts the resident one itself when its
request goes unread for 3 s (since 2026-10-06; `platform/lapy/README.md`, "Normal-use service").
With no daemon the title stops waiting after 3 s (60 polls of 50 ms; the contract's default is 10 s, and 15
recorded grants finished within 0.51 s of the request) and keeps to its own folder; the launcher then says the
library is unavailable, not empty (`docs/ELEVATION_VALIDATION.md`, "Startup time and normal launches"). After success, also
`setegid(getgid())` — elevation leaves the effective group apart from the real one and Mesa turns RADV's
disk cache off for a process whose ids differ.

This replaces two earlier mechanisms, both removed with no fallback: a helper ELF the title streamed into
elfldr (`ps5/src/elevation/`, `sandbox-elevator.elf`), which published root-vnode pointers itself, and a
homegrown copy of prospero-win's request file with a settle delay. Kernel ownership and firmware support now
have one upstream implementation. Upstream documents completed validation on firmware 12.02 only.

**Proven on the console on 2026-01-04, firmware 10.01.** One launch of `exit-probe.exe`: the game instance
and the launcher instance it returns to both logged `elevation: ok (filesystem access)` and `data: granted`,
the launcher then read `/data/ps5proton/games` (8 entries), the probe printed `PS5_PROTON_EXIT_PROBE PASS`,
and the title ended itself with `ends: status 0` — 7.2 s, one launch, no drop (`evidence/lapy-elev-02.json`).
Upstream had validated 12.02 only, so 10.01 was unknown until this run: the daemon's own layout preflight
accepted it and it served the requests. It was not clean: during that run two `payload` processes — the
daemon's donor children, released when the elevated game instance exec'd — faulted in user mode and were
coredumped. That is the signature upstream records for firmware it has not validated (its 6.02 note: the
donor processes faulted in user mode while every cycle was functionally correct), so the elevation works
here, but whether repeating it is safe remained open, so the launch cap was retained at that stage
(removed at the user's request on 2026-10-04; see the launcher section below). Two things made even that
possible. The request is now made *before*
`platform_init`, because the daemon refuses a target with more than one thread
(`private_process_credential` in `owned_root_daemon.c`) and the klog reader thread starts there. And the
daemon is built with the payload SDK its own code matches.

How I run the daemon, until its log channel works from this host:

1. Build it separately from the title with the SDK the daemon's code expects — **v0.40**, the last release
   whose `kernel_get_ucred_attrs` still returns `uint64_t` (v0.41 changed the prototype):
   `PS5_PAYLOAD_SDK=<sdk-v0.40> python3 tools/build_owned_daemon.py --logging-client <ps5log> --service
   --max-requests 4`. The bound is one launch's worth of requests (launcher, game, return to launcher).
2. Send it with websrv's payload launcher, which needs no listener on this host:
   `curl -F elf=@lapy-root-daemon.elf -F args= -F pipe=1 http://<console>:8080/elfldr`.
3. A daemon without its ps5log channel exits 2 before doing anything (non-helper builds), and this host is
   not reachable from the console today — a dial to a listener here times out from the console's side. So
   `/data/homebrew/dev.conf` names a listener on the console itself (`DEV_SERVER=127.0.0.1`,
   `DEV_PORT=8080`) purely to satisfy that gate. The elevation is unaffected; the daemon's own
   `daemon_start`, `target_state` and `target_preflight` events are lost with it, so the root reference
   balance on 10.01 is still unmeasured.

Once granted, `/app0` is *not* visible any more; the title's own folder is the sandbox's
view of it (`PROTON_SANDBOX_APP0`), which `proton_app_root()` and `proton_title_path()`
resolve. So the catalog is read from both places a game may be, and the launcher says
what it searched and what each root held:

| Root | Needs the daemon |
| --- | --- |
| `/data/ps5proton/games/<id>/game.json` | yes (the library, and where a prefix belongs) |
| `<title folder>/games/<id>/game.json`, i.e. `/data/homebrew/PPSA99170/games` over FTP | no |

## What the console proof took (2026-10-02)

Four things had to be right, and the run's klog named every one of them as it failed:

| Symptom in klog | Cause | Fix |
| --- | --- | --- |
| `data: not granted`, `elevation: timeout`, `0 game(s)` | no Lapy daemon was running when the title asked | run the daemon (one-shot for this title's id, or resident), then launch |
| A crash in the base constructor, `exit(-1)` | elevation removes `/app0`, so the foundation's asset-path check failed and called `exit(-1)` — which a title must never call (rule 1) | The assets and shaders now live in the title's install folder (`/data/homebrew/PPSA99170`), and the foundation reports a missing asset path through `exitFatal` (throws) instead of exiting |
| `Failed to create /app0 for shader cache ... disabling` | RADV's disk cache defaults to `/app0`, which is gone after elevation | `MESA_SHADER_CACHE_DIR` points at the title's folder, set before the instance exists |

Two carry-overs for the projects that own them: the `exit(-1)` in the foundation belongs
in `PS5_VulkanTemplate` (this repository's copy has it, the template's does not yet). A one-shot
Lapy daemon is title-specific, so every title needs its own built copy; a resident daemon serves them all.

## M1, as built

`ps5/src/proton_launch.{h,cpp}` is the launch contract (prospero-win's: `profile=`,
`path=`, `cycle=`; no arguments is the launcher), the game catalog read from
`/data/ps5proton/games/<id>/game.json`, the restart through `sceSystemServiceLoadExec`
with the title's own eboot (`/app0/eboot.bin`, then the sandbox path), and the
self-test hook. `examples/ps5proton/ps5proton.cpp` is the launcher: the game list in a
scrolling child with the navigation pin PS5 Vulkan Samples' menu needed, an empty state
that says where games go, and the screen a game shows before the runtime exists.

The restart is proved without a pad: with `/app0/restart-check` present, the launcher
starts the first game, the game process notes itself and restarts back, and the launcher
removes the hook after the round trip, writing each step to `/app0/restart-check.log`.

## The first slice (M1) in detail

1. Generate the title from `PS5_VulkanTemplate` (`ps5/tools/new-title.py`) with the id
   from D7 and the foundation copied from its last commit.
2. Replace the starter program with the launcher: a game list built by scanning
   `/data/ps5proton/games/*/game.json` (name, executable, arguments, profile), an ImGui
   menu with the same interaction contract as PS5 Vulkan Samples (D-pad, CROSS, OPTIONS
   to quit), and a "no games installed" empty state that says where to put them.
3. Choosing a game calls `sceSystemServiceLoadExec` with the title's own path and the
   game's arguments; the game branch of `main` loads the runtime (still a stub in M1),
   runs it, and returns to the launcher.
4. Prove: `run.sh launcher` (menu) PASS, then `run.sh --menu` by hand with the pad, and
   one restart cycle in klog.

## Notes for whoever removes commits from a repository again

- `git push --force <base-commit>:refs/heads/main` rewrites the remote branch and leaves
  the local history alone: the local repository keeps everything, the remote keeps only
  the base commit.
- GitHub refuses to delete the *current* (default) branch — `git push origin --delete main`
  answers `refusing to delete the current branch`. An online repository with no commits
  at all means moving the default branch first, or deleting and recreating the repository.

## Console notes from the M1 install

- **The old control payload wedged again.** Port 9111 accepts a connection and never answers
  (`ps5_console.py payload` times out), a freshly loaded `ps5vkctl` fails with "Address
  already in use", and the payload that answered on 9112 earlier is gone. Deployment and
  launches both go through it, so the console run waits for a working payload: either free
  9111 (the wedged instance is from before this session) or load a fresh one on a port
  nothing holds, and pass `PS5VKCTL_PORT` to the tools. This session loaded the
  existing control payload on **9112**; it responds and reports the console idle.
- **A brand-new title id needs its folder made first.** `PS5_Vulkan/tools/deploy-title-folder.py`
  creates directories with absolute `MKD`, which this console's ftpsrv refuses (550), so the
  first `STOR` fails; creating `/data/homebrew/PPSA99170` with a relative `MKD` from inside
  `/data/homebrew` works, after which the tool's absolute paths are fine. Worth a fix in
  PS5_Vulkan: relative `MKD`, and a clearer error than "550 No such file or directory".
- **`SIZE` is unsupported** on this server: it answers `18446744073709551615` instead of an
  error, so the deploy tool's "already there" check can never match and every deploy
  re-uploads files it could have skipped (and `remote_size` does not see `None`).
- **The compiler warns** on the foundation's own headers (`imgui_internal.h`'s memset of a
  non-trivially-copyable type, `json.hpp`'s literal operators). Not ours, no action.

## Risks, and what is done about each

| Risk | Why it bites | Mitigation |
| --- | --- | --- |
| Patch series rebase conflicts | prospero-win patches a pinned Wine (`490f6d5…`, wine-mirror); the Proton tree is Valve's Wine + staging, and both touch `ntdll`, `server`, `win32u`, `winevulkan` | 54 active patches; EGL rebased as 0820, original optional GL patches and one diagnostic patch retained separately. Cross-build and validate each affected console path |
| Wine's Unix side is not the only port | Wine's PE modules must be built for the console's toolchain and staged in the prefix | Build the PE side with the SDK's mingw path, stage under the runtime directory, pin the staging manifest (prospero-win's `WINE_RUNTIME_MANIFEST.schema.json` is the model) |
| VKD3D-Proton needs more Vulkan than DXVK | It wants a modern feature set; the console's RADV is a 1.4 driver with recorded gaps (`PS5_Vulkan/docs/CTS_GAPS.md`) | DXVK first (M3), VKD3D-Proton second; query features and report what is missing rather than fencing it off |
| Proton's runtime is bash+python | No shell, no Python, and no intention of porting them | D4: reimplement the effects in C, keep the semantics written down, and treat every divergence as a documented one |
| `/data` access needs the environment's daemon | The launcher library and the prefixes live in `/data` | Same prerequisite as prospero-win (Lapy JB daemon); the launcher reports its absence clearly instead of failing silently |
| Firmware sensitivity | prospero-win was validated on 12.02 only | Record the firmware with every console result; never claim a firmware it was not run on |
| Anti-cheat and online games | Kernel-level anti-cheat cannot run in this environment | Out of scope, stated in the README; no compatibility claim for such titles |

## Pins

| Part | Pin | Where |
| --- | --- | --- |
| Runtime stack | `mihawk-99/proton-ge-custom`, `GE-Proton11-7` | `tools/fetch-runtime.sh` |
| Wine (Proton's tree) | `ValveSoftware/wine` `1c78596625336a73dbfc035dbbcf0dbdd95b06eb` | `tools/fetch-runtime.sh`, `.deps/wine-proton` |
| wine-staging | `6cc805ea57132eeaf44764e9213823c9b8d0d300` | `tools/fetch-runtime.sh` |
| DXVK | `doitsujin/dxvk` `601930949d111edbbcf9dd463948426d9f8f6ddd` | `tools/fetch-runtime.sh` |
| VKD3D-Proton | `HansKristian-Work/vkd3d-proton` `af89350cc2eacd9da2293fbae96bd9ab4987c9bb` | `tools/fetch-runtime.sh` |
| PS5 patch series base | prospero-win's series against Wine `490f6d5dcbb2a5047345b8af88d114bbcaad69a8`; 49 of 52 patches imported (the three i386 ones are out of scope) | `wine/patches/`, `NOTICE.md` |
| Title foundation | `mihawk-99/PS5_VulkanTemplate` at the commit the title was generated from | `ps5/` (copied foundation) |
| DXVK / VKD3D-Proton | The fork's submodules | `tools/build-*.sh` |

## Scheduling checkpoint (2026-10-03)

Native Windows-thread priority mapping is available through `WINE_PS5_SCHED=1`.
The console priority probe, OpenGL game regression and D3D11 probe are recorded in
`docs/OPTIMIZATION_LEDGER.md` R5. It stays opt-in: PS5 strict priorities lack Windows
starvation boosting. Clean-exit handling from R4 remains enabled.

## Launcher UI refresh (2026-10-04)

I integrated the template's pinned Vulkan UI kit into the launcher: Gloss /
Skeuomorphic by default, thirty themes with previews and persistent selection.
The picker shares the launcher device; selecting a game defers LoadExec until
the launcher UI, audio and Vulkan device have been released. The Wine launch
contract and elevation-before-threads ordering stay in force. The per-boot
launch cap was removed at the user's request on 2026-10-04; cooldown and
stop-after-disconnect checks remain, and launch counts are diagnostic only.
Console rendering is now proved in sandboxed and elevated modes: 300 frames at
59.9 FPS, with the Gloss theme-information panel visually checked. Six additional
lifecycle/probe runs completed 11 elevations and reaped 28 donors, returning root
counters to 55/54 each time. They include two immediate Wine exit/return cycles,
PE64 memory/JIT/thread/signal checks, 180 D3D11 frames with verified readback,
and an XInput presence check. Complete numbered daemon logs and fresh title
identities underpin the results; no captured faults were found. See
[`ELEVATION_VALIDATION.md`](ELEVATION_VALIDATION.md). Manual theme interaction,
UI sounds and selecting a real game through the new UI still need validation.

## Roadmap (2026-10-04)

The resident Lapy daemon is what lets an ordinary launch see `/data`, where the library, the runtime and
prefixes, saves, shader caches, logs and the Java game's JVM and world live. It replaces the legacy elevator that
was the likely cause of the console drops, gives the root references back at each title's exit (every finite
validation run today returned the counters to baseline, and the resident service was at its baseline before
each request), and logs every grant, which is the lifecycle evidence a game session needs. It does not fix
Windows API gaps, memory pressure or the OpenGL exit problem. The title starts it on the first launch after a restart.

| Milestone | State |
| --- | --- |
| M1 launcher | done; the new UI rendered on the console (sandboxed and elevated); manual theme, sound and pad-pick checks pending |
| M2 Wine on PS5 | done |
| M3 Direct3D | D3D11 through DXVK proven by a probe; D3D9, D3D10 and D3D12 not yet proven on the console |
| M4 real game | in progress: the .NET game's menus, character creation, 60 FPS, audio, DualSense and touchpad proven; gameplay and saves not |
| M5 profiles | not started |

### 1. Real-game lifecycle (finishes M4)

In the .NET game: launch from the library, play, make a dedicated test save without touching existing saves, exit,
relaunch and load it; then a 30-60 minute session. Each run captures the daemon's grant records, descriptor
census, direct memory, frame pacing and a hash of the save before and after. A save is proven when the file
changed in the way the game describes (the in-game day advanced) and the next launch loads it.

**Done on 2026-10-04 with nobody at the controller** (`docs/GAME_LIFECYCLE.md`): a pad script drives the game
(`src/pw_pad_script.h`), the player's saves are held aside and checked by hash.
Launched as the library does, the .NET game loaded a test save, the farmer went to bed, the game saved (day 1 -> 2),
closed through WM_CLOSE and returned to the launcher; the relaunch loaded Tue 2. The first runs found three
defects a player meets: five library entries named `C:` paths that do not exist (fixed, and the launcher now
says so), no `AppData\Roaming` in the prefix so the .NET game's saves never appeared in its load menu (the title now
makes the user folders), and an ignored first press on the title menu (the scripts press again).

### 2. Conformance to "clean"

Full conformance is not currently established: focused headless child creation
and descendant chains now pass, while child graphics remain open;
pages have 16 KiB granularity, filesystem links and hidden/system flags are
unavailable in the current title path, and hardware debug registers are unavailable.
The earlier conclusion that child processes are impossible is superseded by
[the native investigation](CHILD_PROCESSES.md) and
[the service lifecycle findings](SERVICE_SPAWN.md). Measured in the title on
firmware 10.01 (2026-10-04, `native/proton_limits_probe.h`): a 4 KiB `mprotect` inside a 16 KiB page returns 0
and protects the whole 16 KiB page; `fork`, `rfork`, `link`, `symlink`, `readlink` and `ptrace` exist only in
`libkernel_sys`, which a title does not load (its imports of them are calls to address 0) and may not load (the
system ends the title, exception 0xa002030a SYSTEM_ILLEGAL_FUNCTION_CALL); and a title may make no raw system
call. PS5-Lapy-JB-Daemon's `rfork(RFPROC|RFFDG)` works because the daemon is a payload: links (and debug
registers through `ptrace`) would require a service outside the current title path.
The daemon's direct clones are payload processes. Sony's local-process service
now starts headless homebrew helpers running Wine;
[the focused Windows lifecycle](WINDOWS_PROCESSES.md) is proved experimentally.
Clean means: every failure
also happens on the pinned Wine on Linux or is a documented console limit, no `OPEN:` defect remains, and every
test file ends with its summary or is skipped for a written reason. As of 2026-10-04 the comparison finds no
unexplained failure in ntdll, kernel32 or kernelbase. What is left:

| Item | Open lines | Next step |
| --- | ---: | --- |
| DOS device links and volume names (the mount manager needs `winedevice.exe`, a child process) | 69 (ntdll and kernel32) | create the DOS device links as server objects in ntdll's Unix side; the mount manager in the title only if programs need volume names (`docs/BOOT_STATE.md`) |
| A loaded builtin DLL's headers differ from the prefix's copy | 50 | The probe prefix's system32 is refreshed on deploy; rerun kernel32 `loader` |
| objects left in the object directory, DLL file names, `\\?\` paths, pipe and mailslot names, CreateProcess's error, heap order, NtReadFile on a directory | 30 | fix each, or rerun alone where a batch effect is suspected |
| test files that do not finish | ntdll: 2 incomplete, `threadpool` skipped; kernel32: 6 by the watchdog or skipped, `fiber` and `virtual` incomplete | `virtual` ends in `test_stack_commit` (guard pages on 16 KiB pages): a console limit to record; `exception` needs a look |
| Direct3D: `d3d10core` | one console attempt ran out of descriptors | patch 0910 passed its thread probe on the console; run in slices |
| Direct3D: `d3d9`, `d3d11`, `d3d12`, VKD3D-Proton's suite | host baselines only (`d3d12`'s one console attempt recorded nothing) | `d3d12` first, then slices; `dxgi` changes display modes and goes last |
| msvcrt, ucrtbase, advapi32, bcrypt, shlwapi, rpcrt4, ws2_32, ole32, oleaut32 | host baselines only | console runs, msvcrt, ucrtbase and ws2_32 first (Java uses them) |

kernelbase is clean: its four failures are 16 KiB pages.

### 3. A Java game

Java is started with the command line a Java launcher builds (the launcher cannot start a child process here). The
per-game runs of 2026-10-03 and 2026-10-04 (records removed from the tree; in git history) found, for every program:

1. **Exit safety.** Twice the console powered off when the title was replaced while a render thread was inside
   OpenGL; since the presenter waits for that thread, a third run hung instead. A game has to end cleanly: a power
   loss has already emptied a prefix's registry once (R4 in OPTIMIZATION_LEDGER.md).
2. **Memory budget.** 12 GiB of direct memory is shared with the GPU. On the OpenGL path the PS5 OpenGL driver held
   9005 MiB in about 1974 live allocations (5834 made, 3860 released), each around 12 MB, from a renderer mod's
   chunk-buffer arena that allocates a larger buffer and frees the old one at every resize; the JVM's native
   allocations then failed 45 s in. The fix belongs to the PS5 OpenGL driver (buffer release and reuse), a sibling
   repository. The JVM's abort ended the process and the title returned to the launcher in 53 s, with no hang.
3. **Vulkan.** The same game drawing through Wine's `vulkan-1.dll` and winevulkan to RADV never touches the OpenGL
   driver: direct memory stayed flat at about 2.6 GiB. A mod loader's early loading window drawn with OpenGL kept the
   display (`sceVideoOutOpen` 0x80290009) until it was turned off. Median 80-84 FPS into new terrain, 113-118 FPS in
   generated areas; the limit was the game's own CPU work (terrain generation, meshing, 345-800 MiB/s of garbage).
4. **120 Hz.** The title declares high frame rate (`param.json` attribute3 0x40) and a game opts in with
   `PROTON_120HZ=1` in its `game.json`, which points RADV at the declaration and tells Wine's display driver (patch
   0924) to report 120 Hz. On the test TV the shell reports 2560x1440 at 119.8801 Hz; frames were paced at 8.5 ms.
5. **CPU count** (OPTIMIZATION_LEDGER R7): a game process runs on 13 CPUs (0-12); with Wine showing 13 instead of
   16: mean 77.6 -> 86.7 FPS while walking, worst 5 s interval 23.5 -> 52.9 FPS, p99 frame 45.6 -> 17.8 ms.

**A loading screen for every game** (2026-10-04, `native/proton_loading.cpp`): until a game's own presenter asks
for the display (`pw_wine_release_display`, from Vulkan's display plane and from OpenGL), the title draws the
game's name, the seconds since it started and a spinner straight through VideoOut, in the launcher's Inter
font; it closes the display (a blank flip, then unregister and close) before the presenter opens it, and before
the title replaces itself when a game ends without presenting. The .NET game: shown 5.8-5.9 s, the OpenGL presenter
took over at once (2026-10-04; records removed from the tree; in git history); the Java game: 19.6 s, then 116-120 FPS on Vulkan; `sync-probe.exe`, which never presents: closed as it ended, the launcher back
(`probe-loading-01`). `PROTON_LOADING_SCREEN=0` leaves it out.

### Where the roadmap stands (end of 2026-10-04)

* **Step 1, done**: the .NET game loads, saves (by sleeping and by passing out), closes, relaunches and loads, and ran 30
  minutes with flat memory and descriptors (`docs/GAME_LIFECYCLE.md`).
* **Step 2, conformance**: ntdll, kernelbase, kernel32, ucrtbase, msvcrt, d3d12 and d3d10core compare with no
  unexplained failure (`docs/CONFORMANCE.md`). Patches 0914-0917 fixed CreateProcess's error, directory reads, and
  BOOLEAN/WORD syscall arguments the console's clang misread (GetLogicalDrives looped forever); wined3d's Vulkan
  renderer is the default (DXCore finds the adapter). Open: the mount manager (70 lines), `\KnownDlls` (11),
  d3d10core's last slice (a worker thread past its stack) and two crashing test functions, ws2_32 (151 lines:
  no DNS, no services database, socket option and error differences), and d3d11, d3d9, dxgi and VKD3D-Proton's
  own suite on the console.
* **Step 3, the Java game**: **runs on Vulkan** (below); the OpenGL path stays blocked by the OpenGL driver holding
  9 GiB of Sodium's buffers (a fix for the PS5 OpenGL repository).
* **From prospero-win** (upstream `d1ca1e59`): its resolver is adopted. Its in-process server running sync
  requests and file opens on the calling thread (its #341, #342: about 20 us saved per request, 112.8 -> 99.8 s to
  a GTA IV new game) is the strongest next optimisation and needs its server-lock patch ported to this tree; its
  virtual links on create (#337) would let a game's chatty log go to /dev/null. Its IA-32 translator work is out of
  scope (D1).

### Rules for this work

A 100% Wine conformance pass is not the gate for game work. Compare the pinned Linux baseline and the PS5 runs;
keep unsupported behavior and upstream failures visible, and prioritize new PS5 regressions, crashes, hangs,
leaks, incorrect saves and API failures used by the target games. Expected limitations can still block games and
are not evidence of compatibility. Expand conformance coverage alongside game testing.
