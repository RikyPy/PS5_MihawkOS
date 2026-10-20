> Historical snapshot of `docs/GRAPHICS_PROCESS_BROKER.md`, archived on 2026-10-07 from `9b07ebb`.
> Defaults, next steps, counts and failure classifications below belong to their original runs.
> Use the [current document](../../../GRAPHICS_PROCESS_BROKER.md) for present guidance; see the [archive index](../README.md).

# Graphics in separate subprocesses: GPU broker investigation

The console now has a tested route for **two separate Sony-managed helpers to
prepare GPU commands and observe GPU-written results**, using the original
title's GPU context. Separate native helpers now also create and recreate Vulkan devices and
pass complete offscreen image readback checks. Child images now reach the owning
title’s display. The experimental Wine/DXVK route also passed two rendering
children and two rendering descendants, with four independent native rendering
PIDs, 32 confirmed flips and verified pixels. This removes the one-graphics-client
restriction in the tested broker configuration. Clients now also get
device-local memory from the owner's 12 GiB direct pool, as a discrete GPU's
VRAM; what they share with the CPU stays bounded by the title's flexible
memory, which every helper shares and every shared byte costs twice (see the
high-water mark below). Input/audio, other graphics APIs and general game
compatibility remain unproved.

## What actually passed on 2026-10-04

`graphics-child-gpu-broker-shm-02` passed two simultaneously live native helpers,
three interleaved GPU submissions each, one deliberately malformed packet
rejected before submission, CPU alias teardown and Sony service closure.
Both helpers observed their own GPU-written completion values. The owner
only initializes the markers to zero; it never writes the requested completion
values on the CPU. Descriptors balanced at 12/12 and available direct memory
returned exactly to the warmed baseline. Zero fatal/GPU-fault records were
captured, and signed production was restored and the helper removed.

`graphics-child-gpu-broker-shm-window-01` repeated those checks with **two 32 MiB
shared allocations** placed in RADV's `0x200000000..0x300000000` shader-address
window. The last submission of each helper targets a marker near its
allocation's end, rather than just the first page. All seven verdicts passed,
both aliases unmapped, both services disappeared, descriptors balanced at
12/12 and direct memory returned to baseline. This proves GPU access at the
tested addresses within both allocations; it is not a full-memory stress test.

| Run | Result and boundary |
| --- | --- |
| `graphics-child-process-identity-01` | Headless lifecycle completed; inherited owner GPU-descriptor ioctl still returned EPERM in all three helpers before and after elevation. |
| `graphics-child-gpu-broker-native-01` | Owner direct-memory alias returned EACCES in an unelevated helper; no GPU work submitted. Cleanup balanced. |
| `graphics-child-gpu-broker-native-02` | Filesystem elevation succeeded, effective UID 0, virtual reservation succeeded, but the owner direct-memory alias still returned EACCES. Cleanup balanced. |
| `graphics-child-gpu-broker-shm-01` | Both shared aliases and all seven verdicts passed. Overall FAIL: first submission reduced available direct memory by 2 MiB. |
| `graphics-child-gpu-broker-shm-02` | PASS after a separate owner-only warm-up submission before taking the resource baseline. No remaining direct-memory change across the two helpers. |
| `graphics-child-gpu-broker-shm-window-01` | PASS with two 32 MiB shader-window allocations, including completion writes near both ends. |

The first-submission allocation is observed, not attributed to a particular
driver structure. Warming that path before recording the baseline separates
native submission initialization from per-client allocations. Shared mappings
are explicitly unmapped and their transferred descriptors closed; no claim of
a flexible-memory budget measurement is made by the direct-memory counter.

The identity probe's initial `sceKernelGetProcessType` declaration was not
verified. Its error-like raw returns do **not** establish process categories;
that call was removed. The guarded `sceKernelGetAppInfo` reads did show the
same application ID, with header word at offset 4 equal to 1 for the native
owner and 0 for its helpers. The meaning of that difference remains unproved.

## Native child Vulkan image operations (2026-10-05)

`graphics-child-gpu-broker-vulkan-03` passed two distinct native helpers
(PIDs 130 and 133), each creating, destroying and recreating its own Vulkan
instance/device. Four 64×64 RGBA8 offscreen images were cleared and copied
back: red, green, magenta and cyan. Every one of the 4,096 pixels per image
matched; the owner independently checked each full-image hash. The native
owner submitted four 627-word RADV command streams, interleaved across clients.

Both helpers closed through Sony's service and disappeared. Descriptors
balanced at 12/12; available direct memory was exactly 12,863,864,832 bytes
before/after, flexible memory exactly 434,110,464 bytes before/after. Each
helper retained RADV's 16 MiB command ring plus 16 KiB marker page until
process closure (16,793,600 bytes), after which the owner released the shared
backing. Zero fault records; signed production restored; helper and client
module removed. This run proves image operations and device recreation; the following run
adds a shader-pipeline draw. Neither establishes onscreen presentation or
Wine graphics children.

`graphics-child-gpu-broker-draw-01` also passed two native helpers (PIDs 140
and 143), each building a graphics pipeline with independent RADV shader
compilation. Each device drew a fullscreen triangle using synthetic vertex
and fragment shaders and a push-constant colour, copied the image back and
verified all 4,096 pixels. Devices were destroyed/recreated; four draws and
four full-image checks passed, interleaved across the two processes. Each
submission held 635 words. Maximum observed per-client shared allocation
was 17,104,896 bytes. Both clients' GPU queue allocations remained until
service closure, then released. Descriptors 12/12, direct and flexible memory
exactly the same before/after as the image test, zero fault records,
production restored, synthetic helper and module removed. The SPIR-V is
repository-owned synthetic code, reproducible with the pinned glslang 16.4.0
binary hash in `tools/generate-gpu-probe-shaders.py`.

The first run lacked the current boot's elevation service and reached no
client initialization; it closed both helpers and restored production cleanly.
The second loaded the client PRX but hit a user-process SIGSEGV in the SDK
heap during RADV constructors. The optional weak-entry-point linker script
had also zeroed real `pthread_mutexattr_init/destroy` imports. The corrected
client link excludes actual SDK exports from that script. The passing run
contains real relocations for the mutex-attribute functions. No kernel panic
was observed in these runs; the second run's truncated results cannot prove
resource balance.

The experimental client uses the pinned title SDK and the unchanged
PS5_Vulkan RADV link recipe. It replaces the native GPU platform and VideoOut
archive objects; its only native dependencies are ordinary libkernel and libc.
A small pinned Mesa winsys patch disables sparse resources, capture/replay and
external-host/user-pointer memory. That earlier offscreen build has no display
targets; the later Wine build adds the display-plane WSI transport below.
A 128 MiB budget per client, 128 allocation slots and monotonic address cursors
bound this synthetic experiment; this is not the general runtime allocator.
Timestamp submissions are deliberately rejected and timestamp query support
has not been implemented. Do not install this experimental driver as the Wine
runtime's general Vulkan provider.

