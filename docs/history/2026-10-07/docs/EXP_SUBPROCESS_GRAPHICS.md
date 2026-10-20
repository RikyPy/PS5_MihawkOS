> Historical snapshot of `docs/EXP_SUBPROCESS_GRAPHICS.md`, archived on 2026-10-07 from `9b07ebb`.
> Defaults, next steps, counts and failure classifications below belong to their original runs.
> Use the [current document](../../../EXP_SUBPROCESS_GRAPHICS.md) for present guidance; see the [archive index](../README.md).

# Giving a second process graphics

Branch `experiment/subprocess-graphics`, from `main` at `2dd897b`. Opt-in work only: production
stays signed and untouched, and every console deployment restores it.

## The question

A PS5 title session hosts **one** graphics client. A graphical launcher and a graphical game
therefore cannot run at the same time; switching between applications means restarting the native
title (`sceSystemServiceLoadExec`). That is a property of the arrangement in `2dd897b`, not proof
that two graphical applications are impossible. This branch asks:

> Can any subprocess — a Sony-managed helper, a Windows descendant, or a payload — be given GPU
> access, and if not, what is the cheapest arrangement that puts two graphical applications in
> one session?

## What is already proved on the console (firmware 10.01)

* Headless Windows descendants through Sony-managed helpers: three generations, four repeated
  chains, spawning after the immediate parent exits (`6ae97ac`, `12d7cbf`, `c12e3f8`).
* The original GPU-capable title hosts the Windows **graphics** child while the headless Windows
  launcher runs in a helper: runs 05/06/07 each presented 180 frames in 3007/3000/2997 ms, matched
  five triangle pixels, released the loading display, balanced parent handles 14/14 and left zero
  fatal or GPU-fault records (`2dd897b`).
* A headless grandchild runs with the GPU device of its parent still alive, and the helper chain
  is deleted at teardown (`evidence/graphics-process-console-regression-01.json`).
* Switching applications by restarting the title works (`47f…` regression runs, and every
  `tools/run-console.py` run since).

## What is ruled out

* A **Sony-managed helper cannot open GPU VA space** (`2dd897b`).
* A **transferred owner GPU descriptor returns `EPERM`** in a helper (`2dd897b`).
* A **second graphics client in the existing session** is rejected with `ERROR_BUSY` — but that is
  **our own one-shot guard**, `native/proton_wine.cpp:286-302` (`graphics_claimed.exchange(true)` ->
  `EBUSY` -> `STATUS_DEVICE_BUSY` -> `ERROR_BUSY`, asserted by `tests/process-probe.c:203`). It is
  therefore *not* evidence about what the console allows: it is a policy we chose. Relaxing it is a
  cheap experiment, not a platform change.

So the two obvious sharing routes — hand the helper a descriptor, or open a fresh GPU context in
the helper — are closed as measured, and the remaining question is *which* check closes them and
whether that check can be satisfied, inherited, or avoided. A second client also has to get past
Wine itself: the in-process wineserver and win32u bring-up are init-once
(`wine/patches/0110-server-run-in-process-on-ps5.patch`, `docs/ARCHITECTURE.md`).

## Research threads

Each thread is a read-only investigation; findings land here with citations.

1. **The exact refusal path.** Which call, in which process, returns the `EPERM`/`ERROR_BUSY`: our
   guard, a Sony userspace API, or a kernel check on GPU VA ownership.
2. **App identity and budgets.** Is GPU eligibility bound to the app id and the ResourceArbitrator
   budgets the shell grants a launched title, and can a child inherit or be granted them?
3. **Precedents elsewhere on the console.** System overlays, capture, mini/background apps,
   payload-side rendering, and what the HEN's privilege features actually grant — plus which
   cross-process sharing mechanisms the console's driver supports at all.
4. **The Wine side.** Where the Windows graphics chain would have to change for a second process,
   and what alternatives exist inside one GPU process (single-owner compositing, a headless or
   software-rendered launcher, a cheaper application switch).

## Findings

### Thread 4 — the Wine side (complete)

