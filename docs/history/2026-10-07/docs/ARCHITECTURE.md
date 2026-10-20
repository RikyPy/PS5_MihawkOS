> Historical snapshot of `docs/ARCHITECTURE.md`, archived on 2026-10-07 from `9b07ebb`.
> Defaults, next steps, counts and failure classifications below belong to their original runs.
> Use the [current document](../../../ARCHITECTURE.md) for present guidance; see the [archive index](../README.md).

# Architecture

Who owns what, how a game boots, and where the boundary between this project,
prospero-win's Wine work and upstream Proton runs.

## Ownership

| Layer | Owner | Notes |
| --- | --- | --- |
| Title foundation: Vulkan device, display through RADV, tools, test harness, Vulkan UI kit | `PS5_VulkanTemplate` (copied by `new-title.py`) | Same foundation as my other titles, so the launcher is tested the same way |
| Launcher: game list, menu, profiles, prefix layout, restart per game | this repository | The part of Proton's runtime that is a shell script upstream becomes C here |
| Wine's Unix side on PS5: PRX modules, in-process wineserver, `win32u` user driver, winevulkan display plane, audio driver, XInput, `/data` mount | this repository, derived from prospero-win's patch series (LGPL-2.1-or-later) | Read `../prospero-win-main/docs/WINE_PS5_BUILD.md` before touching a patch |
| Wine's Windows side (PE modules), Wine's own DLLs | upstream Wine, as pinned by the Proton-GE fork | Never hand-edited; patches only, as files |
| DXVK (d3d9/10/11), VKD3D-Proton (d3d12) | upstream, as pinned by the fork | Built for the console's PE side, staged into the prefix |
| RADV | `PS5_Vulkan` + `PS5_Mesa` | Linked by the recipe; never hand-linked |
| Shared GPU broker: packets, memory, submissions, fence-gated cleanup, display and frame pages | `PS5_Vulkan/tooling/gpu-broker` | Optional backend; consumes PS5_Mesa's ordered broker overlay |
| Mesa broker overlays and Zink presentation behavior | `PS5_Mesa/ps5/broker` and Mesa sources | Foundation-owned; ordinary title RADV remains separate |
| Optional PE64 WGL/Zink build | `PS5_Vulkan/tools/build-mesa-windows-zink.sh` | Pinned Mesa fork and compiler; Wine prefix/DLL configuration remains here |

## Shared graphics foundation

`tools/setup-graphics-foundation.sh` exports PS5_Vulkan revision
`4d7e053de9f3b338630396c2cad07a6a60c2c24d` into an ignored cache. It also
exports the exact platform header from the existing RADV base revision,
`7b59ef27c1b09b9671bc4153c41940c3155c3af2`. The broker build consumes Mesa
overlay revision `f7ce91ac4c3697990bf89d79fdc61ae7cb98570b`; the optional
WGL/Zink build uses the next Mesa fork revision,
`b3588f78dd7fd46fbadc7ecd487e5f14d4213e4b`, which keeps Zink's persistently
mapped buffers host-visible. Mutable sibling sources are not used for these
components. No build fetches implicitly.

The owner compiles foundation allocation/submission/display and packet sources.
Proton provides native-lifetime checks, title paths and Wine desktop callbacks.
The client module links the foundation's complete RADV broker archive with the
canonical recipe; Proton packages its PRX exports and binds the transferred GPU
fd from Wine's environment. Wine's process bootstrap, wineserver, input and DLL
staging stay here. Legacy wire names and log labels preserve current clients
and evidence parsers. Native diagnostic probes remain in this repository.

