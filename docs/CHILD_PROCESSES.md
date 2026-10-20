# Native process investigation

Current disposition, reviewed 2026-10-07: production uses **payload processes
started by resident websrv/elfldr**, integrated with Wine's process bootstrap.
Child creation is no longer a blanket blocker. See
[WINDOWS_PROCESSES.md](WINDOWS_PROCESSES.md) for supported behavior and limits.

## Routes investigated

| Route | Finding |
| --- | --- |
| Bare elevated-title `rfork_thread` | Import/lookup resolved, but the direct spawn caused a user-confirmed kernel panic; no backtrace captured |
| Sony local-process service | Native handshake/socket transfer/teardown passed; later headless Windows children/descendants passed |
| Original title as one graphics client | Focused rendering passed, but one client per fresh title was the architectural limit |
| GPU broker with separate helpers | Multiple rendering Windows clients/descendants passed |
| Payload spawning | Current default; eight Windows children and cleanup passed, without Sony's previous seven-helper ceiling |
| Payload application allocator | Shared application-pool backing passed without a kernel budget patch |

The bare title probe remains excluded from ordinary builds. A reboot does not
authorize repeating that known-panic experiment. Availability of an export or
successful elevation is not proof that a native spawn is safe.

## Current boundaries

The owner title retains the GPU/display and shared Windows session. Windows
parenthood differs from native payload lifetime: descendants may outlive their
immediate Windows parent, but control disconnect ends a payload. Native cleanup
and GPU completion must be checked separately from Windows exit codes.

The original firmware/export findings, interrupted trace and recovery validation
are retained in [the native investigation history](history/2026-10-07/docs/CHILD_PROCESSES.md).
Sony ABI details are in [SERVICE_SPAWN.md](SERVICE_SPAWN.md); the current
architecture and acceptance work are in [ARCHITECTURE.md](ARCHITECTURE.md) and
[PLAN.md](PLAN.md). Locally supplied firmware remains a read-only reference and
is never vendored, logged as bytes or distributed.