**The chain today.** One native title process owns the GPU. Launcher mode is plain template RADV
with no Wine. Game mode: `ntdll.prx` -> `__wine_main` (`src/pw_wine_start.c:99`) -> the in-process
wineserver (`wine/patches/0110-…patch`) -> `win32u.prx` installs the PS5 user driver
(`wine/patches/0400-…`, `0460-…:207-216`). D3D goes game PE -> DXVK -> `winevulkan.dll` ->
`winevulkan.prx` -> win32u `pVulkanInit` (`0460:30-42,181-190`) -> `ps5_vulkan_surface_create`
(`0460:90`), which first releases the display (`0460:106`, dlsym `:212` ->
`pw_wine_release_display`, `wine/ps5/pw_wine_sink.c:57-68`, one-shot per process), then uses one
GPU, one display, `planeIndex 0`, `planeStackIndex 0` (`0460:114-140`) ->
`vkCreateDisplayPlaneSurfaceKHR` -> `libvulkan.prx` (RADV) -> `radv_ps5_platform_once` ->
`sceAgcInit` (`PS5_Mesa/src/amd/vulkan/winsys/ps5/radv_ps5_platform.c:254`). GDI instead has
`ps5drv` compose visible window pixels into one buffer and `pw_wine_present` them, one sink per
process (`wine/ps5/pw_wine_sink.h:5-19`).

**Where a second client stops, outermost first.**
1. *Our guard* — `native/proton_wine.cpp:290`, one-shot, `ERROR_BUSY` (see above).
2. *Wine itself* — wineserver and win32u are init-once per process, so a second client cannot
   re-enter `__wine_main` in the title (patch 0110).
3. *The helper* — GPU VA never opens: `/dev/gc` `ENOENT`, AGC `gbase_gvmsw_open` `6978`/`0x804c0003`,
   a donated descriptor `EPERM` (`docs/WINDOWS_PROCESSES.md:292,337-346`); a helper preloaded with
   VideoOut dies `0xa0020104` (`docs/SERVICE_SPAWN.md:242`).

**What would have to change, by owner.** Us: replace the one-shot flag with per-claim ownership and
a display broker — small, but it only relocates the wall. Wine: support two clients, or re-init
Wine after a client exits, in one process — large (win32u's single sink and plane 0 also need a
plane arbiter). `winevulkan`: nothing, it carries no cross-process limit. PS5_Mesa: nothing —
per-process `sceAgcInit` needing GPU VA is not Mesa work. Platform layer/Sony: give a helper a GPU
VM or budget association; the service does assign a supplied budget
(`docs/SERVICE_SPAWN.md:149-151`) and can preload VideoOut (module 13, bit `0x100`, `:269`), but
whether a local process can ever hold GPU VA is unproved. Four failed AGC attempts do not prove
impossibility (`docs/WINDOWS_PROCESSES.md:305`).

**Alternatives without a second GPU process.** Native launcher with the game via `LoadExec` is
already production (120 frames at 59.9 FPS) and covers everything except a *Windows* GUI launcher.
Software/2D frame forwarding: a headless helper's win32u already CPU-composes its window pixels, so
`pw_wine_present`/`next_input` in `native/proton_wine_child.c` could ship BGRA over the existing
control socket to the title, which presents it and returns input — no GPU in the helper; proof
would be a GDI/Java window from a helper appearing and responding (Java2D's software fallback
actually drawing is unproven). A faster `LoadExec` is incremental against a baseline of 8.3 s to
first frame for a light title and 19.6 s for a heavy one. Launcher-in-title with the game as a
child that hands the GPU back cannot work in-process (init-once), so it collapses into the
`LoadExec` case.

**Experiments it proposes, cheapest first.** (1) host-only: finish tracing the service budget and
application-callback path (`docs/SERVICE_SPAWN.md:137-151`) for any budget/profile that carries GPU
VA; (2) console: spawn a helper with a preload mask including VideoOut and a non-default budget,
logging `sceAgcInit`; (3) console: relax only our guard so a second graphics request reaches the
title, and log where it dies (Wine re-entry vs `release_display` ->
`VK_ERROR_NATIVE_WINDOW_IN_USE_KHR`, `0460:106-110`); (4) console: time `LoadExec` to first
presented frame; (5) host: portable framing test for the software-forwarding path.

### Thread 1 — the exact refusal path (complete)

