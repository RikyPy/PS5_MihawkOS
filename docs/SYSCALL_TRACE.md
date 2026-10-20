# Direct PE64 syscall interception

**Archived 2026-10-18.** The code this note describes is not on main: it is on the local branch
`archive/direct-syscall-tracer-2026-10-10` (`1063ef0`). The record is in [PLAN.md](PLAN.md), "Direct PE64 syscall
tracer". Paths and switches below name files on that branch.

Status: opt-in candidate; owned native interception and the Linux Wine oracle
pass. Wine integration on Pro and application compatibility remain unqualified.
See PLAN.md and the named evidence, not this mechanism alone, for acceptance.

## Ownership and route

`WINE_PS5_DIRECT_SYSCALLS=1` starts one dedicated payload tracer for each opted-in
Wine payload. A bound nonce Unix packet socket authorizes that helper and transfers
a shared native-code range page. There is no command-line arbitrary-PID attach.
The listener exists before websrv/elfldr starts the tracer. The sender holds its
page descriptor until acknowledgement, following the existing transfer contract.

The helper publishes its ELF and loaded PRX executable ranges before its Wine
worker starts. The serialized module-preparation callback appends new PRX ranges
before using them. The mapping and connection live until the helper exits;
closing them while Wine threads still run would race native calls. This assumes
native PRXs remain resident for the payload lifetime. If unloading becomes part
of that contract, the whitelist needs removal/generation handling first.

At a PE64 entry outside these native ranges, the tracer saves the stopped
thread's registers and substitutes native getpid before the kernel performs the
original operation. An owned `int3` return gate gives the tracer control again.
It checks the exact gate IP and pending thread, restores the saved registers and
delivers SIGSYS. Wine patch 1004 reuses its existing SIGSYS handler and dispatcher:
Wine owns service numbers, Windows arguments, the TEB, output buffers and errors.
The syscall frame's +0xb compensates Wine's existing dispatcher -0xb; it does not
assume the application's stub layout. No application bytes are patched.

Native calls continue unchanged. Real application signals are forwarded;
Pro syscall-entry stops can retain stale PL_FLAG_SI from attachment, so
SIGTRAP+SCE is treated as syscall entry. Exact owned gate recognition comes first;
real breakpoint traps elsewhere are forwarded.
IA-32 direct calls are a separate ABI and are not intercepted by this PE64 mode.
Descendants inherit the opt-in and establish their own tracer.

## Scoped debugger adapter