Startup is serialized because the existing elevation service has one request
pathname. Each helper elevates while single-threaded, loads the signed PRX
from its root-visible title directory, then establishes the owned socket.
Allocation descriptors are transferred with the already-tested process-I/O
transport. The broker validates ownership/ranges, copies command bytes into
owner-private staging, applies the client's tessellation settings and submits.
Only GPU completion permits staged commands and backing to be freed. A submission error or missing
completion retains backing and asks the runner not to force-close the title.
General PM4 commands are accepted only from our signed synthetic client; this
is not a complete command-stream validator for untrusted clients.

## Mechanism

1. Sony's validated local-process service creates a fresh helper using the
   existing headless preload policy `0x8000000000000002`. No direct `rfork`,
   new spawning payload or guessed kernel mutation is involved.
2. The original title creates an anonymous `shm_open(SHM_ANON)` object, sizes
   it, maps it with `MAP_SHARED`, and gives **its own mapping** GPU read/write
   protection through the existing RADV platform operation.
3. The title transfers the shared-memory descriptor over its owned
   `SOCK_SEQPACKET` socket using `SCM_RIGHTS`. The helper reserves the same
   numerical virtual address and maps the descriptor there with CPU-only
   read/write access. It never overwrites an unreserved mapping.
4. The helper constructs commands in those shared pages. The title validates
   the exact known eight-word RADV completion packet, flushes CPU caches and
   submits it through its original AGC context.
5. The GPU writes a completion value into the shared pages. The helper
   invalidates its cache line and checks that value. Cleanup unmaps the
   helper, closes its service and confirms absence before freeing backing.

The helpers import ordinary libkernel and, for the elevation variant, libc.
They have no AGC or VideoOut dependency and never create a native GPU VM.
Their existing filesystem elevation uses the same cooperative client as Wine;
it does not supply the missing GPU permission. Removing helper elevation from
this shared-descriptor route has not been tested.

The probe accepts only a fixed, already-used Mesa completion packet. It is
**not** a general PM4 forwarding service. A missing GPU completion or unknown
live-helper cleanup state retains all backing and stops normal teardown;
the run tool must not force-close or replace that title.

## How to turn this into graphics for every supported Wine subprocess

Keep each Windows application's own Wine and RADV state in its separate native
process. Replace RADV's PS5 platform operations with requests to a GPU broker
in the owning title. This places the boundary below Vulkan, avoiding a new
marshaller for every Vulkan command and structure.

```text
Windows launcher -> Windows graphics child A -> graphics grandchild B
  separate Wine/native processes; separate Vulkan/RADV client state
                 | shared allocations + submission requests
                 v
original native title: GPU memory/addresses, AGC submission, VideoOut
```

The remaining integration sequence is:

1. **Broader offscreen rendering.** The client transport at
   `radv_ps5_platform`, owner allocation, matching CPU aliases and submissions
   now work for both the image and shader-draw probes. Broaden coverage to
   textures, depth, compute, synchronization and larger allocations. Build the client driver with PS5_Vulkan's pinned
   link recipe and a repository-owned overlay; never edit the sibling Mesa
   checkout or hand-link RADV. Two helpers already create/recreate devices and verify different cleared
   and shader-drawn images. Native presentation and focused Wine/DXVK
   presentation now pass; the broader GPU coverage remains open.
2. **Global address and lifetime management.** One broker owns GPU addresses
   across every client, including the 32-bit shader-address window. Never
   free backing until all GPU uses complete, including client crashes and
   Windows termination. Serialize native submission and bind the client's
   tessellation state for each submission. The owner's local graphics path
   must use the same arbitration.
3. **Display and Windows presentation.** Keep one native VideoOut owner.
   Children render to broker-backed images; the owner presents or composites
   them and routes focus/input to the relevant Windows process. Validate two
   visible clients, display handoff, teardown and a replacement client in the
   same title session. Native presentation and four Wine/DXVK rendering
   processes now pass; focus/input and composition remain open.
4. **Runtime compatibility.** Wire the transport through ordinary and
   descendant Wine spawn requests, then test two DXVK children and their
   graphics descendants (the focused DXVK test now passes). DXVK and VKD3D share the Vulkan boundary; the native
   OpenGL backend needs its own adaptation and is not covered automatically.
   Retain the direct Java-game workaround until the Java launcher → Java → game chain
   passes rendering, exit and relaunch.

Anonymous shared memory is charged to the title's flexible memory and
direct memory cannot be shared between processes; the measurements and the
resulting split between device-local and host-visible client memory are in
the sections below. Shared/placed memory,
sparse resources, external-host memory and capture/replay must either be
implemented correctly or disabled truthfully in the client driver. Do not
claim the existing driver's complete feature set on an incomplete backend.

This architecture removes the need to nominate one particular Windows child
as the original title's sole graphics client. It still has finite hardware,
memory and native-process limits. “Any subprocess” means graphics can follow
any supported process in the tree, not unlimited concurrent applications or
automatic compatibility with every graphics API.

## Reproduction

```sh
python3 tools/verify-service-spawn.py \
  --system '<your firmware dump>/system' \
  --reference evidence/graphics-child-gpu-lifecycle-static-10.01.json
# 3 pinned local inputs; 24 checks. Selected metadata/call edges only.

python3 tools/build-child-probe.py --kind service-native --elevate-gpu-helper
# Use the printed isolated build directory in the next command.
python3 tools/run-child-probe.py --kind service-native --mode gpu-broker-shm \
  --gpu-bytes 33554432 --gpu-window32 --build BUILD_DIRECTORY \
  --name UNIQUE_RUN_NAME
```

The runner verifies the idle console, signed deployment identities and helper
dependencies; it captures fresh logs and restores production afterward.
For the separate native Vulkan image probe:

```sh
python3 tools/build-child-probe.py --kind service-native --vulkan-gpu-helper
python3 tools/run-child-probe.py --kind service-native --mode gpu-vulkan \
  --build BUILD_DIRECTORY --name UNIQUE_RUN_NAME
python3 tools/build-child-probe.py --kind service-native --draw-gpu-helper
python3 tools/run-child-probe.py --kind service-native --mode gpu-draw \
  --build DRAW_BUILD_DIRECTORY --name ANOTHER_UNIQUE_RUN_NAME
```

Raw captures stay under ignored `klog/`; `evidence/` holds sanitized run
metadata, source/archive hashes and explicit pass/fail results.

