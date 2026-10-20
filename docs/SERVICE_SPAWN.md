# Sony-managed spawning reference

Reviewed 2026-10-07. This is a reference to the **earlier Sony local-process route**.
Production now starts Windows helpers through websrv/elfldr payloads. The full
static ABI investigation and console attempts are retained in
[the service-spawn history](history/2026-10-07/docs/SERVICE_SPAWN.md).

## What the investigation established

Sony's service manages spawn, native application association and budget/lifetime
handling. The firmware 10.01 API differs from public PS4 declarations, so an
apparently familiar function signature is not a safe ABI substitute.
The options field at `0x8` controls crash reporting; the service supplies the
source PID from the IPC caller. A title import/export check alone does not prove
that loading every system library is permitted.

`service-native-07`, `-08` and `-09` transferred a packet connection to descriptor
3, completed identity/native-PID handshakes and service teardown with status 0.
Children disappeared, parent descriptors stayed 12/12, and no fatal signal was
recorded. Missing-image rejection also kept list/descriptor counts unchanged.
Headless PE64 children and descendants subsequently passed through Wine's shared
server. Direct helper GPU initialization still failed in that arrangement.

An explicit headless preload policy was required in the tested setup. The mask
and helper process parameter must match the qualified image; copying raw rfork
flags or private syscall names does not reproduce service initialization.

## Current production distinction

The Sony route's seven-helper ceiling motivated the payload route. Current
packaging ships the payload loader and removes the old `wine-child.bin` image.
Production's process adapter calls `payload_spawn` directly; the Sony-spawn
adapter and its environment opt-out were removed. Standalone Sony probes and
historical builders remain as investigation tools, not a production fallback.
Payloads use a control connection for lifetime and explicit native-PID checks
for cleanup. Application-pool memory uses `sceKernelAllocateDirectMemoryForApp`,
not Sony service budget reassignment.

For current launch behavior use [WINDOWS_PROCESSES.md](WINDOWS_PROCESSES.md).
For graphics use [GRAPHICS_PROCESS_BROKER.md](GRAPHICS_PROCESS_BROKER.md).
The known-panic bare-title spawn remains disabled, as explained in
[CHILD_PROCESSES.md](CHILD_PROCESSES.md).
