> Historical snapshot of `README.md`, archived on 2026-10-07 from `9b07ebb`.
> Defaults, next steps, counts and failure classifications below belong to their original runs.
> Use the [current document](../../../README.md) for present guidance; see the [archive index](README.md).

# PS5_Proton

**A PlayStation 5 homebrew launcher that runs 64-bit Windows games, on a Proton-derived
runtime: Wine's Unix side native on the console, Direct3D through DXVK and
VKD3D-Proton on the console's Vulkan (RADV), a DualSense in the player's hands.**

Status: **A .NET (MonoGame) game launches and renders its menu on the console (2026-10-02).** The launcher runs: it asks
the Lapy daemon for `/data` (a title cannot see `/data` otherwise; the client is the platform
layer's `ps5_elevation_request`, and a daemon must be running), lists the games
under `/data/ps5proton/games` and under its own folder's `games/`, starts the chosen one
by restarting the title with that game's arguments, and comes back when the game closes.
On the console it lists that game with `elevation: ok`, `data: granted`. The Wine runtime
is integrated into the game launch path
([`docs/PLAN.md`](docs/PLAN.md), [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md),
[`NOTICE.md`](../../../NOTICE.md)).

The runtime has **81 patches**, of which seven (0160, 0161 and 0910–0913) are checked on the host only and wait for a console run. Console probes verify PE64 threads/events,
fixed-address JIT memory, fault recovery with FP/AVX state, and 180 D3D11 frames.
A capture of the actual 3840×2160 Vulkan swapchain verifies presentation scaling.
All 11 PS5 runtime PRXs cross-build with no unresolved symbols.

The installed game uses **MonoGame DesktopGL**. Its OpenGL path is now
integrated with a pinned PS5 OpenGL SDK; the title screen and complete menu render correctly at
1920×1080 and approximately 60 FPS after moving the final copy to the GPU.
The audio path submits 48 kHz stereo; after correcting AudioOut return-value
handling, I confirmed the sound is smooth. A Windows HID console probe
now identifies the DualSense (`054c:0ce6`) and reads its 64-byte reports.
I confirmed controller navigation, touchpad mouse movement and touchpad
left-click in-game; the character-creation screen renders at about 60 FPS. Gameplay and saves remain to be tested (see [`docs/PLAN.md`](docs/PLAN.md)).

## Launcher appearance

The rebuilt launcher uses the updated PS5 Vulkan Template UI kit, with **Gloss /
Skeuomorphic** as the default and **all 30 themes** available. Press **SQUARE**
to open the picker, use the D-pad to preview, **CROSS**
to apply, and **CIRCLE** to return. The choice survives launcher restarts in
`hui/theme.txt` under the title folder. Hold **OPTIONS** for one second to leave the launcher.
The main list contains games only; appearance information stays in the picker.

The launcher now opens without elevation, using its sandbox assets. On the
user-confirmed kstuff-only boot, the Themes screen rendered 300 frames at 59.9 FPS
both without and with `/data` access. Six subsequent lifecycle/probe runs completed
11 elevations, with all 28 disposable donors reaped and root counters restored;
PE64, D3D11 and XInput probes passed. See
[`docs/ELEVATION_VALIDATION.md`](docs/ELEVATION_VALIDATION.md) for hashes, failures,
measurements and limits. Manual UI/audio checks and real-game save/reload remain.

The game library is under `/data`, so the title only sees it while the Lapy daemon
is running. The title starts the resident daemon itself: when no daemon takes its
request within 3 s, it sends the pinned daemon from its own folder to websrv/elfldr on
the console's loopback and asks again. The first launch after a console restart draws
the library about 5 s after the title starts (4.9 s measured); later launches use the
running daemon (0.5-0.7 s). Only kstuff and elfldr/websrv, loaded with the console's
jailbreak, are needed by hand. If the daemon cannot be started, the launcher shows
"Game library unavailable", not an empty library.

64-bit Windows games only, for now: the launcher reads each game's executable header
and marks a 32-bit program (native, or .NET that requires 32 bits) **32-BIT**; it is
listed but cannot be started.
The per-boot launch cap is removed; 60-second pacing and the disconnect guard remain.

## How it is measured

A game that fails shows one symptom of one broken behaviour, so the runtime is measured against tests of the behaviour
itself, and a game run is the last check. Wine's own conformance tests (ntdll, kernelbase and kernel32 so far) run on
the console and on Linux, the Direct3D 9, 10, 11, 12 and DXGI tests run against DXVK and VKD3D-Proton, and every failing
line is classified: a limit of the console (16 KiB pages, no symbolic links, polled directory
changes), or an open defect of the port, which is then reduced to a probe and fixed. The method, the numbers and what is
still open are in [`docs/CONFORMANCE.md`](docs/CONFORMANCE.md); what Windows starts at boot and the console cannot is in
[`docs/BOOT_STATE.md`](docs/BOOT_STATE.md); the plan for the next console session is
[`docs/NEXT_CONSOLE_SESSION.md`](docs/NEXT_CONSOLE_SESSION.md).

Windows processes now run as **elfldr payloads** by default. The title owns the
shared wineserver, GPU broker, display and input; descendants spawn through that
owner. This requires resident websrv/elfldr. Eight simultaneous Windows children
passed arguments, inherited handles, pipes, exit codes and native cleanup. A
separate test passed four DXVK rendering processes, including descendants, with
correct owner-display pixels and input. Production's default also passed a
180-frame D3D11 probe with matching readback and display pixels. Process/GPU
tables hold 32 entries; 32 simultaneous Windows clients remain unproved.
OpenGL payload children, application compatibility and child audio remain open.
Sony spawning remains available for diagnostics with `WINE_PS5_PAYLOAD_CHILDREN=0`.
See [`docs/WINDOWS_PROCESSES.md`](docs/WINDOWS_PROCESSES.md).

## What this is, and what it is not

It is a **launcher plus a Proton runtime**. The runtime is
[GE-Proton11-7](https://github.com/GloriousEggroll/proton-ge-custom): Valve's Wine
(the revision that release pins), wine-staging, GE's own patch groups, DXVK and
VKD3D-Proton — the PS5 patch series is rebased *onto that tree*, not onto vanilla Wine.
The launcher is an ordinary PS5 title (Vulkan display through RADV, the DualSense,
AudioOut) and takes the place of the launcher's Linux runtime that drives Proton there.

What cannot cross over is named in [`docs/PLAN.md`](docs/PLAN.md), piece by piece: the
`proton` **bash** runtime and `protonfixes` **Python** are reimplemented — in C, with the
same environment knobs, and as per-game data files — because the console has no shell and
no Python; the game launcher's client and its runtime container are out of scope entirely. Every
other piece of Proton is kept as it ships.

It is **not** a Windows emulator for 32-bit code: that is
[prospero-win](https://github.com/mpereiraesaa/prospero-win)'s ground, with its own
IA-32 translator. This project takes the path that needs no translator at all — the
console is x86-64, so a **64-bit** Windows executable runs natively under Wine's
x86_64 PE modules, and only Wine's Unix side has to be ported.

| | |
| --- | --- |
| Target | 64-bit Windows games, Direct3D 9/10/11 through DXVK, Direct3D 12 through VKD3D-Proton |
| Console | PS5 homebrew environment: FTP, an ELF loader, titles in `/data/homebrew`, `/data` access |
| Data | your own game copies; no game data is ever shipped, downloaded or committed here |
| Out of scope | the game launcher's client, anti-cheat, kernel drivers, 32-bit-only games |

## Lineage, and what is owed to whom

- **[prospero-win](https://github.com/mpereiraesaa/prospero-win)** (mpereiraesaa,
  LGPL-2.1-or-later) is the knowledge base: Wine's Unix side on PS5 as PRX modules, the
  in-process wineserver, the PS5 user driver for `win32u`, the winevulkan display-plane
  patches, the audio driver, the XInput patch, the data mount, and above all its
  documentation (`docs/WINE_PS5_BUILD.md`, `docs/ARCHITECTURE.md`). Its PS5 patch series
  is the starting point for this project's, and it is credited in [`NOTICE.md`](../../../NOTICE.md).
- **[GE-Proton](https://github.com/GloriousEggroll/proton-ge-custom)** (GloriousEggroll)
  and **[Valve's Proton](https://github.com/ValveSoftware/Proton)**: the runtime stack —
  Wine, DXVK, VKD3D-Proton and the patches that make modern games work at all. The fork
  this project pins is [`mihawk-99/proton-ge-custom`](https://github.com/mihawk-99/proton-ge-custom).
- **[PS5 Vulkan Template](https://github.com/mihawk-99/PS5_VulkanTemplate)** (mine): the
  title foundation, the build/deploy/run tools, and the test harness every step here is
  proved with.

## Licence

LGPL-2.1-or-later, for the same reason prospero-win chose it: a compatibility layer
exists to be combined with proprietary code at run time, and its value accumulates in a
long tail of small fixes that must flow back. See [`LICENSING.md`](../../../LICENSING.md) and
[`NOTICE.md`](../../../NOTICE.md). Game data keeps its own licence and is never distributed here.
