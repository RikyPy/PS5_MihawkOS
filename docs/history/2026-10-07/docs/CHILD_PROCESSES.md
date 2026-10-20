> Historical snapshot of `docs/CHILD_PROCESSES.md`, archived on 2026-10-07 from `9b07ebb`.
> Defaults, next steps, counts and failure classifications below belong to their original runs.
> Use the [current document](../../../CHILD_PROCESSES.md) for present guidance; see the [archive index](../README.md).

# Native child processes: investigation and recovery

## What is established (2026-10-04)

Static inspection of the locally supplied firmware 10.01 libraries found a real
`rfork_thread` implementation in ordinary `libkernel`, not only in
`libkernel_sys`. The implementation calls syscall 251, selects a separate child
stack, calls the supplied callback and terminates the native child when that
callback returns. This is export/implementation evidence, not proof that a title
can safely spawn children.

Console evidence narrows the boundary:

- `evidence/child-native-02.json`: lookup using module handle 1 failed. The probe
  stopped without spawning, closed through the shell and its production backup
  was restored. This lookup failure does not establish a missing title import.
- `evidence/child-native-04.json`: the recovered, identity-bound durable trace
  records successful elevation (`uid=euid=0`), a nonzero title import of
  `rfork_thread`, and a successful lookup at handle `0x2001` matching that import.
  Stack and private-memory mappings succeeded, as did `socketpair`.
- The last durable line records `spawn flags=0x14 sigsys_ignore=0`, immediately
  before `rfork_thread(RFPROC | RFFDG, ...)`. No return, child PID, acknowledgment
  or wait status was recorded. The user confirmed a kernel panic. There is no
  captured panic backtrace, so the exact kernel failure is unresolved.
- The final `BUDGET_FD_FILE` warnings also appeared in the earlier cleanly exiting
  diagnostic run. They cannot identify the panic's cause by themselves.

The title's system-version API reports `12090001`; the resident daemon's firmware
record reports `10010000`. Preserve both observations rather than treating the
title-facing version value as the firmware identity for the static inspection.

Sony-managed native helpers now pass: `service-native-07`, `-08` and `-09` each
created a fresh process, transferred a SOCK_SEQPACKET connection to descriptor
3, completed the identity/PID protocol, and closed through Sony's service.
The helper disappeared, parent descriptors stayed 12/12, and the parent closed
normally with zero fatal signals. The synthetic missing-image rejection also
passed with unchanged list/descriptor counts. See [SERVICE_SPAWN.md](SERVICE_SPAWN.md)
for the preload policy, failed startup attempts and exact limits of this proof.

The next native lifecycle checks and the experimental Windows integration now
pass: four-cycle native allocation reuse, application closure with a live native
helper, and headless PE64 children connected to the original wineserver. Shared
handles, process events, full exit codes, suspend/resume, termination and Windows
parent exit are covered in [WINDOWS_PROCESSES.md](WINDOWS_PROCESSES.md).
Descendants now also pass through the owning-title broker: three generations,
four repeated chains and a grandchild spawning after its immediate Windows
parent exits. Child graphics remain open; production defaults stay off.

## Recovery and containment

After the user restarted the console, `tools/recover-child-probe.py` retrieved
the surviving result before clearing startup hooks. It restored the saved signed
production SELF and verified its headers and executable/import segments against
the matching local ELF. FTP's reconstructed ELF has unstable unmapped trailing
metadata, which is excluded from this executable comparison; full-file hashes
are recorded separately. The interrupted run's record retains the panic outcome.

`evidence/child-recovery-launcher-01.json` proves the restored launcher rendered
120 frames and closed normally, without fault or driver-error records. That run
was sandbox-only after the reboot. The previously validated title-scoped
elevation service was then restarted. `evidence/child-recovery-library-01.json`
records filesystem access, all eight library entries, 120 frames in 2.45 seconds
(59.9 FPS after the first half), normal shell exit and zero fault/driver-error
records. No game was launched.

The probe is excluded from ordinary builds (`PS5_CHILD_PROBE=OFF`). The dedicated
builder enables it only in an isolated snapshot, using the pinned SDK/UI inputs
and the existing RADV recipe. The runner refuses another direct probe after a
recorded panic. Rebooting resets the ordinary launch guard, not that refusal.
Native child callback return is distinct from title termination: the parent
always returns through the CRT's shell-close path.

## Next boundary to investigate

The follow-up static investigation is recorded in
[SERVICE_SPAWN.md](SERVICE_SPAWN.md): the PS5 local-process API has a different
ABI from the public PS4 declaration, carries a source PID/descriptor pair, and
routes through Sony's service-owned spawn/budget machinery. Its pinned-input
verifier now passes 55 static checks, including export library namespaces and startup call edges. Two service-managed helpers completed their protocol and service teardown.
The next console step passed: `evidence/service-preflight-01.json` confirms four
title imports, a valid app association, rejected null/oversized requests, stable
12-descriptor counts and normal shell exit. It sent no valid spawn request. A
deeper service-handler trace also corrects the public options field at `0x8`:
it is a crash-report setting; Sony supplies the source PID from the IPC caller.

Do not repeat the bare title spawn. The service now proves native creation and
socket transfer; complete the remaining native spawn contract:
budget creation/assignment, descriptor accounting, application association,
permitted executable images and teardown. Static inspection shows Sony's spawn
paths manage budgets and use `rfork_thread` with `0x1030` before fresh `execve`.
Those flags alone are not a safe substitute; the service's setup and ABI matter.

The service-managed path and descendant broker are now integrated experimentally.
Wine's existing `WINESERVERSOCKET` path connects each separate PE64 child to the
original server. Descendants transfer new server and reply sockets to the owning
title; it performs every Sony launch and native teardown. Wine preserves the
immediate Windows parent tree. Keep this proved headless lifecycle separate
from future child graphics, whose module startup and display ownership still
need console evidence.