The subsequent normal-child regression request could not reach the console
and performed no deployment or launch. The user confirmed an intentional
shutdown after the passing large-buffer probe. This was not a kernel panic
report. Production restoration and helper removal were verified before the
shutdown; the subsequent fresh-boot native four-cycle regression and 120-frame
restored launcher both passed (`graphics-child-gpu-broker-native-regression-02`,
`graphics-child-broker-restored-production-03`).

Static inspection also established that the 10.01 `GetAppInfo` getter requests
88 bytes while its setter consumes 144 bytes. A getter snapshot is not a safe
setter input or rollback image. Neither that setter nor guessed canvas,
budget, VM-container or process-property mutations were invoked. In
particular, the `SetProcessProperty` export belongs to `libkernel_psmkit`,
not the ordinary `libkernel` import namespace.

The restored production launcher passed 120 frames after the shader draw run
(`graphics-child-broker-restored-production-04`), with its signed executable
unchanged. No probe helper or client module remains installed.

`graphics-child-gpu-broker-draw-02` repeated all four shader draws and pixel
checks successfully with the final submission-error retention policy. This
run again reported two distinct helper PIDs, four device creations, 12/12
descriptors, unchanged direct/flexible memory, zero faults and verified
production restoration. No actual failed hardware submission was injected;
the retention branch itself remains a conservative failure policy, not a
console-tested error recovery claim.


## Owner presentation (2026-10-05)

`graphics-child-gpu-broker-present-03` passed two native rendering helpers,
four recreated Vulkan devices and four shader draws, followed by four owner
VideoOut flips. Each completed shared 64×64 RGBA image is validated against
its independent full-image hash, copied into an owner-only tiled 1920×1080
buffer and flipped. `sceVideoOutGetFlipStatus` confirmed arguments 1–4.
Every pixel of all four captured scanout buffers matched the child’s colour
(2,073,600 pixels each). These are copies of the confirmed scanout buffers,
not camera photographs or an independent capture of the HDMI signal.

The first complete presentation run (`present-02`) showed correct frames and
released direct/shared memory, but its descriptor count changed from 12 to
14. A full owner-only display warm-up before the next baseline also changed
12 to 14; the subsequent child display lifecycle then balanced at 14/14.
Direct memory stayed at 12,863,864,832 bytes and flexible memory at
434,110,464 bytes. Both helpers disappeared, blank flip/unregister/close and
memory release returned zero, no fault records were captured, and the exact
signed production title was restored. `present-01` timed out before any
deployment; the normal control payload had exited on an accept error. A
locally patched payload retries up to eight consecutive accept errors and
allowed the following runs to complete.

This first presentation path performs a synchronous CPU copy and nearest
neighbour scaling after completed GPU work. It establishes display handoff
between separate rendering processes, without native VideoOut in a child.
It is not a performance claim or a zero-copy scanout implementation.

The experimental Wine route adds a dedicated GPU socket to every child,
including children created by descendants. The owning title serializes
native submissions and presentation; Wine’s shared server retains Windows
parentage, handles, inherited pipes and exit codes. A patched Mesa CPU WSI
backend exposes a broker-backed display-plane swapchain and copies finished
optimal images into shared linear buffers. The loader and runtime use a new
private bootstrap protocol version and must be deployed together. The route
is opt-in, bounded to 16 broker slots and 128 MiB shared GPU backing per
client; input/audio forwarding and game-scale allocation policy remain work.
`graphics-child-gpu-broker-wine-descendants-03` passed the headless Wine
regression with this transport: three descendant node verdicts, five native
helper closures, zero fault records and exact production module/title
restoration. No child initialized the Vulkan driver in that run, so its GPU
connections remained unused. The rendering validation follows below.
The earlier Wine run (`wine-descendants-02`) exited cleanly before spawning
any child because its owner log used `/app0` after elevation. It was fixed by
using the existing `proton_title_path()` resolver; production was restored.
`wine-descendants-01` refused deployment because the available build’s driver
backup differed from the installed production module. The matching signed
staging image was extracted with the pinned plaintext SELF tool and verified
against every installed ELF executable segment before subsequent replacement.

## Wine/DXVK children and graphics descendants (2026-10-05)

`graphics-child-gpu-broker-dxvk-descendants-06` passed a headless Windows
launcher starting two separate D3D11/DXVK rendering children. Each retained
its GPU device and swapchain while starting another rendering child. Four
distinct native rendering PIDs (346, 349, 352, 355) each drew eight 640×360
frames through independent Wine/RADV devices. The owner submitted 24 completed
streams per renderer (96 total) and confirmed 32 VideoOut flips. All 16
1920×1080 scanout-buffer captures and four 640×360 guest readbacks passed
the five independent background/triangle colour samples. These are sampled
pixel checks, unlike the native colour-image test's full-image check;
the captures are not independent HDMI captures.

All four Windows parent/environment/inherited-pipe checks passed. Both
rendering-descendant creation/wait/exit-code checks passed while their
parents' devices remained alive; all four explicitly released their D3D11
devices and swapchains. The Windows launcher's handle count balanced at
14/14. All six native helpers (launcher, auxiliary Wine helper, four renderers)
disappeared through Sony-managed closure. The broker released each renderer's
remaining 2,113,536-byte command-ring/marker backing only after absence,
blanked/unregistered/closed VideoOut with zero status, and stopped after 32
flips. No native fault records or broker-operation errors were captured.
Signed production title, ntdll, wineserver, win32u and libvulkan were restored
and read back; synthetic helpers, PEs and captures were removed.

The measured peak of the broker's live shared allocations was 98,828,288
bytes. It is a broker backing count, not the entire application's flexible
memory consumption. The client command ring is now 2 MiB, with both its
allocator and command-stream bounds rebuilt from the same pinned header;
the existing separate allocation for a stream too large for the ring remains.
The owner continues using the original native RADV recipe and ring. Broker
helpers use a 4 MiB native bootstrap stack; Wine's Windows/Unix thread stacks
remain its own. All six reported successful native thread creation.

Wine patch `0928` disables the fixed-VideoOut fullscreen blit only when the
native broker environment is enabled: the owner scales the actual guest image
instead. The focused process probe embeds our own HLSL's DXBC, generated by
the pinned host Wine/compiler digest in `tools/generate-d3d-probe-shaders.py`.
It does not load the HLSL compiler's incidental wined3d/OpenGL dependencies.
That narrows this test to DXVK rendering; runtime HLSL compilation and OpenGL
children are not established by it. The opt-in runtime uses `--no-opengl`.

Earlier rendering attempts are retained as failures:

| Suffix of `graphics-child-gpu-broker-dxvk-descendants-` | Observed boundary |
| --- | --- |
| `01` | Both Vulkan device creations caught a null device-procedure call; no GPU submissions or frames. The exported device lookup incorrectly used instance lookup with a null instance. Production restored, zero native faults. |
| `02` | Corrected device lookup: two DXVK children each submitted 32 streams and eight flips, but the forced fullscreen blit selected a 1×1 output. Descendant initialization/spawning failed under memory pressure. Production restored, zero native faults. |
| `03` | Broker-aware Wine sizing and lean shader probe: two children and one rendering descendant passed sampled displayed pixels (24 flips). Another descendant's initial 16 MiB GPU allocation returned ENOMEM. Production restored, zero native faults. |
| `04` | Bounded 640×360 windows: two children and one descendant drew (24 flips); another helper failed before starting its Wine worker. Production restored, zero native faults. |
| `05` | 4 MiB bootstrap stacks: all six workers started, but the fourth renderer's 16 MiB shared ring mapping failed with only 7,340,032 flexible bytes available. Three renderers drew 24 flips. Production restored, zero native faults. |
| `06` | 2 MiB client rings: all four renderers, both graphics-descendant chains, 32 flips, pixel checks and restoration passed. |

This remains an experimental allocator and synchronous CPU-copy presenter.
Shared buffers consume the application's finite flexible-memory budget;
128 MiB per-client limits and 16 broker slots do not imply that all those
clients can consume their maxima together. A game-scale backing strategy,
input/audio routing, timestamp support, textures/depth/compute coverage,
VKD3D and OpenGL integration are subsequent work. Keep the direct Java-game
workaround until the complete Java launcher → Java → game sequence passes.

## Shared-memory high-water mark (2026-10-05)

`gpu-memory-ceiling-01` measured how much memory one helper can share with
the owner's GPU, in two arms, in the same title session. Arm A is the
broker's current backing: `shm_open(SHM_ANON)` objects in 16 MiB chunks,
mapped and granted GPU access by the owner, mapped at the same address by
the helper, every page written by both. Arm B is device-local backing:
256 MiB direct-memory allocations mapped with GPU access by the owner, with
the helper only reserving the same numerical range (no CPU access, the same
position as the refused direct alias above). After each chunk the helper
builds the known completion packet inside it, the owner submits it, and both
sides read the GPU-written marker.

| Arm | Stopped at | GPU checks | Pool it draws from |
| --- | --- | --- | --- |
| Shared (`shm_open`) | 10 chunks, 167,772,160 bytes, the probe's 96 MiB flexible reserve | 2/2 (first and last chunk) | Flexible: 434,110,464 bytes at baseline |
| Direct (owner-only CPU) | 32 chunks, 8,589,934,592 bytes, the probe's 8 GiB cap | 32/32 (every chunk) | Direct: 12,863,864,832 of 12,884,901,888 bytes |

**Every shared byte is charged twice.** Each 16 MiB chunk took 16 MiB of
flexible memory when the owner mapped it and granted GPU access, and another
16 MiB when the helper mapped it (owner: 430,964,736 → 414,187,520 →
397,410,304 bytes). The helper's own counter fell in lockstep (411 → 379 →
347 MiB): the title and its Sony-managed helpers draw from **one** flexible
budget. Ten chunks (160 MiB shared) consumed 320 MiB of it.
`gpu-memory-ceiling-02` repeated the shared arm with the owner never writing
its pages: identical numbers, so the owner's charge comes from mapping plus
the GPU grant, not from CPU writes. Direct chunks left flexible memory
untouched and returned direct memory exactly; the direct arm's last chunk
left 4,271,898,624 bytes available.

Both runs restored flexible and direct memory and descriptors (12/12)
exactly, showed zero fault records, removed the helper and restored signed
production.

What this decides:

- `shm_open` backing cannot host game memory. Every client, Wine's own
  native heaps and the title share roughly 414 MiB, and shared GPU memory
  costs two bytes of it per byte. `dxvk-descendants-05` already ran into
  this limit with four renderers.
- Direct memory scales to gigabytes with GPU access, but the client cannot
  touch it with the CPU. That is the memory model of a discrete GPU. Give
  each client a large **device-local, not host-visible** heap from the
  owner's direct pool, and a small **host-visible** heap from `shm_open` for
  staging, uniforms, descriptors and command rings. DXVK and VKD3D already
  target that topology (desktop GPUs without resizable BAR).
- A client never receives a CPU mapping of device-local memory, so a
  client-side `vkMapMemory` of it is impossible by construction, not merely
  refused.

### Neither process can map the other's direct memory