**A second graphics client in one session is our own code, end to end.** `tests/process-probe.c:191`
sets `WINE_PS5_GRAPHICS_CHILD=1`; `:201-205` requires `CreateProcessW` to fail with `ERROR_BUSY`.
`wine/patches/0927` (`dlls/ntdll/unix/process.c:28-39`) scans the environment for that literal, sets
`PW_CHILD_GRAPHICS`, and calls `pw_ps5_spawn(..., 2)` ->
`wine/ps5/pw_wine_process.c:230-237` -> `spawn_through_owner` (`:200-219`) sends `PW_CHILD_SPAWN`
over `child_control` (`:206`) to the owning title -> `child_agent` (`:300`) -> `accept_spawn`
(`:77`) -> `spawn_worker` (`:59-76`) -> `graphics_start()` = `native/proton_wine.cpp:286`
`start_owner_graphics` (registered at `:786`) -> **`:290` `if (graphics_claimed.exchange(true)) { errno = EBUSY; return -1; }`**.
The flag is set by the first graphics child, so the second returns before any Sony call. The errno
rides the reply (`:70`), the helper sees `errno=16` (`:211`), ntdll maps it through
`errno_to_status(EBUSY) = STATUS_DEVICE_BUSY` (`dlls/ntdll/unix/file.c:355`) to `ERROR_BUSY = 170`
(`dlls/ntdll/error.h:208`). Console record: `klog/graphics-owner-render-05.log:443`
`broker requester=882 child=0 status=16`, with the probe passing in
`klog/graphics-owner-render-05-wine-child-882.log:60`.

**A helper asking for the GPU is refused by a kernel check on GPU VA, per calling process.**
* `/dev/gc` open returns `ENOENT` **before and after elevation**
  (`native/proton_wine_child.c:50`; `klog/graphics-child-device-query-01-wine-child-733.log:3,5`).
* The kernel prints, per attempt, `====== gbase_gvmsw_open 6978 error` and
  `gc_open_internal. Can't open GPU VA space. ret:-2142502909 (0x804c0003)`
  (`klog/graphics-child-device-query-01.log:278,286`). With a VideoOut preload it is fatal:
  `0xa0020104` (`klog/service-native-03/capture.log:199-204`, `docs/SERVICE_SPAWN.md:242`).
* `sceSysmoduleLoadModuleByNameInternal("libSceAgcDriver")` in a helper returns `0x80020002`
  (`klog/graphics-child-agc-preflight-04-wine-child-716.log:7`) and `sceAgcInit` is never reached:
  AGCDriver propagates the refusal, it is not the guard.
* The owner opens `/dev/gc` and its ioctl `0xc004812e` returns `0`, value `0`
  (`wine/ps5/pw_wine_process.c:162-167`; `klog/graphics-child-gpu-donor-query-02.log:278-280`).
  **The same fd passed to a helper over `SCM_RIGHTS` (`:169-170`) gives the helper
  `ioctl -1`, errno `EPERM`** (`native/proton_wine_child.c:57`;
  `klog/graphics-child-gpu-donor-query-02-wine-child-759.log:4,7`). So the permission is evaluated
  per calling process at ioctl time, inside the kernel — descriptor donation does not carry it.

**Discriminators already eliminated.** `sceKernelGetProcessType` reports `0x80020003` for the owner
and `0x8002004e` for a helper, and a helper reads `0x80020003` **after elevation**; the app id is
`40984` in both. A helper matching the owner's type *and* app id still gets `EPERM`
(`evidence/graphics-child-process-identity-01-deployment.json`, untracked: gpu-child queries 6/6
`{status:-1, errno:1}` against owner `{0,0}`). Neither process type nor app id is the discriminator.

**Ruled out by commit.** `7cbd5b1`: our own `rfork_thread` probe in a title panicked the kernel and
produced no child. `6ae97ac`: `AddLocalProcess` with the headless mask `0x8000000000000002` works,
while the default VideoOut preload dies `0xa0020104`. `12d7cbf`, `c12e3f8`: Wine and its
descendants run in helpers, headless only. `8dfcc3c`: a fresh child's AGC/GPU VA lifecycle and
elevation (RegMgr `0x14`, AgcDriver `0x80020002`, klog `0x804c0003`). `2dd897b`: descriptor donation
(`EPERM`) and the arrangement that does work.