`native/pw_ptrace_ps5.h` captures the native carry flag and normalizes errno because both the
SDK ptrace and generic syscall shims return positive errno on carry. The adapter
also declares the Pro's R8/R9 clobbers and mutable fourth argument register;
omitting these caused the compiler to preserve a pointer in a register the
kernel cleared. The dedicated tracer session uses the published [shsrv debugger profile](https://github.com/ps5-payload-dev/shsrv/blob/master/pt.c)
on the dedicated tracer's own credentials, then restores and verifies both the
original authentication ID and capability bytes at teardown. It does not alter the target,
title, loader or kernel instructions. The tracer is single-threaded and has no
credential-changing signal handlers. SDK inputs remain pinned by the existing
payload build manifest.

Pending calls are bounded to 64 simultaneous threads with individual ten-second
return-gate deadlines. A disconnected helper gets a bounded teardown interval.
On failure the tracer kills only its nonce-authorized helper; it never detaches
an intercepted Windows call into native execution. Logs are local ignored files
`/data/ps5proton/logs/syscall-trace-<native-pid>.log`, including phase, errno,
entry/intercept/delivery counters. Functional API success is the owned PE probe's
verdict, not the tracer's observation of native process exit.

## Commands and evidence

- `bash tools/build-syscall-trace-probe.sh`
- `python3 tools/run-syscall-trace-probe.py --name <fresh-name> --mode redirect`
  (`--host-only` runs just the Linux protocol/oracle).
- `bash tools/host-tests.sh syscall_trace`: sanitized native-range classifier.
- `bash tools/build-direct-syscall-probe.sh`: owned PE64 with service IDs generated
  by the pinned Wine source, imported API comparisons, concurrent threads and
  one descendant. The host oracle uses an isolated Wine prefix.
- `bash tools/build-production.sh`, normal deployment, then
  `python3 tools/run-console.py --exe direct-syscall64-probe.exe --marker
  PS5_PROTON_DIRECT_SYSCALL --env WINE_PS5_DIRECT_SYSCALLS=1 --watchdog 240 --name
  <fresh-name>` (one shell command).
- The permanent `direct-syscall64` case belongs to the full exact-source gate.

`syscall-trace-pro-gate-2026-10-10-03.json` proves original native side-effect
prevention and same-thread owned dispatch, including clean process absence.
`direct-syscall-host-2026-10-10-02.json` proves the owned Windows oracle: 338
checks, two processes, four worker threads in each, 160 direct calls per process.
Earlier failed experiments remain recorded; syscall-exit-stop support was not
qualified, so this route uses an owned return gate.

This is not Linux syscall-user dispatch: every native entry also incurs a
ptrace stop. Application overhead must be measured. Matching all Windows service
number versions, IA-32 direct calls, graphics workload performance and the
original game's startup are not proven by the owned native probe.

## Current qualification boundary

Run `direct-syscall-pro-2026-10-10-05.json` reached and neutralized the owned
PE64 query, but did not deliver it into Wine. Root stop-info failed; service
shutdown also exposed the kernel's native signal-return stub outside the PRX
list. Three terminated helpers remained in websrv's process table, so a reboot
is required by the existing cleanup census. No outage was recorded. The user subsequently confirmed a fresh reboot and restored services; focused
native qualification resumed.

The read-only `KERN_PROC_SIGTRAMP` query returns four zero bytes on this firmware,
not FreeBSD's address record. Instead, before Wine starts, an owned native signal
handler observes the kernel-provided return address; sceKernelVirtualQuery must
identify its executable, read-only mapping. The prior handler and mask are
restored on every path. That exact mapping is published as native. No kernel
code scanning or address band guesses.

Three post-reboot focused probes pass: `syscall-trace-pro-entry-gate`,
`syscall-trace-pro-signal-gate` and `syscall-trace-pro-thread-gate`, each with the
suffix `2026-10-10-01.json`. Together they qualify PT_TO_SCE continuation through
the owned gate, TID register access with PID continuation, and exact native
trampoline discovery. All finish with no surviving helpers or tracers. These
probes do not qualify Wine SIGSYS delivery or application startup.

Native PRXs use read-only `sys_dynlib_get_list` (592); Pro run 05 enumerated
kernel, libc, sysmodule, net and ntdll as well as eboot. Initial and subsequently
prepared PRX executable ranges share one table. Trace logs record stop metadata
and progress every ten seconds. A native process exit with an undelivered call
is an error, even if the process exit itself was observed.

Run `direct-syscall-pro-2026-10-10-06.json` timed out after 241 seconds before
any owned PE verdict. The ten traced clients total 1,142,004 native entries;
no Windows calls were intercepted. Each client remained as a terminated entry
under websrv. Tracer process-exit observation therefore does not prove loader
reaping. The cleanup census correctly blocks another job pending reboot.

The follow-up saves/restores the dedicated tracer's credentials once per
session, rather than repeating the kernel credential walks on every request.
It detaches at classified native SYS_exit only with no undelivered Windows
call; the original loader parent then owns reaping. On error it releases
tracing only with SIGKILL. The native qualification below proves normal exit
cleanup; pending-call failure cleanup and full Wine delivery still need evidence.

`python3 tools/run-tracer-lifecycle.py --name <fresh>` builds an owned probe
using the actual production tracer, compares 4096 native getpid results and
before/after timing, and requires the native-exit detach verdict and an empty
helper/tracer census. No title or Wine starts. `--build-only` builds without
console access. Both focused probes now pass after a confirmed fresh boot.

`syscall-trace-pro-scoped-gate-2026-10-10-01.json` preserves the failed
return-gate follow-up: its tracer crashes in event() after PT_LWPINFO because
R9, holding an output pointer, returns zero from the inline syscall. No helper
survives and the console remains responsive. The shared adapter fix declares
R8/R9 clobbers and the fourth argument mutable. Run 02 of that probe passes
same-thread dispatch, prevented pipe write, EFAULT normalization and credential
restoration; the pair finishes in 0.431 seconds with an empty census.

`tracer-lifecycle-pro-2026-10-10-02.json` uses that same corrected adapter in
the actual production tracer. All 4096 native calls return correctly; 4281
entries lead to native-exit detach, and neither payload remains. The run takes
1.256 seconds. Its call-loop baseline is 3.652 ms versus 697.192 ms traced,
about 170 microseconds per traced call (191 times this minimal baseline).
This is one native sample, not a Wine startup benchmark or game speed claim.
The previous adapter's lifecycle run 01 also passed, but does not qualify the
corrected source. The Linux return-gate oracle passes with run 02. Next is the
bounded owned Windows API probe; Wine SIGSYS dispatch remains unproven.

The bounded follow-up `direct-syscall-pro-2026-10-10-07.json` fails after
177.7 seconds, returning through the owner with status 1. It reaches the owned
PE query and neutralizes service 54, but Wine receives EXCEPTION_BREAKPOINT
at the exact owned return gate (symbolized against the preserved candidate ELF).
The tracer never sees its expected gate stop, records foreign=1/delivered=0 and
ends at the pending deadline (ETIMEDOUT). This identifies a signal-handoff
qualification gap: the single-thread native test without Wine's installed
handlers does not prove the route in a Wine process. No API dispatch passes.

Across ten clients, 3,404,186 native entries are observed. Eight detach at native
exit; one instead observes process exit; the root fails. One forced service
helper remains terminated under websrv after the owner closes. The console
answers normally and is idle, but the cleanup census blocks another launch.
Normal native-exit cleanup does not establish externally terminated cleanup.

Pending-stop logging is now capped at 32 records locally; the failed gate
otherwise generated 13019 trace lines. That logging change is not console
qualified. Next isolate installed native SIGTRAP handlers/concurrent threads
and forced termination in small owned probes before spending another full
Wine startup. Preserve real Wine signal ownership and Windows dispatch;
no application instruction patches, service-ID substitutions or watchdog waiver.

The small signal probes are built. Linux oracle runs
`syscall-trace-host-handled-gate-2026-10-10-01.json` and
`syscall-trace-host-threaded-gate-2026-10-10-01.json` pass with zero handler
hits; the second adds two concurrent workers. An unexpected native SIGTRAP
handler hit recovers only the probe's owned gate into its owned dispatcher and
reports failure. This avoids needing a deliberate crash to diagnose premature
signal delivery. It is not a Wine handler replacement or a passing fallback.

After confirmed reboot and readiness, run separately with fresh names:

```sh
python3 tools/run-syscall-trace-probe.py --name <fresh-handled> --mode redirect-handled
python3 tools/run-syscall-trace-probe.py --name <fresh-threaded> --mode redirect-threaded
python3 tools/run-tracer-lifecycle.py --name <fresh-forced> --case forced
```

The forced case builds the actual production tracer, verifies 4096 native calls,
then sends SIGKILL only to itself. Acceptance requires the signal-exit trace,
successful tracer termination and empty helper/tracer census. Preserved partial
HTTP output is diagnostic evidence, never sufficient for a pass. Self-termination
isolates reaping after a signal; a racing external owner termination still needs
qualification. Both native probe builds pass, but these three console cases
remain unrun. Do not launch the next case over a failed cleanup guard. No code
commit or Wine dispatch claim until the required console evidence exists.