`gpu-memory-alias-01` tried every remaining route to CPU-visible direct memory
in a client. The helper can allocate direct memory of its own and map it for
its CPU: it received physical offset `0x1600000`, the same offset the owner
had just released, so the title and its helpers draw on one physical pool
(16 MiB took 18,808,832 bytes from the owner's available figure). But every
cross-process mapping was refused with `0x8002000d` (EACCES), in both
directions and in all seven variants: the helper mapping the owner's direct
memory read-write at a fixed address, read-write anywhere, or read-only; and
the owner mapping the helper's memory CPU+GPU fixed, CPU+GPU anywhere, CPU
only, or GPU only. Direct memory belongs to the process that allocated it.
Memory, descriptors (12/12), helper and production were all restored.

### What a shared mapping costs

`gpu-memory-charge-01` shared five fresh 16 MiB chunks, logging the flexible
figure after every step. In all five variants the `shm_open` object and its
`ftruncate` cost nothing; the owner's `mmap` cost the full 16 MiB at once,
whatever its protection and whether GPU access was granted afterwards; and
the helper's `mmap` cost another 16 MiB at once, even when the helper never
touched a page. Each unmap returned its 16 MiB. A mapping the owner gives
GPU access only (`gpu_only`, no CPU access) and a CPU-read-only one
(`cpu_r_gpu_rw`) both carried GPU writes the helper read (4/4 GPU checks).
So the double charge is a commit charge per mapping, taken at `mmap`; it
cannot be reduced by protections or by leaving pages untouched. Shared
memory is worth using only for what a client must touch with its CPU.

## Device-local client memory (2026-10-05)

The broker now gives clients the memory model of a discrete GPU without a
CPU window on its memory:

- **Device-local** (`VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT` only): the owner
  allocates direct memory with `radv_ps5_memory_alloc` and maps it for its CPU
  and GPU. The client receives the address and nothing to map. RADV's
  `RADEON_FLAG_NO_CPU_ACCESS` buffers go here, which on any discrete GPU are
  never mapped by RADV itself. The broker zeroes it on request
  (`RADEON_FLAG_ZERO_VRAM`) since the client cannot.
- **Host-visible** (the GTT heap): shared memory as before, at most
  `PW_GPU_LIMIT` (128 MiB) per client, for command streams, staging and
  everything else the client writes.

The INIT reply now carries the client's device-local and host-visible limits.
A broker that offers no device-local bytes leaves the client exactly as
before: one unified host-visible pool. The owner offers each client up to
8 GiB and refuses any device-local allocation that would leave less than
1 GiB of direct memory for the title and every process's own memory.
`tools/patches/radv-gpu-broker-local-memory.patch` (applied to the overlay copies
of the pinned winsys sources only) reports the heaps and routes the buffers.
The Wine route enables it with `PROTON_GPU_LOCAL=1`; the native probe with the
third field of `gpu-broker-settings.txt` (`--gpu-local`).

| Run | Result |
| --- | --- |
| `graphics-child-gpu-local-draw-01` | Two native helpers, four shader draws, every pixel of four 64×64 images verified; each client's render target lived in the owner's direct memory (16,384 bytes, offered 8 GiB). Descriptors 12/12, direct and flexible memory exactly restored, zero faults. Overall FAIL only because klog lost the title's exit line; the capture shows `Kill for LoadExec ... => 0` and the process deleted. |
| `graphics-child-gpu-local-draw-02` | The same, PASS, exit line seen. |
| `graphics-child-gpu-local-dxvk-01` | The owner title crashed with SIGSEGV before any client connected: a write just below its stack pointer. Growing the per-client buffer table to 1,024 slots made a client 76 KiB, and `*c=Client{}` built that temporary on the stack of the Wine thread opening a broker slot (frame 0x12A80 bytes). Fixed by resetting clients in place. Production restored. |
| `graphics-child-gpu-local-dxvk-02` | Two DXVK children and two rendering descendants, four distinct renderers, 8 frames each, 32 flips; 16/16 scanout captures and 4/4 guest readbacks passed their samples; each renderer used 7,143,424 device-local bytes. Zero faults or transport errors. Runner FAIL only because the title-end pattern raced the relaunch; the title logged `ends: status 0` after returning to the launcher. |
| `graphics-child-gpu-local-dxvk-03` | The same configuration, PASS throughout. Shared peak 90,161,152 bytes, device-local peak 28,573,696 bytes, lowest free flexible memory 26,214,400 bytes. |

A renderer's buffers in `dxvk-02`, by kind: 25 host-visible allocations of
22,773,760 bytes (the largest 8, 4, 4 and 2 MiB, then three 933,888-byte
swapchain buffers) and 4 device-local ones of 7,143,424 bytes (a 4 MiB chunk
and three 983,040-byte swapchain images). At this test's 640×360, DXVK keeps
most of its memory host-visible, so the shared peak fell only from
98,828,288 to about 90 MB. The device-local path matters for what a real
game puts there: textures, render targets and static geometry.

### The per-process budget

The broker now logs free flexible memory when it opens a slot for a child,
at that child's Vulkan INIT, and when it closes it (`dxvk-03`). The non-rendering
Windows launcher process cost about 15 MB. Each rendering process cost
47–49 MB between its slot opening and its Vulkan INIT, before any GPU
allocation, and then its shared GPU memory cost twice its size again (about
45 MB here). One DXVK renderer at this size is therefore about 95 MB of the
roughly 414 MB that the title and every helper share, and four of them took
the pool down to 26 MB free. The broker's client module is 24 MB of loaded
segments (8 MB code, 15 MB read-only data); the measurements agree with each
renderer receiving a private copy, but that attribution is inferred, not
measured per segment.

What this means for scope:

- One game renderer in a helper, beside a launcher, has roughly 300 MB of
  flexible memory, about 150 MB of host-visible GPU memory after the double
  charge, and gigabytes of device-local memory.
- Several renderers at once are bounded by the flexible pool, not by the
  GPU: about four at this test's size.
- Further savings must come from what clients map: swapchain buffers moved
  device-local (the owner reads them through its own mapping; three 1080p
  buffers are about 24 MB, 48 MB of flexible memory once charged twice), and smaller host-visible heaps for clients that do not need them.
- The kernel logs every process of the title with `FMEM size: 0x1c000000`
  (448 MiB) beside `DMEM size: 0x300000000` (12 GiB). That size can be
  raised to 1 GiB through the title's `param.json`; see
  "A larger flexible pool" below.

## A larger flexible pool (2026-10-05)

**The executable's memory parameters do not set it.** PS5_Vulkan's native
tool already writes a memory-parameter block (`0x38` bytes) that the process
parameters point to at `+0x40`, by relocation. An experimental build of the
tool (PS5_Vulkan branch `experiment/flexible-memory`) filled its `+0x10`
field, `sceKernelFlexibleMemorySize` in the PS4 layout, with 1 GiB: by
relocation, with the pointers also stored in the file at the `0x400000` load
base, and image-relative (`flexible-memory-request-01` to `-03`). In all three
the kernel still logged `FMEM size: 0x1c000000` and the title started with
433,061,888 bytes of flexible memory free. Without its options the tool's
output is unchanged byte for byte; the branch is kept as a negative result.

**`param.json` does.** `SceShellCore` (FW 10.01) reads `kernel/flexibleMemorySize`,
beside `kernel/cpuPageTableSize`, `kernel/gpuPageTableSize` and
`kernel/extendedDirectMemorySizeInMiB`, from the title's `param.json`, which
the console reads from the title folder at each launch. The run tools'
`--param-kernel KEY=VALUE` puts a changed copy there for one run, over the
production file only, and always puts the original back and checks it.

| Run | `kernel.flexibleMemorySize` | Result |
| --- | --- | --- |
| `flexible-memory-param-01` | 1,073,741,824 | `FMEM size: 0x40000000` and `DMEM size: 0x2dc000000` for the title and its helper; 1,037,041,664 bytes of flexible memory free at the title's start (was 433,061,888), direct 12,259,885,056 (was 12,863,864,832). GPU checks 4/4. |
| `flexible-memory-param-02` | 2,147,483,648 | Launch refused (`0x80020016`): the kernel logs that the property must be between 2,097,152 and 1,073,741,824. An error dialog was left on the console's screen. |
| `flexible-memory-param-03` | 1,073,741,824 | The shared-memory fill reached 28 chunks, 469,762,048 bytes, before the 96 MiB reserve (was 10 chunks, 167,772,160 bytes). GPU checks 2/2. |
| `graphics-child-flexible-1g-dxvk-01` | 1,073,741,824 | The four-renderer Wine/DXVK and input test passed; the lowest free flexible memory in the run was 628,097,024 bytes (24–26 MB with the default). |

Every one of these restored `param.json` and production, checked by
readback, with no fault records. The 576 MiB added to the flexible pool came
out of the direct pool: the total the kernel gives the title stays the same.
1 GiB is the kernel's maximum. The production `param.json` now asks for it
(`main` a5f2dbe): the title trades 576 MiB of direct memory, which games in
the title itself also use, for room for more and larger child processes.
With it deployed, the production launcher passed 120 frames, its picture and
log checks and a clean close, the kernel logging `FMEM size: 0x40000000`
(`production-flexible-1g-01`).

## CPU–GPU coherence across processes (2026-10-05)

`graphics-child-gpu-coherence-local-01` and `-unified-01` checked what the
client's host-visible memory types promise (`HOST_COHERENT`), with no cache
maintenance of any kind: no flush, no invalidate. Each client's CPU writes a
pattern with plain stores into a host-visible buffer, first dirties every
line of a second buffer with `0xa5a5a5a5`, then has the GPU upload the
pattern into its image and copy the image into the second buffer; the CPU
reads that. Both runs passed 8/8 checks (two clients, two cycles, the
write-combined and the cached type), 4,096 of 4,096 words each, with the
image in device-local memory in the first run and in the unified pool in the
second. The GPU sees a client's unflushed writes through the owner's mapping
of the same pages, and the client sees the GPU's writes over its own dirty
lines. The memory types are therefore advertised truthfully, and a client
may wait on its own completion marker in shared memory.

## Submission throughput (2026-10-05)

Each native client now times 30 submissions of a small fill, waiting for each
(serial) and with two in flight (pipelined), while the other client does the
same. Measured round trip per submission, GPU work included:

| Run | Broker | Per submission |
| --- | --- | --- |
| `graphics-child-gpu-sync-timing-02` | synchronous, a log line flushed to disk for every operation | 193,000–202,000 µs |
| `graphics-child-gpu-sync-timing-04` | synchronous, operation lines only when tracing | 4,490–6,620 µs |
| `graphics-child-gpu-async-21` | asynchronous fences, peek-driven serving, log through a pipe | **124–144 µs** |

What made the difference, each measured on the console:

1. **Disk writes.** The broker wrote and `fsync`ed a line per operation, two
   per submission; a submission's broker time was about 40 µs of work and
   about 190 ms of logging. Operation lines are now written only with
   `PROTON_GPU_TRACE=1`; peaks are kept in memory and logged at the end.
2. **Asynchronous submission.** The broker copies the stream into
   owner-private staging, appends its own end-of-work packet writing a
   sequence number to an owner-private fence page, submits, and replies at
   once. Staging and any buffer a client frees while work is in flight are
   released only once the fence has passed the last submission that could
   use them; changing the tessellation ring or off-chip state, which every
   client shares, first waits for all work in flight. The client waits on
   its own marker, which the coherence result above makes safe.
   `async-21` issued 372 fences, all completed, with no drain needed.
3. **Waiting by peeking.** Both ends now look for a message with a
   non-blocking `recv(MSG_PEEK)`: continuously for a short window after the
   last one (the broker 1 ms, the client 2 ms, yielding the CPU), then after
   a 1 ms `poll` that only bounds the wait. This was adopted while `poll`
   was suspected: with a peek beside it, 170 of one client's 186 requests
   were found by the peek after `poll` had returned without reporting them
   (`async-11`). That count includes requests that arrived between `poll`'s
   return and the peek, and the stalls then blamed on `poll` were item 4's,
   so `poll` misbehaving is **not** established. Whether blocking `poll`
   alone now performs as well has not been re-measured.
4. **No disk write under the broker lock.** With the rest fixed, one client
   still averaged 2.3 ms per submission in some phases. The cause was the
   other client's report lines: written to disk under the broker lock, single
   writes took up to 262 ms (`async-18`), and one such stall spread over a
   30-submission phase. Every line the broker writes during service now goes
   through a pipe a thread of its own drains to the file
   (`LogPipe`); lock waiting over 362 messages fell from about 2 s to 13 ms.

The broker serves each client on a thread of its own, the work serialized by
one lock held only while a message is handled (about 40–110 µs for a
submission). The kick that starts a submission promptly (`sceAgcSuspendPoint`,
`ps5platform/agc.h`) is made by the serving thread after its reply is sent:
372 kicks took 6.8 ms in all. The native probe's serving threads start before
its resource baseline and take their heap arena first, as the title heap
keeps one per allocating thread for the life of the process.

`graphics-child-gpu-async-dxvk-01` ran the four-renderer Wine/DXVK test on
this broker, with device-local memory: every check passed (4 metadata,
4 teardown and 2 descendant verdicts, 32 flips, 16/16 scanout captures and
4/4 guest readbacks), 96 fences with no drain, zero transport errors and
fault records, production restored. The run took 18.1 s against 35.9 s for
`dxvk-descendants-06`; process and Wine startup are most of what remains.

### Presenting a client's frame

The owner still presents by copying a client's finished frame with the CPU
into a tiled 1920×1080 scanout buffer. Timed on the console in the native
present test, that cost 10.4–18.2 ms a frame
(`graphics-child-gpu-present-timing-01`), held under the broker's lock: a
game in a child could not pass about 95 frames a second, and each of its
frames stalled every other client. The presenter now writes each 64 KiB tile
in memory order through a precomputed inverse of the tiling, scales through
tables instead of two divisions a pixel, flushes with `clflushopt` as each
tile is finished, and rotates three buffers, so it waits only when presents
outrun the display. A present then took 4.0–4.4 ms, of which the copy was
4.0 ms (`present-timing-02`, every flip confirmed shown and every captured
buffer's pixels checked). In the Wine broker the copy runs under a display
lock of its own, outside the broker's, and without flushing the client's
frame, which the coherence result makes unnecessary.
`graphics-child-gpu-async-dxvk-02` passed the four-renderer test on it
(16/16 captures, 4/4 readbacks, each 640×360 → 1080p copy about 3.1 ms).
A zero-copy path, the client rendering into a scanout buffer the owner
flips, remains the way to remove this cost.

## Shared GPU state and the address contract (2026-10-05)

**State every client shares.** Of the RADV platform operations a client can
call, two program state AGC holds for the whole title: the tessellation
factor ring (`sceAgcDriverSetTFRing`) and the off-chip parameter. The broker
records each client's values and, before a submission that needs different
ones, waits for all work in flight, then sets them. Everything else a client
programs (scratch, shader rings, context and uconfig registers) is in its
command streams: the client winsys copies RADV's initial preamble at the head
of every stream it submits, so each submission re-establishes its own state.
This is an audit of the code, not a console test: no probe has used
tessellation.

**Addresses.** Host-visible client buffers must sit at the same address in
the owner and in the client. The broker used to take them from two cursors
that only moved forward, the high one inside the owner RADV's own
device-memory region, and each client reserved its buffer's range only when
the buffer arrived, by which time Wine might have mapped something there
(kernel-placed mappings land in the GPU window). Now two fixed ranges
(`PW_GPU_HOST_*`: 512 MiB at `0x240000000` in the window, 16 GiB at
`0x3000000000`) are reserved whole by the owner when its broker starts and by
each child before Wine loads (the child loader, and the native helper), never
over anything already there. The owner hands out ranges first fit and gives
them back coalesced; a client maps each buffer over its own reservation and
restores the reservation when it is freed. A process that could not reserve
them falls back to the old per-buffer path.

`graphics-child-gpu-ranges-02` found which 16 GiB bases were free in the
title and in its helpers: `0x2000000000` was taken in the title (status
`0x8002000c`), 1 TiB is outside user space (`0x80020016`), and `0x1000000000`,
`0x3000000000`, `0x3c00000000`, `0x7c00000000` and `0x8000000000` were free in
both. With the high range at `0x3000000000`, `ranges-03` (native) passed with
both ranges reserved in the owner and both helpers, 40 buffers taken and all
given back into one extent per range. `graphics-child-gpu-ranges-dxvk-01`
passed the four-renderer Wine/DXVK test with all six children reserving
before Wine started: 80 window and 20 high buffers, all given back, window
high-water 62.9 MB (DXVK through RADV places about 15 MB a renderer in the
32-bit window, which bounds how many renderers the 512 MiB range serves).
Neither run had a Windows program map memory where the broker's ranges are,
so the conflict this prevents was not reproduced.

## The title's controller in child processes (2026-10-05)

XInput in a Wine process reads the pad from that process's sink in `ntdll`
(patch 0470), and only the title's sink is ever set: the title polls the
DualSense every tick and hands the sink the state. A child's sink stayed
empty, so a game in a child had no controller. Now the title's sink also
keeps its pad in a 16 KiB shared page, made on first use; the spawn path
sends the page to every child it starts, in a `PW_CHILD_INPUT_PAGE` message
after the bootstrap (protocol `0x50573505`, so a loader and runtime that do
not agree refuse each other); the loader passes it on as
`WINE_PS5_INPUT_FD`; and a sink that the title never set reads the pad from
the page, under a sequence number that is odd while the page is written.
The headless owner used to block waiting for its children, so the tick loop
that feeds the pad never ran there: it now runs the loop while the children
do and leaves for the launcher once the initial client has ended,
`back_to_launcher` waiting for every child and stopping the broker.

`graphics-child-input-dxvk-01` showed both halves of the problem: all six
children received the page, but every `XInputGetState` returned 1167 (not
connected), as the owner never wrote it. With the loop running,
`graphics-child-input-dxvk-03` passed: a pad script on the owner held
Triangle and set the left stick to half right, and each of the four rendering
children, two of them descendants, read result 0, buttons `0x8000` (Y) and
left stick 16,383 through XInput, while every rendering check passed as
before. Host tests cover the owner's page. Not yet forwarded: the touchpad's
mouse and keyboard events, the DualSense's raw HID reports, and audio. (Later on
2026-10-05 patch 0931 had the title inject the touchpad's pointer, clicks and keys into the
desktop itself, through the server, to whichever process owns the window:
`desktop-01`, [WINDOWS_PROCESSES.md](WINDOWS_PROCESSES.md). Since
2026-10-06 the page also carries the DualSense's USB report, and a helper's
`pw_wine_dualsense` reads it there. That is the report winebus's PS5 bus gives a
game that finds the pad through raw HID, as a .NET (MonoGame) game's SDL does with
`SDL_XINPUT_ENABLED=0` (2026-10-06 (record removed from the tree; in git history)). On the console,
scripted presses reached that game this way. winebus created the pad
(054c:0ce6) in the game's helper, the game read its reports through hid.dll,
and the menu selection followed the script (2026-10-06 (record removed from the tree; in git history)). A
person's presses on the pad have not yet reached it ([WINDOWS_PROCESSES.md](WINDOWS_PROCESSES.md)).
Audio: below.) Rumble
is not forwarded either, but the title does not apply a game's rumble
(`pw_wine_rumble`) in any mode yet.

### Audio for helpers: behind a switch, heard on the console (2026-10-06)

Why a helper is silent. Wine's audio driver (`wine/wineps5`) mixes every
stream into grains of 256 stereo 16-bit frames at 48 kHz and hands each grain
to `pw_wine_audio_output`, which blocks while the title's one AudioOut port
plays it. `pw_wine_audio_available` is 1 only where the title set that sink,
in the title's own process. In a helper it is 0, so the driver reports itself
unavailable and mmdevapi finds no device. The game's helper logged "No
driver from L"ps5" could be initialized" (2026-10-06 (record removed from the tree; in git history)).

What it would take, modelled on the input page:

1. **A ring per helper.** The spawn path creates a small shared memory
   object for each child, with a header (magic, the helper's write count, the
   title's read count, drops) and 8 grains (8 KiB, 43 ms), in one 16 KiB
   page, as the input page is. It passes the
   object in the bootstrap, and the loader exports it as `WINE_PS5_AUDIO_FD`,
   as it does `WINE_PS5_INPUT_FD`. The rings are per helper, because several
   may play at once.
2. **Helper side, in `pw_wine_sink`.** `pw_wine_audio_available` is 1 when
   the child has a ring. `pw_wine_audio_output` copies the grain into the next
   slot once the ring has room, and waits in short sleeps while it is full.
   After about 100 ms with no room it gives up and counts a drop, so a stalled
   title never holds a game's audio thread.
3. **Title side.** The thread that owns the AudioOut port takes one grain per
   output from each live ring, mixes them (`src/pw_audio_mix.h` already mixes
   streams), or takes only the foreground helper's. It plays silence when no
   ring has a grain, so the port stays the clock. A ring goes when its child
   is closed (`monitor_child`). `pw_wine_stop_audio` drains the rings before
   the title replaces itself.
4. **Proof.** Host tests for the ring: wrap, full, empty, drop, a reader
   that stalls. On the console: a helper that plays a tone, with the title's
   log counting each helper's grains played and dropped, and the stream's
   `IAudioClock` advancing at 48,000 frames per second. Then a game with
   sound, checked by ear.

Built as above, with `WINE_PS5_HELPER_AUDIO=1` (the title's environment: a
game.json or a test) until a console run proves it:

* `src/pw_audio_ring.{h,c}`, the ring.
* `pw_wine_audio_ring_create` and `pw_wine_audio_ring_release` in
  `wine/ps5/pw_wine_sink.c`, with the title's mixer thread. A helper's
  `pw_wine_audio_available` and `pw_wine_audio_output` use the ring.
* The spawn path (`spawn_native`) makes one ring per helper and sends it after
  the input page, as `PW_CHILD_AUDIO_RING`, under the `PW_CHILD_AUDIO` flag.
  `monitor_child` releases it.
* The loader (`native/proton_wine_child.c`) exports it as
  `WINE_PS5_AUDIO_FD`.
* The bootstrap protocol is `0x50573507`, so a loader and a runtime from
  before the change refuse each other: the title folder and the runtime are
  deployed together.

Host tests:

* `test_pw_audio_ring`: as above, and 20,000 grains between two threads.
* `test_pw_wine_sink`: a helper, as a separate process given a ring, writes
  three grains, and the title's mixer plays them through its sink in order.
  Two rings at once are summed, 1000 + 2000 to 3000, and clamped, 32000 + 32000
  to 32767.

On the console. The first two game runs with the switch
(2026-10-06 (record removed from the tree; in git history)) ended after 17 and 19 s, and not
in the audio code. In each, klog shows two `pager error`s in the game's helper, the
kernel failing to authenticate a page of a fake-signed module it was reading back
in. One was win32u's text. The other, at `+0x3c550` in ntdll, is the code of
the signal handler. So the helper died outright instead of reporting
the fault. The title then died of SIGPIPE, writing to the dead helper's socket.
Two fixes followed (details in [WINDOWS_PROCESSES.md](WINDOWS_PROCESSES.md)).
The payload loader wires every segment of each module it loads (`mlock`), and the
owner ignores SIGPIPE.

The third run (2026-10-06 (record removed from the tree; in git history)) passed. The person at the console heard the music. The title logged
`non-silent stereo audio submitted at 48000 Hz`. The game ran its 190 s,
closed on request and returned to the launcher. The game's audio driver, which reports every
10 s, played 1,875 grains each time (10 s at 48 kHz, so in real time), with
no refused grain and no short grain after the first 10 s. A grain took 5.30 ms
on average to hand over (it plays for 5.33 ms) and the longest gap between two
was 1.3 to 2.4 ms. The switch stays off by default until more games have
played with it.

## When the broker cannot vouch for the GPU (2026-10-05)

The broker used to stop dead (`retain`, an endless loop, often holding its
lock) whenever it could not show the GPU had finished with something: a
failed submission, a fence that did not move for 5 s, a helper still listed
after its close. The native probes keep that, so a run shows it. The Wine
broker now marks the GPU **wedged** instead: it logs the reason once, frees
nothing from then on (staging, deferred buffers, the fence page and the
reserved ranges stay mapped until the process is replaced), answers
allocations, submissions and presents with EIO so the game sees its device
lost, and still serves closes and its own stop. `PROTON_GPU_TEST_FAIL_SUBMIT=n`
refuses the submission after `n`, before it reaches the GPU, to test this.

`graphics-child-broker-failure-01` (failure after 20 submissions): the broker
wedged with `test_submit_failure` and did not block; three of the children
ended. The fourth renderer did not: after DXVK's "Failed to begin command
buffer" it stayed alive, and the owner, which by design does not replace a
process while children run, waited for it. The title was killed with
`tools/kill-hung-title.py --force` (2 processes, console idle afterwards),
production restored and checked with the new `tools/restore-process-probe.py`
(eboot and the four runtime modules read back, every probe file removed), and
the restored launcher then passed 120 frames, its picture and log checks,
and closed without force (`graphics-child-broker-restored-production-06`).
`graphics-child-wedge-policy-regression-01` passed the full four-renderer and
input test with this broker. So a broker failure no longer freezes the
title; a child that does not exit after losing its device still holds it.
What remains is for the owner to close its children, through Sony's service,
when a stop is requested while they run.

Observed and not explained: each helper may run on CPUs 0–12 (`0x1fff`, the
title's set), but 200 samples of a sleeping thread saw only even CPUs
(`0x155`). The 1 ms sleep in a helper measured 1.1 ms.

The final helper/runtime also passed the headless regression
`graphics-child-gpu-broker-wine-descendants-04`: three descendant-node
verdicts, five successful 4 MiB bootstrap stacks, five native closures,
zero broker work/faults and verified production restoration. The restored
production launcher then passed 120 frames and its picture/log checks,
closed without force and left the shell idle with zero fault/driver-error
records (`graphics-child-gpu-broker-restored-production-05`).


## Foundation extraction, 2026-10-06

At the user's request, the reusable graphics implementation now belongs to
PS5_Vulkan and PS5_Mesa. The foundation exposes native owner/client APIs without
Wine headers or a process launcher. Proton supplies lifecycle and focus/cursor
callbacks and retains its packet bootstrap and PRX/DLL packaging. The transport,
shared frame format, submission/fence rules and display implementation are no
longer copied implementations in Proton. Native synthetic probes stay here.

The graphics foundation is pinned to PS5_Vulkan
`ae2c38c68e5f65f912b8ee475ed80a165a66f51d`. Four ordered RADV broker overlays
and the generic Zink present-mode fix are owned by PS5_Mesa
`f7ce91ac4c3697990bf89d79fdc61ae7cb98570b`. The ordinary title RADV release
archive remains pinned at `7b59ef27c1b09b9671bc4153c41940c3155c3af2` and its
SHA-256 remains `0ffa39311881236b4872ec77a56bca417ce56c71cefca5954224b9ea8f3a1bf5`.
The Mesa overlays affect isolated broker copies only. The optional Windows
build exports the pinned Mesa fork, with no implicit fetch. Proton consumes
pinned git exports; mutable sibling sources are excluded from these inputs.
Later on 2026-10-06 the foundation moved to PS5_Vulkan `5b5e4fc`, whose
Windows build takes PS5_Mesa `b3588f78dd7` (Zink's descriptor buffers in
host-visible memory, [WINDOWS_PROCESSES.md](WINDOWS_PROCESSES.md)); the
broker overlays stay at `f7ce91ac4c36`. Then to `2d414b1`, whose broker also counts the frames it shows and
takes a picture of the next one on request (`ps5_gpu_broker_flips`, `ps5_gpu_broker_capture_next`), for pad
scripts in the launcher layout. Then to `4d7e053`, whose broker takes the memory it shares with a client from the application when given a
maker (`ps5_gpu_broker_set_shared_memory`): in PS5_Proton the title's memory helper, so that no buffer a helper maps
belongs to the title ([WINDOWS_PROCESSES.md](WINDOWS_PROCESSES.md), "Closing the title from the system menu").

Validation of the extraction is recorded in
[`evidence/graphics-foundation-extraction-01.json`](../../../../evidence/graphics-foundation-extraction-01.json).
Standalone owner/display compile, the WSI client/archive/module, PE64 WGL DLLs,
production/payload builds and focused ASan/UBSan transport checks are build/host
qualification. Previous console evidence remains four DXVK renderers, 96
submissions, 27 flips and 6/6 PIDs absent; it is not a console run of the
extracted binaries. Visible WGL presentation, native EGL/OpenGL, game-scale
concurrency and current game acceptance still need console qualification.
The drop guard remains set pending an actual console restart.