**Unknowns this leaves.** No helper has been spawned with a **service-granted non-default budget
plus a VideoOut preload** — the budget/app association is untested. Our flag has never been relaxed,
so Wine's init-once limit (patch 0110) versus any Sony limit for a second *in-session* client is
unmeasured — and note that removing our flag therefore reaches Wine, not a Sony check. The
`/dev/gc` fd has never been ioctl'd from a forked child before `SCM_RIGHTS`, so "a transferred
descriptor" and "a different process" are not separated. `0x804c0003` and `6978` are undecoded and
`gc_open_internal`'s predicate (process type, app id, GPU VM container, budget) is unknown. No
payload-side or second-app GPU client, and no helper-side frame or swapchain, has ever been tried.

### Thread 2 — app identity and budgets (complete)

**GPU eligibility is created by the shell's launch sequence, not carried by the app id.** The path
is `createApp PPSA99170` -> `processSpawn(pid, appId, ..)` -> `AvControl onAppLaunch` ->
"start videoout session" -> "VideoOut: shared (pid .. appId ..)"
(`klog/graphics-child-process-identity-01.log:69,71,74,80-81`), and it is AGC, not GnmDriver.

**A helper is already inside the application and still gets nothing.** The identity probe measured a
helper at the title's own app id (`0xa018`), `app_status` 0, post-elevation process type
`0x80020003`, and an `EXEC` line with the same `vm#1 dmem#1 abi=native category=native_game` as the
title's (`…identity-01.log:149` against `:225`) — and `/dev/gc` still returns `ENOENT`, a donated
owner fd still `EPERM`. So app id, app status, process type and VM/device memory identity are all
already transferred, and **"the child is outside the application" is retired as a hypothesis**.

**Contention is not the explanation either.** In that run the title held no graphics client (the
owner was bootstrapped, not rendering) and the helper still failed.

**ResourceArbitrator budgets are not the gate.** The title renders with `vcn[0] scanin[-] acp[0]
mem[0] prio[2(2)]` and `SetVCNBudget` refuses it ("pid/appId is INVALID_ID", then "vcnType is
TYPE_UNKNOWN") (`klog/d3d-probe-01.log:12-21,138,16-17,119`); three helper spawns produced no
ResourceArbitrator lines at all (`…identity-01.log:207,240,318`; none between `:141` and `:593`).

**Kernel budgets are a separate system and libkernel_sys-only.** `sceKernelCreateBudget`,
`…Set…`, `…Get…`, `…Delete…` and `sceKernelGetProcessTypeOfBudget` exist only in `libkernel_sys`
(`PS5_PayloadSDK/sce_stubs/libkernel_sys.c:2158,2778`), absent from an ordinary title's libkernel
(`evidence/service-spawn-static-10.01.json`). SysCore creates ten resource entries (ids
1,2,4,10,5,6,7,12,13,14) and sets `(0x3511, 0x18fa6)`, then deletes some
(`docs/SERVICE_SPAWN.md:137-139,149-152`); **no budget value has ever been measured** — a title
cannot call `sceKernelGetBudget`.

**The spawn API has no GPU knob.** `sceSystemServiceAddLocalProcess(app_id, "/app0/wine-child.bin",
argv, &options)` with a 72-byte options struct (`size`, `fd` = the source descriptor, `report=1`,
`0xc=-1`, `0x18=` the preload mask) (`wine/ps5/pw_wine_process.c:139,142-143,152-156`;
`docs/SERVICE_SPAWN.md:64-79`). No field takes a budget, an app attribute or a GPU flag; the only
GPU-adjacent one is the preload mask (VideoOut = module 13, bit `0x100`, `:265-272`). `PW_CHILD_GRAPHICS`
is ours.

**New lead, uninterpreted.** The 88-byte `GetAppInfo` word at offset 4 is `1` in the title and `0`
in every helper (`native/proton_gpu_identity.h:20-37`; `…identity-01.log:194` against
`…wine-child-992.log:4`). The SDK layout calls those bytes padding/unknown. If it is an
eligibility attribute rather than padding, that is the first measured difference that survives all
the eliminations above — and it is speculation until decoded.

**Inference.** What the helper lacks is the **per-process GPU VA/VM object and device node that the
shell's app-launch path creates**. No capture names it, and `vm#1` is identical for title and
helper, so `vm#1` is not it.

**What would settle it.** (a) Host-only: trace profile/process-type selection for a local process
against an app process. (b) Console: a helper spawned with VideoOut in the preload mask plus a real
`sceAgcInit`, while the title holds no graphics client. (c) Console: find a non-title GPU-VA holder
(`AgcCompositor.elf`, `SceAvCapture` are GPU users, `…identity-01.log:266,269`) and ask whether it
is app-associated or a system process. (d) Console: list `/dev` from inside a helper, to separate
`ENOENT` from `EPERM`.

### Thread 3 — precedents elsewhere on the console (complete)

**Who ever gets graphics in a second process on a PS5: system processes launched by the shell, and
anything `ptrace`-injected into one. Never a local process a title creates.**

*(1) Borrow a GPU owner by injection.* `ptrace PT_ATTACH` plus a remote ELF load into SceShellUI,
AgcCompositor.elf or SceAvCapture, needing authid `0x4800000000010003` (kstuff) and an elfldr/pt
layer. Public, maintained precedent on this firmware class: `ps5-new-overlay` injects its HUD ELF
into SceShellUI and draws over games (as Mono/PUI widgets appended to the ShellUI scene, *not* its
own GPU context), NineS does generic named-process ELF injection with SceShellUI as its example,
and onionHEN's overlay is the same arrangement (its `fps_agc.cpp` reads an AGC/DCE counter from
another process through kernel DMAP — a counter, not a display path). **Two GPU-capable processes
demonstrably coexist on our console while our title runs**: `AgcCompositor.elf` (NPXS40135) and
`SceAvCapture` (NPXS40100) with live FMEM (`klog/d3d-probe-01.log:263,265`,
`klog/d3d11-probe-reg.log:425,428`), with SceShellUI scheduling BGLayer transitions in the same run.
*Falsifier:* injected code inside SceShellUI that logs `/dev/gc`, ioctl `0xc004812e` and
`sceAgcInit` — if ShellUI's own process sees the same `ENOENT`/`EPERM`/`0x804c0003` our helpers
see, then GPU eligibility is a launch property that injection cannot borrow.