The extraction initially passed builds and focused host checks. Subsequent
console runs prove visible WGL/Zink presentation through the broker and
a .NET (MonoGame) game's title screen with audible helper audio using production defaults.
The title retains its loading display until a presenter opens VideoOut;
Wine's PRX presenters use their release_display callback, and the native
broker uses the title's linker wrapper. See
[WINDOWS_PROCESSES.md](WINDOWS_PROCESSES.md#manual-launch-defaults-2026-10-06) and
[GRAPHICS_PROCESS_BROKER.md](GRAPHICS_PROCESS_BROKER.md#foundation-extraction-2026-10-06).

## Boot flow

```text
home screen -> the launcher title
   main() -> detect which mode: no arguments = launcher, arguments = run a game
   launcher:  read /data/ps5proton/games/*/game.json
              show the themed library (UI kit over a RADV display)
              on choose: sceSystemServiceLoadExec(self, game id)      # clean process
   game:      load the runtime for that game's profile
              load ntdll.prx, win32u.prx, wineserver.prx             # Wine's Unix side
              start Wine with the game's PE64 executable
              become the platform side: frames out, pad in, audio out, /data mounted
              when the game exits: sceSystemServiceLoadExec(self)     # back to the launcher
```

One game per process is not a preference, it is the design: it gives every game a clean
Wine process, a clean shader-cache state and a clean `/data` mount, and it means a game
that crashes the runtime cannot take the launcher with it. prospero-win arrived at the
same shape.

## Child processes and layouts

Every launch defaults to the owner/broker layout (2026-10-06): the title owns
wineserver, GPU submission, display and input; every Windows process runs as an
elfldr payload. Executable names do not select different process architectures.
The `title` and `single` layouts remain explicit diagnostics, as does Sony spawning
with `WINE_PS5_PAYLOAD_CHILDREN=0`. See [WINDOWS_PROCESSES.md](WINDOWS_PROCESSES.md), "Layouts", and
`tools/run-battery.py`. The sections below record how the pieces were first proved, as experiments.

## Experimental headless children

With `WINE_PS5_CHILDREN=1`, the game's original native title remains the shared
wineserver host. Sony's local-process service starts a separate headless loader,
which receives Wine's existing server socket through SCM_RIGHTS and uses
`WINESERVERSOCKET`. Windows startup parameters and inherited handles come from
the original server. Native signals travel through a separate control socket;
Sony owns native teardown. A Windows parent may exit while the native host waits
for its children, but closing the entire PS5 application closes those children.
Production defaults remain off. See [WINDOWS_PROCESSES.md](WINDOWS_PROCESSES.md)
for evidence, build steps and the unproved graphics boundary.

Descendants additionally require `WINE_PS5_DESCENDANTS=1` in the Windows
environment. Each helper forwards its new server socket and a private reply
socket to the original title, which owns every native launch and teardown.
Sony's ownership is flat while Wine retains each immediate Windows parent.
Three-generation, repeated-chain and spawning-after-parent-exit probes passed.

An additional experimental arrangement (`WINE_PS5_OWNER_BOOTSTRAP=1` and
`WINE_PS5_GRAPHICS_HOST=1`) reserves the original title for wineserver and its
first Windows graphics client. The initial Windows launcher runs in a helper.
A child request with `WINE_PS5_GRAPHICS_CHILD=1` in its Windows environment
transfers the existing startup connection back to the original title, where the
unused Wine client starts with the title's existing GPU capability. The request
marker is consumed before the child's environment is published, so its ordinary
descendants can use helpers. Windows parenthood follows Wine, independently of
the flat Sony native ownership. Two full DXVK/Vulkan probes passed pixels,
loading-display release, a headless grandchild and teardown. The original title
can host one graphics client per fresh session; a second returns ERROR_BUSY.
A Java launcher, a Java game, other graphics APIs and general process conformance are unproved.

The next graphics architecture is a broker below Vulkan at RADV's PS5 platform
boundary: separate subprocesses retain their own Wine/RADV state, while the
original native title owns GPU mappings, submissions and presentation. Two
native helpers have passed shared-descriptor CPU aliases and interleaved GPU
completion checks, including two 32 MiB allocations in the shader-address
window. Two native children now also initialize and recreate independent Vulkan
devices and pass complete offscreen image-clear/readback checks. A subsequent probe passed four shader-pipeline triangle draws with verified
readback pixels and balanced cleanup. The owner has also presented four child
images with confirmed flips and full scanout pixel checks. Headless Wine
descendants pass with the transport. Two Wine/DXVK rendering children and two
rendering descendants also pass 32 confirmed flips, sampled scanout/guest
pixels, Windows parentage and cleanup. See
[GRAPHICS_PROCESS_BROKER.md](GRAPHICS_PROCESS_BROKER.md).

## The runtime directory (on the console)

```text
/data/ps5proton/
├── runtime/<version>/          the pinned runtime: Wine's PRX and PE modules, DXVK, VKD3D-Proton
├── games/<id>/game.json        what the launcher reads: name, exe, args, profile
├── games/<id>/drive_c/         the game's own files (user-supplied, never shipped)
├── prefixes/<id>/              the Wine prefix for that game (per profile)
├── caches/<id>/                DXVK state cache, RADV shader cache, VKD3D pipelines
└── logs/                       per-launch logs for bug reports
```

`/app0` (the title folder) holds only what the title itself needs; everything a game
owns lives in `/data`, where it survives a title update.

## The platform boundary

Wine expects a Unix side that provides: a user driver for `win32u` (windows, cursor,
monitor, Vulkan surface), an audio driver, XInput, files, threads and memory. On the
console each of those is a sink:

| Wine expects | The console provides | Where |
| --- | --- | --- |
| A window system for its user driver | VideoOut, scanned out through AGC, with the title presenting frames | `platform/` (PS5), derived from prospero-win's `native/pw_videoout_ps5` |
| A Vulkan surface (`winevulkan`) | `VK_KHR_display` on VideoOut, via the display-plane patches | `wine/patches/` (winevulkan group) |
| OpenGL / WGL | Single-title EGL retains the pinned PS5 OpenGL SDK; payload WGL defaults to foundation PE64 Mesa/Zink through Wine Vulkan and the broker (a visible game title screen proved) | Wine EGL patches remain here; shared WGL build and Zink fixes belong to PS5_Vulkan/PS5_Mesa |
| An audio device | AudioOut, 48 kHz stereo; payload helpers default to the owner's audio mixer | derived from prospero-win's `wine/wineps5` |
| Controller input | native DualSense HID through in-process Wine PnP/winebus/winehid; XInput compatibility bridge remains available | `0870`, native report encoder and Wine sink |
| Touchpad | relative mouse motion and left-button press/release through the win32u input queue | `src/proton_touchpad.h`, `native/proton_wine.cpp` |
| A filesystem with `$HOME`, `getcwd`, directory listing | `/app0` and `/data` only; no `getcwd` | platform layer + patches |
| Threads, heaps, signals | the console's `pthread` and direct memory | platform layer |

## Why 64-bit first, and what that buys

The console is x86-64, so a 64-bit Windows executable's code runs natively under Wine's
x86_64 PE modules — no instruction translation, no translator's accuracy or speed tax,
and none of the DBT maintenance prospero-win carries for i386. What remains is Wine's
Unix side, which is what the patch series ports.

That is a scoping choice, not a limit of Proton, which runs 32-bit games on Linux: the
kernel there offers the CPU's 32-bit compatibility mode, and **the console refuses that
mode to a title**. Wine's stock WoW64 CPU backend (`wow64cpu.dll`) therefore cannot run,
which is why prospero-win wrote `wowprospero.dll` and selected it with the prefix's
`HKLM\Software\Microsoft\Wow64\x86`. A 32-bit game on this console needs that
translator; adopting it is a later workstream of this project (decision D1), not a dead
end — and a 32-bit game that is also OpenGL needs the GL path as well.


The experimental GPU broker also has an owner presentation transport.
Completed linear RGBA/BGRA pixels stay in descriptor-backed shared memory;
the owner copies them into its own tiled VideoOut buffers and waits for the
reported flip argument. Native helpers passed four child-image presentations
with complete scanout pixel checks and balanced cleanup after display warm-up.
The Wine prototype adds a separate GPU descriptor to every Sony-managed
helper, including descendant requests routed back through the owner. Its
Mesa CPU WSI overlay uses shared linear buffers for display-plane swapchains;
native GPU libraries and display handles remain exclusively in the title.
The matching loader/runtime bootstrap protocol is `0x50573504`, and the
route is opt-in (`PS5_GPU_BROKER`, `WINE_PS5_GPU_BROKER=1`). Headless Wine
process semantics pass with it; four independent DXVK renderers also pass.
The broker client uses a 2 MiB command ring (the allocator and bounds are
rebuilt together) and helpers use 4 MiB native bootstrap stacks. Wine patch
`0928` lets the broker scale the guest image instead of forcing the ordinary
title's fixed-VideoOut fullscreen blit. Shared GPU backing consumes flexible
memory; the 640×360 test peaked at 98,828,288 shared bytes. This does not
establish game-scale allocations, input/audio, OpenGL or VKD3D children.


In the payload-process experiment, the owner can replace Sony's helper creation
with a request to resident websrv/elfldr (`WINE_PS5_PAYLOAD_CHILDREN=1`). A unique
rendezvous transfers the existing packet control endpoint; fd 3 stays control,
WINESERVERSOCKET names the separate wineserver connection, and GPU/input retain
their existing descriptor contracts. The native loader is shared with the Sony
helper. Descendant requests still return to the one owning title and wineserver.
Payload lifetime belongs to the control connection rather than Sony's title
service: EOF terminates the payload; native PID absence gates GPU reclamation.
The adapter passes a negative native PID to the GPU close callback for this route;
positive values retain the Sony service-ID meaning. All three owner tables have
32 slots, while console evidence currently proves eight simultaneous Windows
children. Suspension is not inherited from Sony's owner title.

Payloads explicitly adopt their SDK's executable-memory functions and supply
`sceKernelAllocateDirectMemoryForApp` through a callback. Wine's direct-memory
mapper backs their Windows commits and Unix heap from the shared 11,712 MiB
application pool while the helpers retain budget 0. Only the payload imports
`libkernel_sys`; title PRXs do not. A diagnostic opt-out retains flexible-memory
backing. Two simultaneous 2 GiB Windows allocations and capacity reuse passed
on 2026-10-07 (see `WINDOWS_PROCESSES.md`). A payload-only PRX
load hook changes its verified native text from Sony's execute-only protection
to read+execute and prefaults the 16 KiB text pages, including ntdll before Wine
starts. This lets the native runtime/unwinder read code. The same runtime without
these callbacks keeps Sony's original mapping and module-loading behavior.
The graphics experiment has produced correct owner-display pixels from two DXVK
children and two rendering descendants, with separate input pages and clean
native-PID/fence-gated reclamation; it does not prove game-scale concurrency.