*(2) One GPU owner as a broker.* The title keeps the GPU; the helper builds buffers or frames. No
cross-process *graphics client* precedent exists — the closest is `ps5-jar-loader`'s
`KernelAccessorAgc.java`, which uses AGC DMA to read and write kernel memory from a Java process
(a kernel-accessor technique, not a renderer). Cheapest variant needs no GPU in the helper at all:
its win32u already CPU-composites window pixels, so it can ship BGRA over the control socket it
already has and the title presents it (see thread 4).

*(3) Kernel-assisted GPU VM grant.* A payload-side kernel R/W attaches a GPU VM container or
aperture to the child's process and retries `gbase_gvmsw_open`. Nobody is documented doing it;
kstuff-lite and the HENs' privilege features are credential/authid/`ptrace`-focused. Speculation.

*(4) Reach a second application through the shell.* `libSceSystemService` exports
`AddLocalProcessForJvm/ForPs/ForPsmKit` and `sceSystemServiceLaunchApp`, but the payload SDK has no
`LaunchApp` wrapper (`PS5_PayloadSDK/sce_stubs/libSceSystemService.c:1358-1368`); only an injected
shell-side payload could call that path. Not cheap, and (1) comes first.

**Closed by evidence, with sources.** A local process cannot open GPU VA space (`/dev/gc` `ENOENT`,
`gbase_gvmsw_open 6978` / `gc_open_internal 0x804c0003`, AgcDriver `0x80020002`, fatal `0xa0020104`
with VideoOut preloaded). Donating the owner's fd does not carry permission (evaluated per calling
process at ioctl time). App id, process type and VM identity are inherited and still refused.
**Vulkan external memory and semaphores are unavailable on this console**: the PS5 winsys sets
`has_external_fd = false` — "GPU memory and the queue's syncs have no file descriptor to share"
(`PS5_Mesa/src/amd/vulkan/winsys/ps5/radv_ps5_winsys.c:423`); only `VK_EXT_external_memory_host`
exists, and it would still need GPU VA in the receiving process. A title cannot load a graphics
module at run time (`dlopen`/`sceKernelLoadStartModule` refuse repository-built `.so`; AGC arrives
by linked import). And `/dev/gc` is **per process**: AGC needs its own 2 MiB direct pool at
`0xfe0000000` and a small aperture at `0xfe0200000`, discovered by ioctl #46 per process
(`PS5PCEM/src/hle/libs/kernel_ioctl.zig:15-52,219-232`) — a graphics client is a per-process
context, not a shareable device.

## Synthesis

**Closed:** a fresh GPU context in a process we create; descriptor donation; app id, process type
and VM identity as the discriminator; ResourceArbitrator budgets; Vulkan external
memory/semaphore sharing; runtime loading of a graphics module; and two Wine clients in one native
process (init-once, and win32u's single sink and plane 0 would need an arbiter).

**Surviving, cheapest first.**

1. **Frame forwarding (B1).** Helper CPU-composites and ships BGRA to the title over the control
   socket; the title presents and returns input. No GPU in the helper, no platform change. Cost:
   moderate, host-testable first. Covers a *Windows* GUI launcher beside a game. Risk: whether a
   headless Wine launcher's software path actually draws (Java2D unproven).
2. **Borrow a GPU owner by injection (A).** Gives drawing on screen today, with public tools
   (NineS/elfldr, kstuff), but it is a drawing surface inside a system process — *not* a second Wine
   client. Worth exactly one falsifier run, because the answer decides whether eligibility is a
   launch property (then only the shell can create a graphics process) or a kernel object
   (then route 3 becomes conceivable).
3. **Command broker (B2).** Helper writes GPU commands into a buffer the title submits. Needs a
   shareable GPU-visible buffer plus our guard relaxed; no precedent for a graphics client.
4. **Kernel-assisted grant (C).** Speculative; do not spend console time on it before A's falsifier.

**For the goal as stated** — a graphical launcher and a graphical game in one session — route 1 is
the only one that needs no new platform capability, and route 2 is the only one that could change
what the platform allows. They are not alternatives: 1 is the deliverable, 2 is the experiment.

## Experiment log

| When | What was tried | Result |
| --- | --- | --- |
| 2026-01-04 | `tools/gcprobe.c` as a payload (websrv `POST /elfldr`, `pipe=1`), firmware 10.01 | **A payload opens `/dev/gc` and the AGC device's first request succeeds with a genuine zero reply** |
| 2026-01-04, later | the same probe with two additions in one send: `fork()`, and a 2 MiB reservation inside the AGC pool range | **no output, then the console went down** — the sixth drop, cause unattributed |
| — | the same probe inside a helper and inside the title | not yet run |

### First result: the payload side of the cheap half

Sent as a payload, so the process is neither the title nor a Sony-managed helper
(`build/gcprobe.elf`, sha256 `5f52638005ddb94788e99f8938066d6d731f0ee949242bdf5bff22bc37160a44`):

```
gcprobe payload pid=1038 uid=1 euid=0 gid=1 egid=1
dev: gc / dev: dce / dev: dbggc / dev: dbggc_wfdebug      (/dev entries total=222)
stat /dev/gc -> 0
open /dev/gc O_RDWR -> 8
  #46 zeroed buffer -> 0 errno=0 reply=0
  #46 0xff buffer   -> 0 errno=0 reply=0
  bare 46           -> -1 errno=25 (Inappropriate ioctl for device)
allocate 1MiB direct -> 0 physical=0x20003c0000
```

1. **The device node is not app-only.** A payload — no app id of its own, not launched by the
   shell's app path — has `/dev/gc`, opens it, and gets `0` back from the AGC device's first
   request. PS5PCEM's model: *"a successful zero reply selects the retail compatibility path, which
   maps the small `/dev/gc` aperture and builds the driver's context."* The reply is genuine — the
   buffer was filled with `0xff` first and the kernel wrote `0`.
2. **`0xc004812e` is `_IOWR(0x81, 46, uint32_t)`**, the AGC device's first request, not a separate
   one: a bare `46` is a different request and the kernel answers `ENOTTY`. Our first probe's
   `ENOTTY` was our own encoding error, not a refusal.
3. **Second elimination.** The helper's `ENOENT` is not "GPU is only for app processes": a title has
   the device, a payload has the device, a Sony-managed helper does not. With app id, process type
   and VM identity already eliminated, what remains is the *process context* of an
   `AddLocalProcess` child — its devfs view, jail or credential — rather than a platform rule about
   applications.

A payload still cannot bring AGC up: the SDK ships no `libSceAgc` stub,
`sceSysmoduleLoadModuleByNameInternal` is undefined at link time for a payload, and
`ps5platform/agc.h` records that AGC arrives in a title by *linked import*, with `sceAgcInit`
succeeding once per process from the title's own executable. So the bring-up question stays with
the title and the helper, where it was measured.

### The run that took the console down, and what it changed

The next send added two operations to that working baseline at once: a `fork()`, and a 2 MiB
direct-memory reservation **inside the AGC pool range** `0xfe0000000`-`0xfe0200000` — the range
PS5PCEM's model says the driver's private pool must occupy. It produced **no output at all**, then
the console stopped answering entirely (no ping, FTP/klog/control/websrv closed, ARP entry
`INCOMPLETE`): the sixth drop of 2026-10-04, recorded in `tools/console_guard.py` as caused by my
run.

The cause is **unattributed**, and both readings fit: the payload ran and one of the two new
operations killed the console, or the console had already dropped on its own and the send simply
hung on `connect()` until its timeout. "No output" does not decide it either — a crash can take
in-flight stream data with it, so the absence of even the first flushed line is not proof that the
payload never ran. No klog window covers it: the drop took klog with it.

Two things changed because of it.

* `tools/gcprobe.c` selects its steps from argv, which websrv's payload launcher passes through as
  `args`: no arguments is the known-good device question alone, `args=child` adds the fork, and
  `args=pool` adds the reservation. Every risky step prints what it is about to do **before** it
  does it, so an operation that takes the console down still leaves its name in the stream. The
  pool step reserves **64 KiB and releases it** instead of seizing 2 MiB.
* The bisect order for the restarted console is fixed: device-only, then `child`, then `pool`, one
  send each.

**Why `child` matters most.** A forked child inherits its parent's devfs and jail. If it can open
`/dev/gc` and get the zero reply, then being a child process is not what disqualifies a helper —
its spawn context is, and parity is a spawn or mount change rather than a kernel patch. If the
child is refused, the disqualification is per-process and route C is the road. Thread 1 called
this measurement out as never made: the fd was only ever passed to another process, never opened
by a forked child.

## Experiments this research leaves ready to run

| # | Where | Experiment | What it would prove |
| --- | --- | --- | --- |
| 1 | host | Finish the static trace of a local process's profile: what SysCore sets at `0x18fa6`, and the ten kernel budget ids | whether a local process gets a different resource profile from an app process — free, no console |
| 2 | console | List `/dev` from inside a helper, then from a helper whose parent already opened `/dev/gc` | separates `ENOENT` from `EPERM`, and whether the node appears at all |
| 3 | console | Helper with VideoOut in the preload mask plus a real `sceAgcInit`, title holding no graphics client | the preload/association hypothesis; the one helper attempt never made |
| 4 | console | Relax only our `ERROR_BUSY` guard and log where a second in-session claim dies | our guard versus Wine's init-once versus `release_display`/`VK_ERROR_NATIVE_WINDOW_IN_USE_KHR` (`wine/patches/0460:106-110`) |
| 5 | console | Inject a logger into SceShellUI (NineS/elfldr) that tries `/dev/gc`, ioctl `0xc004812e`, `sceAgcInit` | route A's falsifier: is GPU eligibility a launch property or a kernel object? |
| 6 | host | Prototype route B1's framing (BGRA over the control socket) | whether the software-forwarding launcher works at all, before spending a console launch |
| 7 | payload + console | Compare the helper's process context with a payload's and the title's — devfs view, `fd_rdir`/`fd_jdir`, credential — now that a payload is known to have `/dev/gc` | whether the helper's wall is a namespace problem (its devfs lacks the node) or a permission problem, which decides between a spawn/mount change and a kernel grant |

## How the experiment is switched on

The path is opt-in through the Wine environment and the probe tools, for example:

```
WINE_PS5_CHILDREN=1  WINE_PS5_CHILD_METRICS=1  PROTON_LOADING_SCREEN=0
tools/build-graphics-process-probe.sh      # the graphics-child probe
tools/run-process-probe.py                 # host or console run of the process probe
```

A console run still goes through the project's rules: `tools/console_guard.py` gates every launch,
one console job at a time, the title ends through its own paths, and the signed production build is
redeployed afterwards.
