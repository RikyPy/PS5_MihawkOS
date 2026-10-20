> Historical snapshot of `docs/WINDOWS_PROCESSES.md`, archived on 2026-10-07 from `9b07ebb`.
> Defaults, next steps, counts and failure classifications below belong to their original runs.
> Use the [current document](../../../WINDOWS_PROCESSES.md) for present guidance; see the [archive index](../README.md).
> Run records removed from the tree on 2026-10-18 (in git history) are named below by date and the part of their name
> that named no game or launcher.

# Experimental Windows child processes

Sony-managed spawning is integrated into Wine behind `WINE_PS5_CHILDREN=1`.
The focused headless PE64 lifecycle and an additional single graphics-client
arrangement have run on the console. General Proton process conformance and
GPU setup inside a Sony-managed helper remain unproved. Production defaults stay
unchanged, and the console runner restores the original signed runtime after
its experiments. No new spawning payload is used. The existing elevated-title,
kstuff and owned-root Lapy service setup is still required by these runs.

## Bootstrap and ownership

Patch `0925-ntdll-ps5-service-managed-children.patch` preserves Wine's
`NtCreateUserProcess` image validation, `new_process`/`new_thread` registration,
startup parameters, environment and inherited handle table. It replaces the
native `spawn_process` implementation with `pw_ps5_spawn`.

The parent calls ordinary `sceSystemServiceAddLocalProcess` with the proved
72-byte options layout and headless preload mask `0x8000000000000002`. Sony
transfers a SOCK_SEQPACKET control socket to the helper's descriptor 3. A
versioned bootstrap message transfers Wine's original SOCK_STREAM server
socket through SCM_RIGHTS, plus the runtime, prefix and resolved Unix working
directory. The child requests the established title-scoped elevation before
starting Wine workers, loads `ntdll.prx`, sets `WINESERVERSOCKET` to the received
socket and starts Wine. Wine retrieves the actual Windows image, command line,
environment and handles from the parent's existing server. The child does not
load a second wineserver. The helper's dummy native arguments are not Windows
startup parameters.

The server runs in the original native title. Children use the protocol socket;
they cannot call the server's same-address-space fast path. The process-local
sync device is disabled for this opt-in mode. Native signal requests cross the
control socket, and the child translates them to `pthread_kill` using its own
thread registry. Native scheduler changes are limited to the server's host;
Windows priority state remains server-owned. Remote memory copying is denied
rather than dereferencing a child's address in the server process.

Windows process identity, handle signaling and full exit codes remain Wine
server state. A native completion message triggers Sony service closure; it is
not a native `waitpid` status. Forced cleanup also uses the owned Sony service
ID, never that ID as a native PID. Child slots remain reserved through service
closure, including a race between termination and the completion monitor.
Failure after the helper's ready acknowledgment reports completion so the
parent can close the helper. That allocation/thread-start failure branch is
built but has not been fault-injected on the console.

## Two different parent lifetimes

A Windows parent can exit while its child continues. The parent closes its Wine
client socket, making the Windows parent object signal, and keeps the native
server host alive until the owned children finish. The probe child opens a
synchronization handle to its parent before reporting readiness, waits for that
handle to signal, prints its survival verdict and exits with `0x1234`.

Closing the entire PS5 application has different behavior: Sony closes the
associated native children. `service-native-parent-close-01` left a native
helper holding 1 MiB live, returned through the parent's shell-close path
without `KillLocalProcess`, and captured the helper's NOTE_EXIT and deletion.
The early `service_close id=...` diagnostic in that historical trace was printed
before the parent-close branch; it did not indicate a service-close call. The
log placement is corrected in the source.

## Descendants through the owning title

Patch `0926-ntdll-ps5-broker-descendant-spawning.patch` adds a second opt-in,
`WINE_PS5_DESCENDANTS=1`, in the Windows process environment. The original
`WINE_PS5_CHILDREN=1` session gate still applies. The focused probe sets the
Windows variable after Wine bootstrap and uses `DETACHED_PROCESS`, keeping
automatic console-host and bootstrap spawning outside this experiment.

A headless child sends its newly registered Wine server socket and a private
reply socket through SCM_RIGHTS on its existing control connection. The
original native title starts the descendant through the same Sony API and
returns the launch status on the private reply socket. This separates launch
replies from signals read by the child's control agent. A pending-request
counter keeps the original server host alive during accepted launches;
failure to deliver a successful reply closes the newly owned native process.
These cancellation and launch-timeout paths are built but not fault-injected
on the console. The checked transport closes received descriptors on truncated
messages or ancillary data; real socketpair tests cover both failures.

Sony's native ownership tree is flat under the title; Wine's Windows parent
tree remains intact. No descendant loads an independent server or calls Sony's
launch client directly. The control protocol is version `0x50573502`; matching
experimental helpers and runtime modules must be deployed together.

Three console cases passed:

| Case | Evidence | Result |
| --- | --- | --- |
| Three descendant generations | `wine-descendants-chain-01` | Three distinct Windows PIDs, each queried immediate parent correct, quoted arguments/environment and inherited pipe/event shared across generations; full `0x5100`–`0x5102` exit codes |
| Immediate parent exits before further spawning | `wine-descendants-parent-exit-01` | Grandchild observed its parent's process handle signal, then created the great-grandchild successfully |
| Four repeated chains | `wine-descendants-repeat-01` | 12 descendant node verdicts, eight broker launches, 13 native children including wineboot deleted; Windows handles 12/12 then 14/14 for all three later cycles |

The repeat trace measured native descriptors at 100 before every first-generation
spawn, 109 before each grandchild and 118 before each great-grandchild. All
children closed at 73 descriptors, matching the single-chain and orphan runs;
intermediate closure samples overlap startup and are not steady-state counts.
Each run captured zero native fatal/GPU-fault records, returned through normal
title paths, restored both signed production modules and removed its helper,
probe and child log files. The repeat and orphan cases use the final adapter's
error-preservation fix; the initial chain predates that fix. Host Wine passed
all eight lifecycle/descendant cases in `process-host-descendants-regression-02`.
The `-01` host attempt failed before any PE ran because the sandbox denied
Wine's Unix socket; its failure record is retained.

The final broker build also passed all six earlier console cases:
`wine-broker-basic-01`, `-suspended-01`, `-terminate-01`, `-repeat-01`,
`-parent-exit-01` and `-disabled-01`. The disabled case created no native children;
the enabled cases captured every owned native child's deletion. All six had
zero native fatal/GPU-fault records and verified production restoration.
`wine-broker-restored-launcher-01` then rendered eight library entries and 120
frames at 59.9 FPS with zero native fault/driver-error records and a normal
shell exit. `wine-descendants-milestone-01.json` collects the nine console
cases, eight host cases, final source hashes and remaining unproved boundaries.

## Native resource proof

`service-native-reuse-01` passed four native cycles. Each helper held 1,048,576
bytes at physical offset 18,874,368 until service closure. The available direct
memory span was 12,866,027,520 before, 12,864,978,944 during, and 12,866,027,520
after every cycle. Sony's process-list count returned from two to one and
parent descriptors balanced at 12/12. This proves repeated allocation reuse
and that measured span's recovery, not leak freedom for every Sony budget
counter.

`wine-process-repeat-05` passed four Windows cycles plus missing-image rejection.
Native descriptor counts before the four probe-child spawns were 100 each.
Windows parent handles rose 12 to 14 on first use, then balanced at 14/14 for
three subsequent cycles and the missing-image attempt. Stock host Wine has the
same first-use increase (13 to 15, then 15/15). The probe queries the server's
handle table with `NtQuerySystemInformation`; `GetProcessHandleCount` is a stub
in this Wine revision and cannot substantiate a leak claim. Descriptor samples
during asynchronous closure can overlap the next child's startup; they are
observations, not a claim that all counts equal the early Wine startup count.

## Focused console validation

The original PE64 probe uses an active background Wine thread while spawning.
Its child checks a distinct Windows PID, quoted arguments and the inherited
environment. An inherited pipe and shared event exercise the existing server;
a non-inheritable event handle must be invalid in the child. The parent checks
process-handle signaling, `WaitForSingleObject`, `GetExitCodeProcess`, and the
full `0x1234` exit code. Missing-image rejection must return
`ERROR_FILE_NOT_FOUND` and balance the parent's Windows handles.

The five-case console batch passed on one guarded build; the final local monitor
thread-handle ownership correction was then checked by the four-cycle repeat
run. Evidence includes exact per-run artifacts and source hashes:

| Case | Evidence | Result |
| --- | --- | --- |
| Basic round trip | `wine-process-basic-05` | PASS |
| CREATE_SUSPENDED / ResumeThread | `wine-process-suspended-03` | PASS; child event stayed unsignaled until resume, prior suspend count 1 |
| TerminateProcess | `wine-process-terminate-03` | PASS; STILL_ACTIVE before termination, full exit code `0x4567` afterward |
| Four cycles and missing image | `wine-process-repeat-05` | PASS; stable measured descriptors and handles after first use |
| Windows parent exits first | `wine-process-parent-exit-02` | PASS; child survived Windows parent exit, then completed and was deleted |

`wine-process-disabled-01` passed three creation attempts without the opt-in:
ERROR_NOT_SUPPORTED, individually balanced handle samples and zero native
children. Missing-image rejection also balanced. The restored production runtime
passed the original PE64 smoke probe in `wine-process-production-restore-01`.
The restored launcher then rendered eight library entries and 120 frames at
59.9 FPS, with zero fault/driver records and a normal shell exit
(`wine-process-restored-launcher-01`). The aggregate record is
`wine-process-milestone-01.json`.
The final host baseline is `process-probe-host-verified.json`: all five cases
passed against host Wine revision `a518d643873ff733bafb31595dfd01ab8bbdf790`;
the separate PS5 Unix-side build base is `1c78596625336a73dbfc035dbbcf0dbdd95b06eb`.
Four focused portable tests passed with AddressSanitizer and UndefinedBehaviorSanitizer;
LeakSanitizer was disabled after its sandbox/ptrace runtime error. The original
helper readback and probe-kind checks also passed. The native PRX import audits
reported no unresolved, unbound or raw-syscall entries for ntdll/wineserver.

Each run has a separate deployment record with artifact hashes, restoration and
helper removal. All successful runs returned through the title's normal paths
with zero captured native fatal signals. Wine also launches `wineboot.exe`
through the same bridge, so native child totals include that bootstrap process.

Failures stay in evidence: `basic-02`/`-03` exposed an untracked directory-FD
handoff and returned ERROR_PATH_NOT_FOUND; the resolved Unix path fixes it.
`terminate-01` passed the PE checks but its overall capture ended as `exited`,
so it remains FAIL. An early child attempted to spawn conhost through unresolved
SystemService imports and Wine caught an access violation; API guards removed
that crash. Descendants are rejected before registration unless the separate
Windows-environment opt-in above is present.
`repeat-02` applied a strict balance check to first-use initialization handles;
the identical stock-Wine behavior established why the measured steady cycles
need a separate baseline. Host deployment/argument errors also retain their
failed records and performed no console launch.

## Supported scope and remaining limits

- Only PE64, ordinary headless creation, inherited handles and the tested suspend,
  resume, termination and parent-exit cases are established. The final probe
  calls `CreateProcessW` directly; earlier probes used the A wrapper, whose Wine
  trace reached `CreateProcessInternalW` and `NtCreateUserProcess`.
- Opt-in absence, 32-bit or another machine and a debug-port request are
  explicitly rejected with STATUS_NOT_SUPPORTED. Creation from a headless
  child requires the additional descendant opt-in and goes through the owning
  title. The helper's historical app field `-1` was unqueried; it does not
  establish that Sony failed to associate the child with the application.
- Debug-process flags cannot supply native ptrace/debug-register operations.
  Child graphics/audio/input, new console hosting, remote memory access and
  remote native priority adjustment are not validated/supported by this bridge.
  Job/breakaway, alternate-parent/token, explicit handle lists and extended
  startup attributes remain unvalidated; Wine's existing handling is retained.
- Generic Unix spawning remains unsupported. The bridge has 16 ownership slots,
  and long runtime/prefix/cwd/DLL/debug strings fail rather than truncate.
- The five-launch cap remains lifted; the existing 60-second pacing remains.
  Direct title `rfork_thread` stays disabled after the earlier panic.

Build and run in this repository (sibling repositories remain read-only):

```sh
bash tools/build-process-probe.sh
bash tools/build-wine-ps5.sh --prx-foundation "$PWD/.deps/prx-foundation" \
  --radv "$(dirname "$PWD")/PS5_Vulkan" --jobs 12
python3 tools/build-wine-child.py
python3 tools/run-process-probe.py --loader .deps/wine-child-loader/BUILD_ID \
  --name UNIQUE_RUN --case basic
```

Use `suspended`, `terminate`, `repeat`, `parent-exit`, `disabled`, `descendants`,
`descendants-repeat` or `descendants-parent-exit` for the other cases. The
runner requires an idle console, verified signed production backups in
`.deps/wine-child-production/`, an absent helper/probe path, and passing PRX
import audits. It writes restoration state before mutations. On failure or a
console disconnect, preserve that deployment record and the signed backups;
do not launch again or replace an in-use runtime. The helper/experimental PRXs
are not packaged or enabled in the production defaults.

## Java and graphics validation (2026-10-04)

The requested order is Java subprocesses, a graphics child, then the Java launcher → Java →
the existing Vulkan Java game configuration with a disposable world and a clean
exit/relaunch. Validation reached the graphics initialization gate; the Java game
was not launched through the Java launcher and the direct-launch workaround remains.

`tests/JavaProcessProbe.java` uses the installed Windows JDK, compiled with its
own `javac.exe` through the pinned host Wine. The C probe launches `javaw.exe`,
matching the existing game configuration; Java's `ProcessBuilder` uses
`java.home/bin/java.exe` for three descendant generations. Each checks its
immediate Windows parent and distinct PID, quoted `two words` argument,
synthetic environment cookie, working directory, UTF-8 stdin including λ,
4 KiB stdout payload, separate stderr marker and EOF. Each parent concurrently
drains both output pipes and checks the full exit code `0x5100 + depth`.

| Run | Measured result |
| --- | --- |
| `java-process-host-01` | FAIL: this JDK's `readAllBytes()` reported `Invalid function` on Wine pipes. The probe now uses ordinary stream reads. |
| `java-process-host-02` | FAIL: the probe expected LF in output that Windows emitted as CRLF. Text assertions now normalize CRLF. |
| `java-process-host-03` | PASS with root `java.exe`; three descendant node verdicts. |
| `java-process-host-04`, `-05` | PASS with root `javaw.exe`; three node verdicts. Final run 05 took 3.17 s and records the JDK/class manifest. |
| `java-process-console-01-deployment` | Guard rejected deployment before mutation/launch: the initial check wrongly compared console `javaw.exe` with host `java.exe`. |
| `java-process-console-02` | PASS, 12.3 s capture; three node verdicts, all eight native children deleted, no fatal/GPU-fault record. |
| `java-process-console-03` | PASS, 11.9 s capture; both Java launcher hashes and release matched compilation inputs, three node verdicts, all eight native children deleted, no fatal/GPU-fault record. |

All JVMs use `-Xint`, SerialGC, two active processors and a 64 MiB heap. This
isolates process semantics; it does **not** prove JIT, large-heap Java game,
graphics, visible console windows or audio/input routing in children. The eight
native children comprise Wine bootstrap, four JVMs and three conhost helpers.
The earlier ignored survey's `java_code_matches_host=false` compared the two
different launcher files; run 03 independently verifies each corresponding file.
The source comment was subsequently clarified without changing class bytes;
console run 03 and final host run 05 used the same class hash
`f80bd9f15b6af9aa1c1f3768b2fd19718bba5eff6afd156c59c552fb1060df36`.

`tools/run-process-probe.py --case java` uploads only the original synthetic
class, verifies the selected `javaw.exe`, descendant `java.exe`, JDK release and
class hashes, and removes the class during restoration. Its final PASS now also
requires probe/class removal. `.deps/java-process-validation/input.json` selects
the existing JDK using the `java_windows_path` key; it is ignored. Rebuild with
`tools/build-java-process-probe.py`, then run the host baseline with
`tools/run-process-host.py --name UNIQUE --case java` and the console runner
with a normal headless loader. No JDK or game contents are vendored.

### Graphics preflight and the actual failure

`tools/build-wine-child.py --gpu-preflight` builds an explicitly experimental
helper. It requests the existing filesystem elevation, then checks module
loading and `sceAgcInit(8)` before starting Wine. Normal helper builds exclude
this code and its Sysmodule import; the service/image preload policy stays
`0x8000000000000002`. No direct rfork, raw syscall or default VideoOut preload
was retried. This is an initialization probe, not a rendered-frame test.

| Run | Result and boundary |
| --- | --- |
| `graphics-child-agc-preflight-01` | FAIL: loading AGC directly before its dependencies caused `0xa0020101 (PRX_NOT_RESOLVED_FUNCTION)`. |
| `graphics-child-agc-preflight-02` | FAIL without fatal signal: the initial one-argument ByName declaration returned `0x80020016`. |
| `graphics-child-agc-preflight-03` | FAIL: the corrected six-argument ByName call still loaded AGC without its dependencies and raised `0xa0020101`. |
| `graphics-child-agc-preflight-04` | FAIL without fatal signal: RegMgr loaded with handle `0x14`; AGCDriver returned `0x80020002`. Klog identifies the underlying GPU VA open error `0x804c0003`, preceded by the literal log text `gbase_gvmsw_open 6978 error` (the number is not decoded as an errno). Both native helpers, 716 and 719, were deleted by Sony. |

ByName returns a module handle and does not load the initializer's dependencies
for us. The current helper explicitly loads RegMgr, AGCDriver and AGC in order,
accepts nonnegative handles, then guards the AGC init lookup. The six-argument
declaration is also used by upstream's
[loader](https://github.com/etaHEN/etaHEN-Plugins/blob/main/libhijacker/source/elf/elf.cpp);
no etaHEN component is installed or called by this probe. The successful AGC
handle/init path remains unexecuted because driver loading stops first.

The GPU error occurred **after child entry and elevation**, through a recoverable
module-load result in run 04. It is not a test of display contention: no surface,
swapchain or display handoff was attempted. It does not prove which budget,
process association or GPU VM setting must change, nor that child rendering is
impossible. Offline metadata also shows `libSceVideoOutSecondary` imports normal
VideoOut and exports a separate library namespace without the standard
Open/Close/RegisterBuffers/SubmitFlip exports; its filename alone is not evidence
of a replacement child display driver.

Every graphics attempt restored the signed ntdll/wineserver and removed the
helper/probe. The console remained reachable. Final production verification
`child-java-graphics-production-restored-01` passed 120 frames at 59.9 FPS,
showed all eight library entries, ended idle, and recorded zero fatal or driver
errors. All 17 portable host tests passed outside the tracing sandbox (the
initial sandbox attempt failed because LeakSanitizer cannot run under ptrace).

The follow-up below establishes a working Windows graphics-child arrangement.
It reuses the original title's GPU context; a new GPU VA lifecycle for the helper
remains unresolved. Full Wine/Proton conformance is not a prerequisite for the
remaining Java launcher → Java → the Java game validation.


## Graphics child in the original title (2026-10-04)

**A headless Windows parent now launches a DXVK/Vulkan graphics child with
verified pixels, display handoff and teardown.** The native placement differs
from the Windows tree: the headless parent runs in a Sony-managed helper, while
its graphics child is the first Wine client in the original title. Each Windows
process still has its own address space and Wine identity; the same wineserver
owns the parent relationship, arguments, environment and inherited handles.
No second Wine process is emulated inside an already-running Wine client.

### What the driver comparison established

`graphics-child-device-query-01` failed overall (the runner saw title exit rather
than a finished verdict). Both helpers' own `/dev/gc` opens failed with errno 2,
before elevation (uid/euid 1) and afterward (0). The same GPU VA errors appeared.
This rules out the tested elevation timing as a sufficient fix.

`graphics-child-gpu-donor-query-01` passed its headless lifecycle gate, but its GPU
query failed; that original PASS must not be read as graphics evidence. The
runner gained an explicit GPU-query gate before run 02. In run 02 the original
title opened `/dev/gc` and ioctl `0xc004812e` returned 0 with value 0. The same
transferred descriptor in either helper returned -1/EPERM before and after
elevation. Run 02 therefore failed overall while headless Wine still passed.
Descriptor donation alone does not give the helper GPU access. These probes did
not map GPU memory or submit work.

Read-only local firmware metadata/call analysis identified the driver's device
open and query path; no firmware code or bytes were copied here. No unverified
VM-container ABI, raw syscall, direct rfork, system-only library or new payload
was attempted.

### Placement and opt-ins

```text
Windows: headless launcher -> graphics child -> headless grandchild
Native:  original title    = wineserver + first/only graphics Wine client
         Sony helper      = headless Windows launcher
         Sony helper      = headless grandchild and other headless processes
```

`WINE_PS5_OWNER_BOOTSTRAP=1` loads the server without entering `__wine_main` in
the original title. A fresh server socket plus the initial argv (bounded to
64 entries/4096 bytes) starts the launcher in a helper. That explicitly marked
initial helper may request Wine's own `wineboot.exe` through the broker; ordinary
helpers retain their descendant gate. Bootstrap protocol is `0x50573503`.
The full 10,256-byte bootstrap needs explicitly sized SOCK_SEQPACKET buffers;
a host test transfers the complete packet and two descriptors and checks cleanup.

`WINE_PS5_GRAPHICS_HOST=1` additionally registers one graphics-start callback in
the title and keeps its normal input/performance/stop loop running. A requested
child environment containing `WINE_PS5_GRAPHICS_CHILD=1` routes the original
server startup socket into that callback. Patch 0927 binds the title's existing
server callbacks before attaching `WINESERVERSOCKET`, and consumes the private
Windows routing variable so the game can create ordinary headless descendants.
It does not substitute argv, handles or an independent server for the child's
Windows startup parameters. The original title uses the established RADV link
recipe and normal presenter/display-release/exit paths.

The native owner also waits for helpers when the initial launcher exits without
claiming graphics, and binds registry flush directly to the server in this
server-only case. Its polling result is a separate -1/pending, 0/success,
1/failure verdict; it never treats high-bit Windows exit codes as pending. The
full DWORD remains in Wine and the native completion log. Missing or malformed
initial completion becomes failure, rather than leaving the owner waiting.

One graphics client can claim the native title. A second returns errno EBUSY,
translated to Windows ERROR_BUSY (170), before another client starts. Relaunch the
native title for another game; Wine globals are never reset/reused. The initial
launcher must be headless. Automatic graphics selection, complete profile/native
environment forwarding to the initial helper, a graphical Java launcher UI, multiple
concurrent GPU clients, graphics termination/suspension and audio/input routing
are not established by these probes. NTSYNC remains disabled for this mode.

### Attempts and measured results

| Run | Observed result |
| --- | --- |
| `graphics-owner-bootstrap-01` | FAIL before helper launch: new native bootstrap functions were missing from the PRX descriptor. |
| `graphics-owner-bootstrap-02` | FAIL before helper launch: the Wine loader expects logical `wineserver.so`; a `.prx` name became `.prx.prx`. |
| `graphics-owner-bootstrap-03` | FAIL: server started, but the larger bootstrap could not reach the helper with the default socket buffers. |
| `graphics-owner-bootstrap-04` | FAIL: packet delivery and Wine entry worked, then the initial helper's descendant restriction rejected `wineboot.exe`; its pre-PEB failure raised native SIGSEGV. The console stayed reachable and production was restored. |
| `graphics-owner-bootstrap-05` | PASS, 6.6 s capture: headless initial client plus three descendant generations, all five native helpers deleted. |
| `graphics-process-host-01` | FAIL: Windows parent/pipe metadata passed, but the pinned host build has no window driver and could not create the probe window. It is not a GPU baseline. |
| `graphics-owner-render-01` | PASS: Windows parent/env/inherited pipe, 180 frames in 3014 ms, triangle readback and explicit teardown; both helpers deleted. Loading screen disabled. |
| `graphics-owner-render-02` | PASS: 180 frames in 3000 ms, loading-display release and five expected pixel samples; both helpers deleted. |
| `graphics-owner-render-03`, `-04` | FAIL overall: graphics, headless grandchild and second-client rejection passed, but parent handles counted 12/14. Derived handle types were a directory and mutex: the first OutputDebugString lazily opens its reporting state (`kernelbase/debug.c`'s DBWinMutex). The probe now warms this before the baseline; no runtime handle was hidden or closed by the probe. |
| `graphics-owner-render-05`, `-06` | PASS in fresh title sessions: 180 frames in 3007/3000 ms, five matching pixels, loading handoff, headless grandchild during GPU lifetime, second-client ERROR_BUSY, parent handles 14/14, all three helpers deleted, zero native fatal/GPU-fault records. |

`graphics-owner-no-render-parent-exit-01` additionally passed with graphics
reserved but never claimed: the Windows parent exited first, three descendants
completed afterward, all five helpers were deleted, and the registry was saved
before title replacement. `graphics-owner-failed-exit-01` deliberately exited
with `0xffffffff`; the title reported FAIL (as expected), logged that full
completion code, finished without a stall or fatal signal, deleted both helpers
and restored production. The deployment-level expected-failure check passed;
the application's FAIL is preserved in the underlying run record.

`graphics-owner-render-07` passed the same complete graphics checks with the
final owner wait/registry-flush/completion changes: 180 frames in 2997 ms,
14/14 parent handles, all three helpers deleted and production restored.

All three final runs produced the same 1280×720 RGB readback SHA-256
`f224253333555e210dde7c71af6aa109cc270076f71c842eb17e796a3d7ec6d8`;
the expected red/green/blue interpolated triangle was also visually inspected.
The rendering-loop rate is about 60 FPS. The broader native perf interval includes
startup/descendant time, so its approximately 35 FPS is not the rendering-loop
benchmark. Captures stay ignored under `klog/`; only hashes/numeric evidence land
in the repository. Every attempt restored both signed production PRXs, removed
its synthetic files and, where deployed, restored the original signed title.

Build/run with existing pinned dependencies:

```sh
python3 tools/build-child-probe.py --kind wine-owner
python3 tools/build-wine-child.py
bash tools/build-graphics-process-probe.sh
bash tools/build-wine-ps5.sh --prx-foundation "$PWD/.deps/prx-foundation" --radv "$(dirname "$PWD")/PS5_Vulkan" --jobs 12
python3 tools/run-process-probe.py --owner-build .deps/child-probe/OWNER_ID --loader .deps/wine-child-loader/HELPER_ID --case graphics --loading-screen --name UNIQUE_RUN
```

The runner verifies signed production backups and readbacks, then restores the
parent title and both PRXs and removes the synthetic renderer, parent probe,
helper, readback and child logs. A final PASS requires successful restoration.
The isolated builder overlays only repository-owned sources into an ignored
archive build; production artifacts and sibling repositories remain untouched.

The eight existing host lifecycle/descendant cases and all 17 portable sanitizer
tests passed; all 95 patches replayed cleanly. The original-title descendant
console path is checked separately. Next is integrating the explicit selection
and launcher environment for the Java launcher → Java → the Java game, then verifying a disposable
world, clean exit and fresh-title relaunch. Keep the direct-launch workaround
until that chain passes; this result does not prove general graphics children.

Final restored production check `graphics-owner-production-restored-02` passed
120 frames at 59.9 FPS with all eight library entries, no fatal/driver errors,
and the console idle afterward. Its signed launcher hash matches the original
production artifact. graphics run owner-validation-01 (2026-10-04) gathers the
three full graphics runs, hashes, handle counts and final restoration result.

## The open-file budget (2026-10-05)

The Java launcher and Java ran out of files within milliseconds of each other (run chain-05 (2026-10-05) to `-07`: "Too many open
files", Java exit 2), although every process had an open-files limit of 13,952. The native probe
`fd-budget-01` (`native/proton_fd_budget_probe.cpp`, the title and one Sony-managed helper, nothing else
running) settled what the console counts:

| Step | Title's headroom | Helper's headroom |
| --- | ---: | ---: |
| baseline | 249 | 249 |
| title holds 40 files | 209 | 209 |
| title passes them to the helper and closes its own copies | 209 | 209 |
| helper closes them | 249 | 249 |
| title holds 40 dups of one file | 248 | 248 |
| title maps 40 files and closes their descriptors | 249 | 249 |
| title holds 40 sockets, 40 pipe ends, 20 kqueues, 20 anonymous shared memory objects | 249 | 249 |
| title holds 10 directories | 239 | 239 |
| helper is closed while holding 40 files | 250 (all returned) | – |

So a title and all its child processes share **one budget of about 250 open files**, directories included.
A file costs one however many processes hold descriptors of it, until the last descriptor closes. Dups,
copies passed over a socket, sockets, pipes, kqueues and mapped files whose descriptor is closed cost nothing.

Wine's server opens every Windows file of every process, so the server, in the title, is where the budget is
kept: it parks idle files (`ps5platform/fdv.h`) and reopens them on the next use. Two things defeated that
with children. A descriptor sent to a child counted as in flight for good, because no receiver in the title
ever claimed it, so the file could never be parked. And the child cached its copy, which kept the file open
whatever the server did. Patch 0929 lends files instead: a child gets regular files and directories
uncached, and the server pins a file it sends only until the receiving thread's next request, by which time
the child has closed its copy. With children the title parks above 150 open files rather than 192
(`native/proton_wine.cpp`), and each child loader parks its own few above 24 and logs a census of its
files every 10 s.

The test is `process-probe.exe --file-budget` (`--case file-budget`): the root makes 450 files of 8 KiB, holds
150 open and starts three descendants that hold 100 each; once all 450 are open, each process reads every one
of its files four times, 512 bytes at the file pointer, and checks each block's contents.

| Run | Runtime | Opened | Read | Server parks / reopens | Result |
| --- | --- | ---: | --- | --- | --- |
| `file-budget-a01` | before 0929 | 450 | none | 1,097 / 855, 4 reopens failed | "cannot reopen ... Too many open files", then "sendmsg: Too many open files", and the server ended all four processes |
| `file-budget-b01` | 0929 | 450 | 7,200 blocks, 0 wrong | 2,816 / 2,516, none failed | PASS: every child exited 0x600d, no protocol errors |

run mp-01 (2026-10-05) (`--case program --profile 9`) ran the official game launcher client, with its normal multi-process
browser, as the first Windows program under 0929. No process ran short of files: the server held 111 (96
parks, 81 reopens, no limit hits) and each child 2. wineboot, its system-information tool, its CEF browser helper and
its crash reporter started as descendants. Its CEF browser helper could not start its own children,
since the Windows environment the game launcher gave it lacked the `WINE_PS5_DESCENDANTS=1` opt-in, and then faulted
(c0000005) executing inside its client library after unloading it. The game launcher ignored the title's close request,
so the title was killed with `tools/kill-hung-title.py --force` and production was restored.

## Layouts: child processes by default (2026-10-05)

Child processes are no longer an experiment: every library launch allows them. Where a program's processes run
is its **layout**, chosen by the title (`native/proton_wine.cpp`) from `PROTON_LAYOUT` in `game.json`'s `env`, or
else by the program:

| Layout | First program | Processes it starts | Chosen for |
| --- | --- | --- | --- |
| `title` | in the title, with the title's own graphics (OpenGL, Vulkan, 120 Hz) | Sony-managed helpers, without graphics | every program not listed below |
| `launcher` | A helper | helpers; every one draws through the title's GPU broker | The known game launchers' and a Java launcher's executables |
| `single` | in the title | none: CreateProcess fails as before | `PROTON_LAYOUT=single` only |

A child may start its own children by default (patch 0930): the game launcher's browser gave its renderers an environment of
its own, without the old `WINE_PS5_DESCENDANTS=1` opt-in, and every one was refused (2026-10-05; record removed from the tree; in git history).
`WINE_PS5_DESCENDANTS=0` declines; wineboot stays refused until its services are set up on the console.
In the launcher layout the title has no Windows program to close: a stop request that the programs ignore
ends them through the server after 10 s, and Sony ends each helper (`PROTON_STOP_TERMINATE=0` declines).
Each helper logs to `wine-child-<pid>.log` in the title's folder; a launch removes the previous session's.

**What production ships.** `tools/build-production.sh` builds the runtime twice from one patch series, with the
OpenGL SDK for the title and without it for helpers, the GPU broker's Vulkan client, the helper loader and the
title (with the broker, `PS5_GPU_BROKER`). Helpers load the title's modules except two:
`win32u-helper.prx` (no OpenGL driver, whose system libraries a helper cannot use) and `libvulkan-helper.prx`
(the broker's client in place of RADV), which `pw_wine_dl.c` picks in a helper. A separate folder for helper
modules did not work: Wine finds its data and DLL folders from ntdll's, which must be named `x86_64-unix`
(`battery-r2-01`: a helper's wineboot found no NLS files and faulted, and the crash report closed the title).

**The battery** (`tools/run-battery.py`) runs ten ordinary launches on the installed title and runtime.
battery run r2-02 (2026-10-05), on the production install built from this state, passed all ten:

| Case | Layout | Helpers | Result |
| --- | --- | ---: | --- |
| `pe64` (wine-probe.exe) | title | 1 | PASS |
| `d3d11` (DXVK, frames and readback) | title | 1 | PASS |
| `opengl` (gl-vertex-probe.exe) | title | 1 | PASS |
| `children` (process-probe.exe) | title | 3 | PASS |
| `descendants` | title | 4 | PASS |
| `file-budget` (450 files, four processes) | title | 4 | PASS |
| `library-entry` (a library game, the case since renamed; 60 s, stopped from the title) | title | 1 | PASS |
| `launcher-children` | launcher | 4 | PASS |
| `launcher-render` (DXVK children and descendants) | launcher | 6 | PASS |
| `launcher-file-budget` | launcher | 5 | PASS |

The one helper of every title-layout case is Wine's own wineboot, which the prefix asks for at each start
(it now runs, and stops short of the services).

## The desktop: helpers' windows on screen, the touchpad's pointer in them (2026-10-05)

In the launcher layout no Windows program runs in the title, so the title has to be the desktop: show the
windows of programs in helper processes, and give them the pointer.

**Pictures.** A helper's user driver composes its visible windows into one picture, as in the title. Its sink
(`wine/ps5/pw_wine_sink.c`) had no one to give it to; now it copies each into a shared page
(`src/pw_frame_page.h`, 1080p BGRA, a sequence number odd while it is written) and hands the title the page once
(`PW_CHILD_FRAME`). The GPU broker's display is the compositor: every 4 ms it shows the newest whole picture of
whichever source holds the screen, beside the Vulkan clients it already presented. A source keeps the screen
while it presents at least every 300 ms, after which the next source to present takes it; the process of the
foreground window takes it whenever it presents. A client that does not hold the screen is told its frame was
shown a frame's time later, so it paces as it would on a display. Patch 0932 has the driver say where its picture
sits on the desktop and leave the cursor to the title, which draws an arrow where the desktop's input left it.

**Pointer and keys.** Patch 0931 lets the title inject input into the desktop itself, from the server thread, as
a device would: the touchpad's motion and clicks go to the window under the cursor or the foreground window,
in whichever process owns it, and a stop request posts WM_CLOSE to the foreground window. The server keeps the
cursor and the foreground window's native process for the compositor. A helper now also inherits the title's
settings for Wine and its graphics (the bootstrap carries them, protocol `0x50573506`): without
`WINE_PS5_DESKTOP` a helper's Wine had reported an 800x600 monitor.

`desktop-01` (`tests/desktop-probe.c` as the first program in the launcher layout, the pad script
`tests/pad-scripts/desktop-pointer.txt` moving a finger on the touchpad and clicking): the helper's 800x500
window at (200, 150) was attached, took the screen as the foreground process and was shown; in the captured
1920x1080 scanout 92.3% of the pixels are the window's own colour (0x20a0e0) and the rest its Wine frame and
caption. The window counted its first pointer move 14.0 s after it was shown, a press 0.8 s later and a release
0.15 s after that: 2 moves, 1 press, 1 release, exit 0.

battery run r3-01 (2026-10-05), on the production install built from this state, passed all eleven cases, the ten of
battery run r2-02 (2026-10-05) and `launcher-desktop` (the desktop probe and pad script above).

run desktop-01 (2026-10-05): the game launcher (launcher layout) for 180 s. Fourteen helpers started: the launcher's main executable, wineboot,
its system-information tool, a console host, its CEF browser helper and, now that descendants are the default, its GPU,
network-service, storage-service and renderer processes, and the crash reporter. The compositor showed
the launcher's main executable's own window, a 313x114 "[the launcher] - Error" message box, as the foreground process. The browser's logs say
why there was no store: "Failed to read DnsConfig" and "GetAdaptersAddresses failed: 2" (no network yet), and
its GPU process was restarted with `--use-gl=disabled` after the first one ended. No Vulkan client presented. At
the run's budget the title's stop ended every helper through the server and returned to the launcher by itself;
the launcher's main executable ended with c0000005.

## The network: names and TLS (2026-10-05)

A title had no DNS: Winsock's Unix side (`wine/ps5/pw_ws2_32_libc.c`) answered only numbers, `localhost` and the
console's own name, and the game launcher's browser logged "Failed to read DnsConfig" and stopped at an error box
(run desktop-01 (2026-10-05)). A dotted name now goes to the console's own resolver, libSceNet's
(`sceNetResolverStartNtoa`, five seconds, two retries, as the ProsperoLight title asks it), for an IPv4 address.
The network is the title's: it calls `sceNetInit` at start and does every lookup; Winsock asks ntdll
(`pw_wine_resolve`), which answers in the title and, in a helper, asks the title over the helper's control socket
(`PW_CHILD_RESOLVE`). A single label still never goes out: resolving the
computer name a prefix made on a PC made GTA IV believe it was online and wait forever. `WINE_PS5_DNS=0` in a
game's environment turns lookups off. The console's DNS server is whatever its network settings name.

Windows' own TLS had no backend: Wine was built `--without-gnutls`, so schannel (secur32), bcrypt's ciphers and
keys, and crypt32's PKCS#12 import reported "no schannel support". GnuTLS 3.8.9 is now built for the console
from its pinned tarball, over Nettle 3.10.1 with its small GMP, with GnuTLS's own libtasn1 and libunistring and
no p11-kit, compression or system trust store (`tools/build-wine-ps5.sh`), and linked into `libgnutls.prx`,
which bcrypt, secur32 and crypt32 load with dlopen as `libgnutls.so`; their Unix sides are now PRX modules too.
GnuTLS draws its randomness from `/dev/urandom`, with no raw system call. A Windows program trusts the public
certificate authorities in the runtime's `share/wine/ca-certificates.crt` (Mozilla's, as curl extracts them,
pinned by date and hash), which the title names to crypt32 (`WINE_ADDITIONAL_CERTS_DIR`) and helpers inherit.

Why helpers do not touch libSceNet: a helper does not preload it, so a direct `sceNetInit` import in the helper
loader stayed unbound and the call jumped to address 0 (`dns-01`: the helper faulted before Wine started, and the
system's crash report for the helper closed the whole title; a native fault in any helper does that). Loading it
by name in each helper worked for one helper, but with a second one loaded the title's own threads faulted
together at one address inside ntdll's data (`battery-r4-01`, `-02` and `-03`, `children`: reproduced three times,
also without the title's `sceNetInit`). With helpers leaving the library out (`children-nodns-01`) and with the
title alone holding it (`children-nodns-02`) the case passed. So only the title loads it, and `ws2_32.prx` imports
nothing of it.

On the console:
- `dns-02`: `example.com` resolved through the console's resolver to a public address, `localhost` and the computer's
  own name to loopback, and an HTTP `HEAD` over TCP to port 80 got `HTTP/1.1 200 OK`. The wineboot helper of that
  run loaded libSceNet (`module=0x14 init=0`).
- `tls-03` (`tests/tls-probe.c`): **The console's clock read 2026-01-05**, nine months behind, so every current
  certificate (they live about 90 days) is not yet valid there and the strict request failed (12157). With only
  the certificate's dates waived, `https://example.com/` answered 200: schannel over GnuTLS completed the
  handshake on the console. The probe then builds each chain itself through crypt32: example.com's has only
  "not time valid" (0x1), so the bundled roots were loaded and trusted; `untrusted-root.badssl.com`'s also has
  "untrusted root" (0x21). Wine's WinHTTP checks a certificate's date before its root and skips the root once the
  date is waived, which is why the probe reads the trust bits directly. **HTTPS on this console needs its clock
  set** (Settings, System, Date and Time); nothing in a title can change it.

battery run r4-04 (2026-10-05), on the production install built from this state, passed all fourteen cases: the eleven of
battery run r3-01 (2026-10-05) and `dns`, `tls` and `launcher-tls` (the TLS probe in a helper, whose lookups go to the title).
The console's clock had been set by then: `tls` read 2026-10-05, `https://example.com/` answered 200 with the
strict check, and the chains read 0x0 for example.com and 0x20 (untrusted root) for `untrusted-root.badssl.com`.

## Prefixes, services and installers (2026-10-05)

A game's prefix had to be made on the PC and copied over (`tools/deploy-wine.py --prefix`): wineboot could not run
on the console, so a prefix was never made or brought up to date there, no service ran, and a program asking the
service manager anything got nothing. Now:

- **The title makes a missing prefix.** It makes the folder and lays down the drive links (C: to `drive_c`, Z: to
  `/`) in the link table the Unix side reads, since Wine makes them only where a client connects to the server
  itself, which a launcher-layout program does not (`prefix-01`: no C:, so neither wineboot nor the program was
  found). Wine and wineboot fill the rest from `wine.inf`, as on a PC.
- **wineboot may start its helpers** (patch 0933): rundll32, control, winemenubuilder, msiexec. `services.exe` starts
  only where `WINE_PS5_SERVICES=1` is in the starting process's environment: the title sets it for a launch whose
  prefix wineboot must make or bring up to date (wineboot's own test: the prefix's `.update-timestamp` against the
  runtime's `wine.inf`), and a game asks for it in its `game.json`. Not by default, even in the launcher layout:
  `services.exe` and what it starts are five helpers, and the console runs seven beside the title. In
  battery run r5-01 (2026-10-05), with services on in every launcher-layout launch, an eighth helper was refused
  (`addLocalProcess` 0x80aa0008) and `launcher-render` and `launcher-file-budget` failed: their own children never
  started. An installer does not need them: msiexec installed its package with none (`prefix-10`).
- **Wine Mono 10.4.1 and Gecko 2.47.4 ship in the runtime** (`share/wine/mono`, `share/wine/gecko`, pinned by the
  hashes Wine's appwiz.cpl names), and wineboot installs them into each new prefix: .NET Framework programs run.
  Without them wineboot asked to download them in a dialog nobody answered, and setup never ended (`prefix-02`,
  where the program then had no `APPDATA` or `ProgramFiles` and 7-Zip installed into the game's folder).
- **A helper without the GPU broker has Vulkan with no driver, not a broken loader.** The title layout's helpers
  are headless: no broker, so the helper's Vulkan module (`native/proton_gpu_vulkan_exports.c`) is bound to
  nothing. It answered `vkGetInstanceProcAddr(NULL, "vkEnumerateInstanceExtensionProperties")` with NULL, which
  the loader's contract does not allow, and win32u called it unchecked the first time a process read its display
  configuration. The fault left two once-only initializers (Vulkan's and D3DKMT's) unfinished, so the process's
  next display query waited in `pthread_once` forever. That was every stall here: `rundll32` at its first window
  during wineboot's setup (`prefix-04`, `-05`), and `msiexec` at an installer's shortcuts (`prefix-06` to `-09`),
  where COM's apartment makes a message window. The server's wait watchdog (`WINE_PS5_WAIT_WATCHDOG=1`) showed
  msiexec's main thread outside any wait for 297 s, its last request `open_key`; a trace (`--trace` in the tool)
  showed the display key opened and nothing after. Now the global commands exist and report no driver
  (`vkCreateInstance` returns `VK_ERROR_INCOMPATIBLE_DRIVER`), as a loader with no driver does, and a Windows
  program in such a helper is told Vulkan is unavailable.

`tools/run-prefix-test.py` adds a throwaway library entry whose prefix does not exist, holding
`tests/prefix-probe.c` and 7-Zip's 64-bit MSI package (pinned), runs it, and removes the entry and its prefix once
the console is idle.

`prefix-03` (launcher layout): the prefix was made on the console, 3,977 files and 573 MB with Mono and Gecko
installed; the program saw `system32` and its modules, a Windows version in the registry and its `APPDATA`; the
service manager answered and listed 22 services (`services.exe`, `svchost.exe`, `plugplay.exe` and `winedevice.exe`
ran as helpers); `msiexec /i` installed 7-Zip into Program Files (exit 0) and the installed `7z.exe` ran (exit 0).
88 s from launch to return, the console idle before the removal.

`prefix-12` (title layout, the default for a game): the same prefix made in the game's own launch, wineboot and
its 22 services as helpers of the title's program, then the MSI installed (exit 0) and `7z.exe` run (exit 0); 86.8 s,
one launch. `prefix-13` (launcher layout): the same, 88.3 s. Before the Vulkan fix the title layout's msiexec never
ended (`prefix-06`: 691.7 s, the run timed out; the probe now ends a program that does not end in 300 s, so a
stall fails the probe instead of holding the title: `prefix-07` to `-09`, 388 s each).

On the build committed with this (services only during setup or on request): `prefix-14` (title layout) 87.7 s and
`prefix-15` (launcher layout) 88.2 s, both passing as above, the console idle before the removal. battery run r5-02 (2026-10-05):
13 of 14; the launcher cases that the services had starved passed again, with no helper refused. Its `dns` case
probed correctly (names resolved, `HTTP/1.1 200 OK`, the title back in the launcher and `ends: status 0` in klog)
but was recorded as "exited": the shared runner polls the console's processes once a second, and the poll fell in
the moment LoadExec replaces the game with the launcher. The same case alone: `battery-r5-03` the same race,
`battery-r5-04` and `-05` passed.


## A game launcher on the round-5 build (2026-10-05)

run desktop-01 (2026-10-05) showed a "[the launcher] - Error" box, put down then to the missing DNS. With names and TLS working the
box was still there (2026-10-05; record removed from the tree; in git history): "There was a problem with your game launcher installation. Please reinstall [the launcher]."
Every game launcher run since run 02 (2026-10-05) had logged why: `InternalAPI_Init_Internal failed, most likely because you are
missing a 32-bit dependency of [its client library]`. Three causes, one under the other:

- **Proton's lsteamclient bridge** (run r5-02 (2026-10-05), with `+module`). Proton runs games against the Linux game launcher
  client: ntdll loads `lsteamclient.dll` for its client library and points that module's `tier0_s64` and
  `vstdlib_s64` imports at ntdll. Here there is no Linux game launcher client and no `lsteamclient.dll`, and the game launcher
  client is itself a Windows program: its client library got about sixty stub functions, its DllMain returned
  0, and the launcher's main executable loaded it twenty times before giving up. Patch 0934 turns the bridge off unless
  `PROTON_DISABLE_LSTEAMCLIENT=0`. After it (2026-10-05; record removed from the tree; in git history) the launcher's main executable loaded the module once and created its UI
  browser.
- **No fonts for DirectWrite** (run r5-03 (2026-10-05) to `-05`). The game launcher's browser then died every ten seconds with a stack
  overflow (c00000fd), and the game launcher restarted it: about 80 processes in 240 s. Chromium's
  `SystemFonts::Initialize` marks itself done only after it has made every system font, and a font it cannot make
  falls back to the system font, which runs `Initialize` again. It could make none: DirectWrite finds fonts only
  through the registry's `Fonts` key, which names files in `C:\windows\Fonts` (`0x88985003`, not found, for
  every one of them, Tahoma included). A prefix made on a PC by Proton has those files as symbolic links to
  Proton's and Wine's fonts (proton: `create_fonts_symlinks`), and the links did not survive the copy to the
  console; GDI still found Wine's fonts in the runtime, so nothing else had shown it. The title now copies the
  runtime's TrueType fonts into `C:\windows\Fonts` of the prefix it starts (again when their size differs), as
  Proton links them. run r5-06 (2026-10-05): six fonts copied, no overflow, 12 processes in 240 s, and for the first time
  the game launcher's log says `BrowserReady` and its shared JavaScript context loaded.
- **The console's seven helpers.** The browser then asked for three renderers and a metrics process, and the
  console refused each (`addLocalProcess` 0x80aa0008, "with 7 alive"): the launcher's main executable, the browser, its GPU, network
  and storage processes, the crash reporter and one more were already running. With no renderer the interface was
  never drawn: no frame reached the compositor. `-cef-single-process` no longer helps: this client
  (1788652215) does not pass it to the browser (run r5-07 (2026-10-05), the same twelve processes and three refusals).
  the game launcher's normal browser needs more processes than the console gives a title, so the next step is a design one:
  fewer processes (Chromium's switches that keep the GPU and network services in the browser, a single
  renderer), or more than one Windows process in a helper.

Proton's fonts proper (Liberation as Arial, Times New Roman and Courier New; Source Han Sans and Noto for CJK and
Indic scripts) are not built for the runtime yet; the registry of a PC-made prefix still names them, and
DirectWrite skips them as not found.

## How many processes (2026-10-05)

Every Windows process but the title's own runs in a helper the console starts for the title
(`sceSystemServiceAddLocalProcess`), and the console stops at a number of them. `process-probe.exe --process-limit`
measures it: it starts children one at a time, each committing and touching its memory and waiting, until the
console refuses one or sixteen run; once with 1 MB children, once with 128 MB children; then it ends one child and
times how soon a new one starts.

| Run | Layout | 1 MB children | 128 MB children | Refusal | An ended child's place |
| --- | --- | ---: | ---: | --- | --- |
| `limit-01` | title (the probe in the title) | 7 | 7 | `addLocalProcess` 0x80aa0008, "with 7 alive"; `CreateProcess` error 5 | taken on the first try, the new child ready in 776 ms |
| `limit-02` | launcher (the probe a helper itself) | 6 | 6 | The same, 7 alive | The same, 807 ms |

So the console runs **seven helpers beside the title**, the title's program or not, nested or not, and it is a
count, not memory: 128 MB children stopped at the same number. A helper's place is free by the time its end is
seen. The spawn's options were not varied: only the headless mask among them has a known meaning, and the static
trace leaves the rest unresolved (`SERVICE_SPAWN.md`). The battery's `process-limit` case keeps the measurement.

Seven is little for a launcher. The game launcher's browser alone asks for itself, its GPU, network and storage processes,
renderers and a metrics process, beside the launcher's main executable and its crash reporter (2026-10-05; record removed from the tree; in git history). What follows works on
the count for every program: spend no place on processes of our own, wait for a place rather than fail, and look
for a way to start helpers outside the count.

### Places our own processes take

Two kinds of Windows process are ours rather than the program's. **wineboot** starts with each session, before the
first program goes on, and ends once the session is set up; in the game launcher's runs it had ended before any refusal
(run r5-06 (2026-10-05), the seven alive at each refusal were the game launcher's own: the launcher's main executable, its crash reporter, four or five
browser processes and, at three of the four, its short-lived GL driver query tool). It costs a place only at the
start, so it stays. **conhost.exe** started with every console program a program without a console starts (a
launcher's tool: its system-information tool, `7z.exe` under the prefix probe), and with every one started with
`CREATE_NO_WINDOW`, and lived as long as it: two places for one program. Nobody can see a console on the PS5, so
patch 0935 starts such a program as Wine already starts the session's first console program when no terminal is
attached: with no console, its standard handles as given. `CREATE_NEW_CONSOLE` and `AllocConsole` still make one;
`WINE_PS5_CONSOLES=1` keeps Windows' behaviour.

`process-probe.exe --console-children` gives up its own console, as a launcher has none, and starts a console child
three ways, counting `conhost.exe` while each runs and reading what the child wrote through a pipe:

| Start | Before 0935 (`console-01`) | With 0935 (`console-02`) |
| --- | --- | --- |
| no flags, parent without a console | A conhost, the child has a console | no conhost, no console; it wrote through its handles |
| `CREATE_NO_WINDOW` | A conhost, a console | no conhost, no console; it wrote |
| `CREATE_NEW_CONSOLE` | A conhost, a console | A conhost, a console |

battery run s2-01 (2026-10-05): 15 of 16 (with `process-limit` and `console-children`); its `dns` case probed correctly but the
runner's process poll fell in the LoadExec gap again, and alone it passed (`battery-s2-02`). `prefix-17` (a new
prefix in the title layout): passed in 85.9 s, and `7z.exe`, which the probe starts with `CREATE_NO_WINDOW`, ran
with no `conhost.exe` beside it.

`prefix-16` found a fault in the font copy of `fba8e4d`: for a new prefix the title made `C:\windows\Fonts` before
wineboot had run, and with a `C:\windows` but no `system32` Wine could not start wineboot (c0000034) nor load
kernel32. The fonts now go only into a prefix that has its `system32`; a new one gets them at its next launch.

### Waiting for a place

A start the console refused failed the program's `CreateProcess` at once, though places come free all the time: a
launcher's tools end within seconds (the game launcher's GL driver query tool held a place at three of its four refusals in
run r5-06 (2026-10-05)). Now the title waits: on a refusal it tries again at each helper's end, and at least once a second,
for up to `WINE_PS5_SPAWN_WAIT` seconds (20 by default; 0 fails at once as before; at most 300). Every start goes
through the title, a helper's included, so the title's setting is the one that counts; a helper waits up to 330 s
for the title's answer, longer than any wait. Each wait is logged with how long it took.

`process-probe.exe --spawn-wait` (run with an 8 s wait) fills the places, then starts one more while a thread ends
a child 3 s later, then one more with every place taken:

| Run | Layout | Places | One freed 3 s in | Every place taken |
| --- | --- | ---: | --- | --- |
| `spawn-wait-01` | title | 7 | The place came free 3,224 ms after the refusal; the child was ready at 3,944 ms | refused after 8,863 ms (9 tries), `CreateProcess` error 5 |
| `spawn-wait-02` | launcher | 6 | 3,223 ms; ready at 4,066 ms | refused after 8,863 ms |

Waiting helps a program whose processes come and go; it cannot give one that needs eight at once its eighth.

battery run s3-01 (2026-10-05): 18 of 18, with `spawn-wait` and `launcher-spawn-wait`. The game launcher on this build (run s3-01 (2026-10-05), 240 s):
the console refused three starts and each waited for a place, 504 ms, 8,365 ms and 403 ms, and started; nothing
failed for want of one. Fourteen processes ran in all, and the launcher's main executable put a window on the screen for the first
time (128x66, then 700x350; its first frame, the one captured, was black). The browser's own window did not reach
the screen, and its GPU process was started again with `--use-gl=disabled` after two minutes: what the game launcher's
interface needs to draw is the next question, a game launcher one.

### Helpers outside the count: a process the payload loader starts

The seven are the title's local processes. The console's payload loader (elfldr, which starts ftpsrv, klogsrv,
kstuff and the rest) makes processes of its own: from its privileged process it calls `rfork_thread`, the child
leaves its budget (`sys_budget_set(0)`), runs a system app's executable under `ptrace`, and elfldr stops it at
`main` and maps the payload in. Such a process is no application's, so it is not one of the title's seven. Could
it carry a Windows process? `tools/helperhostprobe.c`, sent to elfldr one question per send
(`tools/run-helperhostprobe.py`; websrv passes the send's `args` as `argv[0]`):

| Run | Question | Answer |
| --- | --- | --- |
| `helperhost-01` | who it is; the runtime under `/data`; memory the kernel reports | root (euid 0); reads `ntdll.prx` (a SELF, 899,519 bytes); **no direct memory**; 8,159 MiB of flexible memory available |
| `helperhost-03` | anonymous memory, 64 MiB steps to 1 GiB, each touched | 1,024 MiB mapped, touched and released |
| `helperhost-04` | 256 MiB of direct memory | refused, `0x80020023` |
| `helperhost-05` | The GPU broker's host ranges at their fixed addresses | 512 MiB at `0x240000000` and 16 GiB at `0x3000000000` reserved and released |
| `helperhost-06` | The runtime's `ntdll.prx`, as the helper loader loads it | loaded (handle `0x22`, start result 0) and unloaded |

The console answered after every send. A helper needs none of the direct memory such a process lacks: the title's
heap takes direct memory, a helper's stays on flexible memory, and no helper log records the direct-memory heap.
So the process side fits. What is not known yet, in the order it would be settled:

1. **A channel between the title and such a process** for the wineserver's socket and the GPU broker's: Sony hands
   a local process its descriptor; this one needs a Unix-domain socket both can reach, with `SCM_RIGHTS`.
   **Settled, see below.**
2. **Starting one from the title**: the title sends the payload to the console's own payload launcher, as this tool
   does from the PC. **Settled, see below.**
3. **A loader built as a payload**: `wine-child.bin` is a title executable; the same loading code built with the
   payload SDK, without the elevation a title needs.
4. **Lifetime**: Sony ends a title's local processes with the title, and a payload's are nobody's. Such a helper
   would have to end itself when its control socket closes (today a helper that loses its parent waits for Sony),
   and be ended through that socket.
5. **How many** such processes the console runs, and whether a title suspended from the home screen leaves them
   running.

This changes a rule kept so far: helpers come only from Sony's service, with no spawning payload of our own. It
depends on the jailbreak's resident loader, as the title already depends on the Lapy daemon for `/data`.

**The channel works** (`tools/run-channel-test.py`). The payload listens on a Unix-domain socket; the isolated native
title (`native/proton_channel_probe.cpp`, run and then replaced by production through `tools/run-child-probe.py`)
connects and sends one end of a socket pair, as the wineserver's socket is, and an anonymous shared-memory object,
as a frame page is; the payload answers through the first, reads the second, and sends a socket end of its own
back, through which the title answers. `/data` cannot hold a socket: `bind` there is `EOPNOTSUPP` (`channel-01`),
so the payload binds under `/system_tmp`, which can, and writes the path into a plain file under `/data`, which the
title reads; the title reaches `/system_tmp` through the root the Lapy daemon gave it.

| Run | Result |
| --- | --- |
| `channel-01` | `bind` under `/data` refused, errno 45 (`EOPNOTSUPP`); nothing exchanged |
| `channel-02` | bound at `/system_tmp/ps5proton-helper.sock`; the title connected on its first try and sent two descriptors; the payload answered through the socket end, mapped the shared page and found its pattern, and sent a socket end back; the title's reply came through it. Both halves PASS, no fault records, production restored |

**The title starts one itself** (`tools/run-payload-start-test.py`). The isolated native title's `payload-start` mode
reads the payload from `/data/ps5proton/run/`, posts it to websrv's payload launcher on `127.0.0.1:8080` (the
channel step as its argument, the launcher not streaming its output back), and does the channel exchange above
with the process that starts. `payload-start-01`: the launcher answered `HTTP/1.1 200 OK` 190 ms after the request;
the title connected to the new process's socket on its first try; both descriptors there, a socket end back and the
reply through it, the whole exchange held **616 ms after the request**. No fault records, production restored, the
payload file removed. For comparison, a helper Sony starts was ready about 800 ms after its start (`limit-01`).
The payload's own lines do not reach klog when the launcher does not stream them; its verdict came to the title
over the channel.

### Capacity of such a process: count, lifetime and file budget (2026-01-05)

The three items that decide whether a payload-started process is usable as an overflow tier beyond the seven,
measured with `tools/payloadhost.c` and `tools/run-payloadhost.py` (one step per send, firmware 10.01):

| Question | Measurement |
| --- | --- |
| How many run at once | **32 of 32 alive at once** (8/8 and 16/16 first): the console refused none, so the seven-helper count does not apply to them |
| Does one outlive its starter | **No orphans: 8/8, 16/16 and 32/32 ended themselves** when the stream that started them closed |
| Its own open-file budget | **636 descriptors** opened before `EMFILE` (errno 24), where the title's helpers share one title-wide budget |

Method: every instance is a payload sent to websrv's launcher with `pipe=1`, so the sender holds the stream. It
prints a heartbeat every 2 s, writes `/data/payloadhost-<pid>.alive` on the way in, and on the first failed write
to its stream writes `/data/payloadhost-<pid>.gone` and exits. `SIGPIPE` is ignored, so a closed stream is an
error the process can act on rather than a silent kill, and the host reads the markers over FTP — which is what
makes "ended with its starter" distinguishable from "orphaned". The count is the number of instances whose
stream was still open after a settle; their exit markers were checked after the streams were closed.

So open item 4 is answered: **stream-close is a working lifetime mechanism**, and a payload-born helper need not
outlive the title that started it. What remains of item 5 is whether a title suspended from the home screen
leaves these processes running, and whether a count far above 32 is refused rather than served.

### Suspension and teardown: nothing else reaps them (2026-01-05)

Item 5's second half, with four held instances and the production title taken through its own lifecycle (one
launch, `suspension-01`):

| Stage | What the four payload processes did |
| --- | --- |
| title launched, launcher mode | 4/4 alive, heartbeats advancing |
| title suspended from the home screen | 4/4 alive, heartbeats **still advancing** (17/16/16/15 → 41/40/40/39) |
| title resumed | 4/4 alive (→ 52/52/51/51) |
| title closed (`procs count=0`) | **4/4 alive** (→ 75/75/74/74): the teardown did not touch them |
| their own streams closed | **4/4 wrote a gone marker and ended** |

A suspended title neither freezes nor reaps a payload-started process, and closing the title does not end one
either. So **lifetime is ours to implement, not something to rely on** — stream-close is the mechanism, and it
worked again here under a title's own lifecycle. The same test says what such a process does while a title is
parked: it keeps running and keeps its memory, which a design has to account for rather than assume away.

Caveat: in this arrangement the streams were held by the host, not by the title, so the *title-started* variant
(`payload-start` mode, which the regular title does not have) is untested; the console-level behaviour above is
what that variant would inherit.

### The loader itself in a payload process (2026-01-05)

Item 3, first half. `native/proton_payload_child.c` is the Wine helper loader built for the payload path
(`tools/build-payload-loader.sh`: the same sources as `tools/build-wine-child.py` — `src/pw_wine_start.c`,
`wine/ps5/pw_wine_prx.c`, `wine/ps5/pw_wine_process_io.c` — linked as a plain payload ELF instead of converted
into a SONY SELF, so there is no preload mask to ask for). Its `selftest` step, sent through websrv's launcher:

```
payload-child pid=4511 ppid=99 uid=1 euid=0
loader: runtime=/data/ps5proton/runtime/ge11-7/lib/wine/x86_64-unix prefix=/data/ps5proton/prefixes/probe
loader: environment -> 0
loader: load -> 0 stage=6 status=0 module=34 descriptor=800541550 wine_main=8004971b0
loader: ntdll.prx loaded by a payload process
```

`stage=6` is `PW_WINE_START_ADOPT`: the loader loaded `ntdll.prx`, parsed its module record, found the PRX
descriptor and resolved `wine_main` — in a process Sony never started, with no elevation step and no preload
mask. One thing that had to be right and was wrong first: the loader is told the **Unix** directory
(`.../lib/wine/x86_64-unix`), not the runtime root, so `sceKernelLoadStartModule` gets a path that exists;
with the root it returns `0x80020002`, which reads like a refusal and is only a missing file.

At that stage the bootstrap transport was still missing. A real helper receives `struct pw_child_bootstrap` and its
descriptors on fd 3 from Sony's local-process service; a payload-born loader has to be handed them by the
title over the channel (the connection is dup2'd onto fd 3 so the rest of the Wine sequence is unchanged),
and that spawner lives in `wine/ps5/pw_wine_process.c`.

**The transport works (2026-01-05).** Two sends of that binary, no title change at all. The `channel`
step bound `/system_tmp/ps5proton-payload-child.sock`, wrote the path into
`/data/ps5proton/run/helper-channel.path`, accepted the connection and received the whole
`struct pw_child_bootstrap` (14,352 bytes) with one descriptor; it moved the wineserver connection onto
fd 3 (`dup2(wineserver=10, 3) -> 3`), adopted `ntdll.prx` (`stage=6`, descriptor, `wine_main`), resolved
`__wine_ps5_set_output_sink`, `pw_cwd_set` and `pw_ps5_child_start`, and answered `PW_CHILD_READY` on
fd 3. The `spawner` step - the title's half, played by the same binary - read it back:
`reply -> 24 bytes protocol=0x50573506 kind=1` PASS.

Two things the run taught, recorded because each cost a round: one `recvmsg` returned 8,168 of the
14,352 bytes (a stream socket has no message boundaries, so the receive loops until the struct is
whole), and the READY reply arrives on the wineserver end - fd 3 - not on the channel, which is where
the production title reads it.

That historical probe did not call `pw_ps5_child_start(3)` or enter Wine, because the peer in that
test is a socket pair, not a wineserver. That needs the title's real spawn path.


### Production bootstrap through a payload rendezvous (2026-10-06)

The old transport-only prototype placed the wineserver socket on descriptor 3.
That was not production's contract: **descriptor 3 is the packet control socket**;
the wineserver connection is a separate descriptor in its bootstrap message.
The prototype's successful READY exchange alone did not establish Wine startup.

`native/proton_payload_child.c` now receives the owner's existing SOCK_SEQPACKET
endpoint through a unique AF_UNIX stream rendezvous, places that endpoint on 3,
and enters the same loader as `native/proton_wine_child.c`. A native-PID greeting
lets the owner track cleanup even if bootstrap fails. The stream carries only
that greeting and a one-byte SCM_RIGHTS transfer; the full bootstrap stays one
packet, with wineserver and optional GPU descriptors. Input arrives in a separate
packet. The loader calls `pw_ps5_child_start(3)` and runs the real `wine_main`.

The owning title posts the plain ELF to the resident websrv/elfldr service.
The experiment initially required `WINE_PS5_PAYLOAD_CHILDREN=1`; after its local
merge, this backend became the production default. No title-side rfork or raw
system call is introduced. `WINE_PS5_PAYLOAD_CHILDREN=0` selects the legacy Sony
service route for regression tests.
The payload entry skips the helper's elevation request, uses /data logs, watches
control EOF without consuming agent messages, and returns through the payload
SDK's process termination. The Sony title entry still never returns.

Payload memory has two additional requirements. Anonymous executable Wine
mappings use the existing payload SDK's mmap/mprotect via explicit callbacks;
ordinary mappings keep Wine's FD/protection wrappers. Fixed reservations report
success without enrolling flexible RAM in the title's direct-memory mapper.
After loading each signed PRX, a payload-only hook inspects its module segments
and gives its own executable text **read + execute** permission through the SDK,
then touches one byte per 16 KiB text page. Sony reported those segments as
execute-only (4). Reusing that reported value did not suffice; reading the text
before adding read permission produced SYSTEM_XO_VIOLATION. The owning title's
loader and permissions remain on the ordinary path. Runtime PRXs still contain
no raw syscall instructions; the SDK capability stays in the payload ELF.

Normal completion and forced Windows termination shut down the control socket
and wait for native PID absence (`kill(pid,0)` returning ESRCH). GPU close receives
a negative native PID for this backend instead of a Sony service ID, and retains
backing unless the process is absent and GPU fences have drained. Process, GPU
client and frame-source tables now each hold 32 entries. **32 Windows children
have not been proved**; the Windows capacity proof below is eight simultaneous
children, plus their root and startup helper.

| Console run | Result |
| --- | --- |
| `payload-pe64-basic-01` | Preflight refused before deployment: installed ntdll differed from the old signed restoration backup. |
| `payload-pe64-basic-02` | PE64 arguments/environment/handle/pipe/exit-code checks passed; native closure waited out its 10-second timeout. Payload logs showed return only after the owner closed its last endpoint. This run is recorded as FAIL. |
| `payload-pe64-basic-03` | PASS: same Windows checks; 3/3 native payload PIDs absent, normal close status 0, capture 3.8 s. |
| `payload-eight-01` | PASS: eight PE64 children simultaneously ready and STILL_ACTIVE, eight expected 0x600d exits; all 10 native payload PIDs absent. |
| `payload-descendants-01` | PASS: four three-generation rounds, 12 descendant node verdicts, 14/14 native payload PIDs absent, slot reuse and inherited Windows metadata/handles. |
| `payload-owner-close-01` | PASS: the owner replaced itself while eight children waited on events; root, startup helper and eight children all absent afterward (10/10). This was a headless owner-close test. |
| `payload-graphics-01` | Preflight refused before title deployment: earlier synthetic broker readbacks occupied the capture paths. They were preserved. |
| `payload-graphics-02` | FAIL: both graphics children reached their metadata checks but failed a 7.2 MiB Wine anonymous mapping; no GPU submission, 4/4 payloads absent, balanced broker releases. |
| `payload-graphics-03`, `-04` | FAIL: the initial flexible-memory adapter rejected Wine's fixed reservation at SharedUserData; no GPU submission. Corrected the reservation success contract. |
| `payload-graphics-05`, `-06`, `-07` | FAIL: DXVK reached Vulkan queries/device creation, but native-code accesses faulted. Keeping Sony's execute-only value, through either protection API, did not fix it. All four payloads absent afterward in each run. |
| `payload-graphics-08` | FAIL: reading native text before granting read permission produced SYSTEM_XO_VIOLATION in the root payload. Capture stopped at the fatal signal; restoration initially deferred while the owner restarted, then exact signed production and prior files were restored once idle. No kernel panic. |
| `payload-graphics-09` | Windows/GPU checks PASS: two renderers plus two rendering descendants, 96 submissions, 26 display flips, all expected display/readback samples and input checks passed; 6/6 payloads absent. Overall runner still recorded FAIL because its successful owner LoadExec was classified as `exited` instead of `finished`. The final runner accepts `exited` only with the explicit owner PASS and clean restart lines. Original evidence is retained. |
| `payload-pe64-basic-04` | PASS on the final RX loader: Windows arguments/environment/handle/pipe/exit-code checks, 3/3 native PIDs absent, 5.5 s capture. |
| `payload-eight-02` | PASS on the final loader: eight simultaneous Windows children, eight expected exits, 10/10 native PIDs absent, 12.0 s. |
| `payload-descendants-02` | PASS on the final loader: 12 descendant nodes across four three-generation rounds, 14/14 native PIDs absent, 8.5 s. |
| `payload-owner-close-02` | PASS on the final loader: owner replacement with eight waiting children, 10/10 native PIDs absent, 11.8 s. |
| `payload-graphics-10` | PASS end-to-end: two DXVK renderers and their two rendering descendants; eight frames and 24 submissions each, 96 submissions total, 27 owner display flips. All 16 display captures and four child readbacks matched expected pixels; all four received the injected XInput buttons/axis. 6/6 native PIDs absent, zero transport errors, no GPU wedge/fault, balanced host ranges (80/80, 20/20), 19.5 s. |
| `payload-sony-regression-01` | PASS with payload opt-in absent: existing Sony route, Windows process semantics and 3/3 native child deletions, 6.5 s. |
| `payload-restored-launcher-01`, `-02` | PASS: the restored signed production launcher launches and exits normally. |

The EOF fix is needed because peer shutdown can signal readable EOF without
POLLHUP; the main thread now uses a non-consuming MSG_PEEK check as well. The
native process absence probe runs as a short payload because Sony's local-process
list and process-delete events do not enumerate these helpers. No fault records
were observed in the passing headless runs. The runner now restores the exact signed production modules/title and original
synthetic files. An older helper restoration path incorrectly retained FTP's
decoded ELF; it was repaired and the exact signed helper restored before
`payload-graphics-02`. FTP-decoded helper ELF data is never uploaded as a
restoration SELF: the runner requires the matching signed local helper and audits
its executable segments.

Every final run restored the exact signed production title/runtime/helper and
prior synthetic files, and had zero post-launch fatal/GPU fault records. Four
host tests covering descriptor transfer, PRX parsing, bootstrap and the GPU
contract passed with ASan/UBSan. The runtime import audit reports no unresolved
imports, title-only bindings or raw syscall instructions. Rendering peaked at
91,095,040 shared bytes and 28,573,696 device-local bytes in this synthetic workload.

Builds stay isolated from the experiment's shared, read-only .deps symlink:

```sh
bash tools/build-payload-loader.sh
python3 tools/build-payload-runtime.py --base .deps/wine-gpu-broker --output build/payload-runtime
python3 tools/build-wine-child.py --gpu-broker --work-root build/isolated-loaders
python3 tools/build-child-probe.py --kind wine-owner --wine-gpu-broker --work-root build/isolated-owners
bash tools/build-graphics-process-probe.sh build/probes/graphics-render-probe.exe
```

`tools/run-process-probe.py` adds `--payload`, `--probe`, `--graphics-probe` and
`--production-backup`. The backup directory must contain verified signed
ntdll/wineserver/win32u/libvulkan PRXs with their ELF companions, and the signed
wine-child.bin with wine-child.elf. Use the reported owner/loader build directories,
`--runtime-build build/payload-runtime`, and an unused run name. The ordered cases
are basic, payload-eight, descendants-repeat, payload-owner-close, graphics-broker.
The last additionally takes --gpu-broker and the pinned --gpu-client directory;
--gpu-local and the existing child-input pad script validate those descriptors.
Existing synthetic captures are backed up under ignored build/, temporarily
removed, then restored byte-for-byte. Evidence is in evidence/; raw logs and
synthetic images remain under ignored klog/.

These runs prove the process and DXVK graphics route on this firmware/setup, not the game launcher launching games,
32 simultaneous Windows clients, payload suspension, child audio, OpenGL children
or VKD3D children. Payloads do not inherit Sony title suspension; coordinating
that remains separate work.

### Production payload default, 2026-10-06

The verified experiment was committed as `7561901` and merged locally into main
as `dc96d53`. `tools/build-production.sh` now builds the payload loader and the
updated process probe; staging and deployment install the verified plain ELF at
`/data/ps5proton/run/proton-payload-child.elf`. All executable names use the owner
and GPU broker by default. Sony spawning and title/single layouts are diagnostics.
The runner collects the payload logs named by this owner's PID records and rejects
an explicit owner failure even when the native title returns cleanly.

| Production run | Result |
| --- | --- |
| `payload-production-basic-01` | FAIL: an older project-owned process probe had been staged without rebuilding it. Child creation/exit ran, but the old handle-balance check failed. The old runner also missed the child verdict. Retained as a failure. |
| `payload-production-basic-02` | PASS: rebuilt Windows arguments/environment/handles/pipe/exit checks, default payload route without a backend override, 3/3 native payload PIDs absent. |
| `payload-production-eight-01` | PASS: eight simultaneously ready/live Windows children and expected exits, 10/10 native payload PIDs absent. |
| `payload-production-d3d11-01` | PASS: 180 frames through DXVK and the production payload broker, 1280x720 readback and 1920x1080 display. Both center RGB (127,64,64) and background (6,13,26); 2/2 native payload PIDs absent. |

These runs used the canonical production build and deployed artifacts, not an
isolated owner or runtime swap. They do not establish FNA game or game launcher operation.

### Shared memory and OpenGL qualification, 2026-10-06

The SDK executable-memory grant edits entire VM entries. Granting RX to a PE
text range could therefore remove write permission from the image's data/TLS
range. The Wine adapter now first asks ordinary `mprotect` to protect the
requested host-page range without EXEC, then uses the SDK to add EXEC. Wine's
existing 4 KiB Windows / 16 KiB host-page protection union remains in place.
This is a common memory-adapter change, independent of executable names.

| Run | What was observed |
| --- | --- |
| run payload-production-01 (2026-10-06), `-02` | FAIL: write fault in `ntdll` TLS-slot allocation during initial game launcher load; title returned, 3/3 payload PIDs absent in each run. |
| `zink-payload-wgl-01` | FAIL before the fix: write fault storing the probe's global window handle. Diagnostic Mesa DLLs restored exactly. |
| `zink-payload-wgl-02` | PASS after the fix: OpenGL vertex/shader/transform-feedback probe, core contexts 3.2, 3.3 and 4.6; renderer Zink Vulkan 1.4 on PS5 RADV, OpenGL 4.6 Mesa 26.2.0. Clean owner restart and exact probe-prefix DLL restoration. Swapchain initialization still failed, so this proves offscreen work only. Native PID absence was not separately queried. |
| run payload-production-03 (2026-10-06) | FAIL/incomplete cleanup: initial TLS failure no longer observed; repeated `chrome_elf.dll` write AV at a metadata page. 34 payload rendezvous records. The title requested server termination after its 10-second close timeout, then FTP and control commands stopped replying. Open ports alone do not prove healthy services. No verified return, native cleanup, panic or shutdown. |
| `compatibility-probes-host-01` | Host-only PASS for 32 synthetic code executions and 480 adjacent-page writes/queries; three ASan/UBSan host tests passed. WGL presentation probe could not create a host window without an available display. |

`tools/build-mesa-windows-zink.sh` builds pinned upstream PE64 Mesa WGL/Zink
without fetching inputs. Windows OpenGL calls go through Zink to Wine Vulkan
and the existing GPU broker. The probe runner temporarily swaps only the
project-owned probe prefix and restores it, recording restoration failures.
No game installation is edited. Mesa's Windows path requested IMMEDIATE
unconditionally; `mesa-zink-windows-present-mode.patch` retains it when advertised
and otherwise selects mandatory FIFO. The revised DLLs build, but have not yet
been run on the console. The broker's added rejection diagnostic also builds
through the canonical PS5 Vulkan recipe and is not yet deployed.

Next, after the console restarts:

1. Confirm service replies and `procs count=0`; record a fresh boot only after
   an actual restart. Clear stale one-shot requests before launching.
2. Run `vm-protect-probe.exe`, then basic/eight-child and DXVK regression checks
   against the currently deployed protection fix.
3. Deploy the built broker diagnostic and test patched Mesa with
   `run-mesa-zink-probe.py --exe gl-present-probe.exe`. Its 60 swaps and 120
   back-buffer checks must be supplemented with owner-display capture pixels.
   Repeat context teardown and renderer-thread exit tests before global staging.
4. Use a short game launcher browser startup run with `WINEDEBUG=+virtual,+module,+seh`
   to correlate the remaining metadata-page write fault with allocation and
   protection calls. Verify termination and every native PID afterward; full
   Game launcher acceptance requires the client UI and launching/exiting a library game.
5. Test both FNA game backends when a legitimate x64-capable installation is
   available. run installation-audit-01 (2026-10-06) records why the supplied copies
   cannot be executed by this PE64-only runtime.

The console retains the protection-fix production runtime; exact pre-fix signed
artifacts are in ignored `build/production-before-split/`. The probe prefix was
restored after both Zink runs. The third game launcher run's one-shot request cleanup
was not verified. Do not reuse a FTP-decoded ELF as a signed SELF backup.

#### Sparse reservations and decommit follow-up

Local review found that the payload adapter returned success from
`__wine_ps5_dmem_region` without converting the fresh Sony reservation into an
anonymous mapping. Patch 0790 already documents that ordinary protection changes
cannot commit a bare Sony reservation. The current candidate instead establishes
an anonymous `PROT_NONE` mapping over each fresh payload reservation. Later
commits can then fault in flexible pages through the normal protection path.
This is built and staged, **not console-qualified or deployed**. Large reservation
acceptance and actual flexible-memory accounting still need console proof. It
is a candidate explanation for the browser write fault, not a confirmed diagnosis.

The extended `vm-protect-probe.exe` commits only 4 KiB inside a reserved 2 MiB
range, keeps a live neighboring page, decommits/recommits 32 times and checks zero
contents. It also decommits across complete and partial host pages, checking both
zeroed contents and preserved neighbors. Isolated host Wine with 16 KiB page
bookkeeping reproduced stale bytes on the second sparse commit. Patch 0936 zeros
only the committed Windows pages in partial host-page boundaries, discards full
host pages as before, and restores the resulting protection union. Preparation
failures attempt to restore protections before any contents are cleared.

The final probe passes on both 4 KiB and 16 KiB host bookkeeping with patch 0936:
32 code executions, 480 adjacent writes/queries, 32 sparse zero cycles and the
spanning decommit check. `tools/build-wine-host-page-probe.py` reuses immutable
pinned host objects in separate ignored build trees. Its sysconf shim coordinates
the Wine client and server. **Linux kernel pages remain 4 KiB**; this is a policy
simulation, not a PS5 or 16 KiB-kernel test. Early simulation attempts lacked NLS
resources or coordinated shared-data sizing and did not reach the probe.

The installed game launcher profile also contains existing diagnostic arguments that
disable its CEF sandbox and bootstrap/update checks. Runs 01–03 used that profile;
they cannot establish normal game launcher acceptance. The generic one-shot argument
contract now distinguishes an absent argument override from an explicitly empty
one. After deploying the new title, `--profile 9 --args ''` clears those arguments
for a test without editing the installation. Full acceptance must use ordinary
the game launcher behavior, including its sandbox and update path. No bypass flags are added.

The next production deployment needs both the rebuilt title and runtime. First
run the final memory probe and payload regressions; then qualify WGL display
pixels, normal game launcher startup, termination and native cleanup. The latest built
artifacts do not change the last deployed runtime while console services are down.


#### Mesa WGL/Zink presentation and the first game, 2026-10-06 (afternoon)

Since `2586b55` every game runs in a payload helper, which cannot use the console's OpenGL: the .NET game (MonoGame
DesktopGL) ended at `MonoGame.OpenGL.GL.LoadExtensions` with `NoSuitableGraphicsDeviceException` (completion
`0xe0434352`) and the title closed its loading screen after 0.1 s for the GPU broker. The pinned Mesa WGL/Zink build
(`f7ce91ac4c36`, opengl32 `2304968d...`, libgallium_wgl `22b133fc...`) was run in the probe prefix:

| Run | What was observed |
| --- | --- |
| `zink-present-2026-10-06-01`..`-04` | Mesa's `opengl32.dll` and `libgallium_wgl.dll` loaded native in the helper; `renderer=zink Vulkan 1.4(PS5 (MESA_RADV))`, OpenGL 4.6 compatibility; a 632x326 window. `glReadPixels` of the window's back buffer returned 0,0,0 from frame 0 with no GL error; an off-screen renderbuffer cleared and read back exactly (204,102,51), so image readback works and the window's swapchain image is what reads back empty. `MESA_LOG=windbg` routes Mesa's log to `OutputDebugString`; it logged nothing (corrected later: a helper did not inherit `MESA_LOG`, [below](#zinks-descriptor-buffer-in-a-helper-2026-10-06-evening)). |
| `zink-present-2026-10-06-05` | The probe kept presenting through a readback mismatch: 60 swaps, 60 broker flips at 632x326, and the owner display's captures (`gpu-child-1-0..3`) show the expected picture: center 204,102,51, background 25,51,76. **Visible OpenGL presentation through Zink and the broker works.** Reading the window back before or between presents does not (60/60 mismatches); games do not normally read their window. |
| run zink-01 (2026-10-06), `-02` | With Mesa copied into prefix 1 and selected, the .NET game got past device creation; about 2 s after Zink loaded, a **GPU protection fault** in the owner: `SQC Data` read of an unmapped page, VA `0x141aff0f1000`, the same address in both runs. The title ended (fatal signal); the console stayed up. `-02` asked for `ZINK_DESCRIPTORS=lazy` (descriptor sets, not descriptor buffers) and had the same fault; this was read as "descriptor buffers are not the cause", which was wrong: a helper did not inherit the variable, so both runs used Zink's descriptor buffer ([below](#zinks-descriptor-buffer-in-a-helper-2026-10-06-evening)). The address is outside the 32-bit window (`address32_hi` 2) and the host ranges. No frame was presented. |
| run default-01 (2026-10-06) | With Zink opt-in only: the earlier clean failure, `0xe0434352`, back to the launcher, no fault. |

The runtime now ships the build in `lib/wine/mesa/x86_64-windows` (`stage-wine.py`, checked against the build
manifest). In the launcher layout the title copies it into the prefix's `system32` (`pw_wine_launch_sync`: version
stamp and sizes; a new prefix has no `system32` before wineboot, so its first launch keeps Wine's) and selects it with
`opengl32=n,b` and `GALLIUM_DRIVER=zink` **only when `PROTON_ZINK=1`** (game.json or a test), until a game passes;
otherwise it pins Wine's `opengl32=b`. Next: find what owns `0x141aff0f1000` in the owner's GPU VM (the broker's
device-local mappings are placed by the kernel and not logged), and reproduce with a probe that draws textured quads
from vertex buffers with uniforms, as MonoGame's SpriteBatch does.

The 2026-10-06 [foundation extraction](GRAPHICS_PROCESS_BROKER.md#foundation-extraction-2026-10-06)
moves the shared broker and optional WGL build into PS5_Vulkan and the Mesa
fixes into PS5_Mesa. Proton now consumes their pinned local commits. Historical
patch paths above describe the runs before this move; current source ownership
is in ARCHITECTURE.md. All outstanding console acceptance boundaries remain.

#### Zink's descriptor buffer in a helper, 2026-10-06 (evening)

`tests/gl-sprite-probe.c` asks of OpenGL what MonoGame's SpriteBatch asks, one feature at a time: each step draws a
quad into a 64x64 off-screen target, waits (`glFinish`) and reads the centre back; then 60 window frames of a textured
quad are presented (`tools/run-mesa-zink-probe.py --exe gl-sprite-probe.exe`). It prints the Mesa settings it received.

| Run | What was observed |
| --- | --- |
| `zink-sprite-title-2026-10-06-01` | Title layout (Zink on the title's own RADV, no broker): every off-screen draw right, 60 window frames. |
| `zink-sprite-2026-10-06-02`..`-10` | In a helper: the draw coloured by a vertex attribute was right (204,102,51); every draw that reaches a resource through a descriptor read 0,0,0 with no GL error: a texture with no uniforms, a uniform `vec4` array, a uniform block (dynamic and static), a storage buffer (dynamic and static); 7 of 8 wrong from `-05` on. The 60 window frames presented. |
| `zink-sprite-2026-10-06-09` | Asked for `GALLIUM_THREAD=0` and the probe's cache eviction: the probe received neither. **A helper inherits only an allowlist of its spawner's environment** (`inherited()` in `wine/ps5/pw_wine_process.c`), plus `WINEDEBUG`, `WINEDLLOVERRIDES` and `WINEDLLPATH`. So a test's `ZINK_DESCRIPTORS=lazy` (`-03`, run zink-02 (2026-10-06)), `MESA_DEBUG` and `MESA_LOG` (`zink-present-2026-10-06-02`, `-04`) and the title's own `GALLIUM_DRIVER=zink` never reached the helper. The allowlist now passes `GALLIUM_`, `ZINK_` and `MESA_LOG`, but not the rest of `MESA_` (`MESA_DEBUG` included): the title's shader-cache folder is a native path for its own RADV. The eviction is now the probe's `evict` argument. |
| `zink-sprite-2026-10-06-10` | `GALLIUM_THREAD=0` received (no driver thread), and 64 MiB of unrelated memory written on the recording thread between recording each draw and `glFinish`: the same 7 of 8 wrong. The CPU's caches are not the cause, as `graphics-child-gpu-coherence-*` had shown for the GPU's vector reads. |
| `zink-sprite-2026-10-06-11` | `ZINK_DESCRIPTORS=lazy` received: **PASS**, 8 of 8 draws right, 60 window frames. |
| `zink-sprite-2026-10-06-12` | Zink's default descriptor buffer, with `PROTON_GPU_LOCAL=0` (no device-local memory for the helper): **PASS**, 8 of 8. |
| `zink-sprite-2026-10-06-13` | `PROTON_ZINK=1` alone, the title's new default `ZINK_DESCRIPTORS=lazy`: **PASS**, 8 of 8. The owner's captures of the window (`gpu-child-1-0`, `-3`) show the background 25,51,76 on 1,555,083 pixels and the textured quad 51,153,204 on 960x540. |
| run zink-03 (2026-10-06) | The .NET game with `PROTON_ZINK=1`: Mesa's DLLs loaded native, no fault. 5,188 frames presented and 10,425 submissions, no fence drain, no wedge; local peak 219.7 MB, host peak 49.0 MB. The owner's captures show the developer's splash at frame 0, the intro's mountain scene at frame 600 and the title screen at frames 1200..4800 (first recorded as "the title screen at frames 600..4800"). After the 90 s budget the game closed on the close request and the title returned to the launcher. The runner's verdict is FAIL only because the owner reports the game's exit status 1 (`PS5_PROTON_OWNER_BOOTSTRAP FAIL initial_status=1`). The helper had no audio: `mmdevapi` found no `ps5` driver. |

The cause, read from the sources and confirmed by `-11` and `-12`. With `PROTON_GPU_LOCAL=1` a helper's RADV is a
discrete GPU whose device-local memory the helper cannot map (the broker overlay reports no CPU-accessible VRAM), so
it has no device-local, host-visible memory type. Zink creates its descriptor buffer with `PIPE_USAGE_DEFAULT`, which
asks for that type (`zink_resource.c`); with none, Zink's heap map falls back to the device-local types
(`zink_screen.c`). Zink maps the buffer persistently, and for memory that is not host-visible `zink_buffer_map`
returns a staging buffer, which a persistent map never copies back. Zink wrote its descriptors into the staging copy,
and the GPU read the untouched device-local buffer: zeros in the probe. In the .NET game, memory nothing wrote also
fits the protection fault at one unmapped address in two runs, but that was not proven separately. Descriptor sets live
in RADV's own pools, which are host-visible.

The title now sets `ZINK_DESCRIPTORS=lazy` beside `GALLIUM_DRIVER=zink` whenever it selects Mesa's OpenGL for a
helper; a game.json or a test can override it. Zink stays opt-in (`PROTON_ZINK=1`).

The fix that keeps Zink's descriptor buffer is PS5_Mesa `b3588f78dd7` ("zink: keep persistently mapped buffers in
host-visible memory"). Zink's descriptor buffers are created as persistently mapped
(`PIPE_RESOURCE_FLAG_MAP_PERSISTENT`). In `allocate_bo`, a persistent buffer whose heap is not really host-visible
moves to the host-visible heap. A device that has a host-visible device-local type keeps its placement. It was run
before it was pinned, as a candidate built from the same tree (`c0db59f`, made with `git stash create`; tree
`7b79cbed`) in a scratch folder, through `tools/run-mesa-zink-probe.py --mesa-work`:

| Run | What was observed |
| --- | --- |
| `zink-sprite-db-fix-2026-10-06-01` | A helper with device-local memory, `ZINK_DESCRIPTORS=db` received: **PASS**, 8 of 8 draws, 60 window frames. The same mode had 7 of 8 wrong before the fix. |
| `zink-sprite-title-db-fix-2026-10-06-01` | The title layout (the title's own RADV, which has host-visible device-local types), `ZINK_DESCRIPTORS=db`: **PASS**, 8 of 8, unchanged. |
| run zink-db-fix-01 (2026-10-06) | The .NET game with `PROTON_ZINK=1` and `ZINK_DESCRIPTORS=db`. The candidate was placed in the console's runtime with its own version stamp, so the title copied it into the prefix. Mesa's DLLs loaded native, and there was no fault. 5,212 frames and 10,473 submissions, no fence drain, no wedge; host peak 50.9 MB (49.0 MB with descriptor sets); the 32-bit host window's high water was 5.46 MB (3.77 MB): the descriptor buffers in host-visible memory. The captures show the splash and the title screen. It closed on request and returned to the launcher. The runner's verdict is FAIL only for the exit status 1, as before. |

Putting the pinned build back afterwards: `deploy-wine.py` did not restore the runtime's Mesa files, because it
compares the staged runtime with its own `deploy-manifest.json` on the console, not with the files there. A check of
their hashes found the candidate still in place. The three files (`opengl32.dll`, `libgallium_wgl.dll`, `version`)
were uploaded again from `.deps/staged` and read back identical.

The WGL build's Mesa revision is pinned in PS5_Vulkan's `tools/build-mesa-windows-zink.sh` (`f7ce91ac4c36`), which
this repository takes at the foundation revision `tools/setup-graphics-foundation.sh` pins. Until both pins move to
`b3588f78dd7`, the runtime ships the build without the fix, and the title's `ZINK_DESCRIPTORS=lazy` stays.

Both pins then moved. PS5_Vulkan `5b5e4fc` builds the WGL DLLs from `b3588f78dd7` (the broker client's overlays stay at
`f7ce91ac4c36`), and `tools/setup-graphics-foundation.sh` takes that foundation. The runtime ships opengl32
`deb4da5a...` and libgallium_wgl `ee2421a7...` before stripping, and the title no longer sets `ZINK_DESCRIPTORS`, so
Zink uses its descriptor buffer by default again:

| Run | What was observed |
| --- | --- |
| `zink-sprite-default-2026-10-06-01` | `PROTON_ZINK=1` alone; the probe received no `ZINK_DESCRIPTORS`, and the title copied the runtime's Mesa into the probe prefix: **PASS**, 8 of 8 draws, 60 window frames. |
| run zink-default-01 (2026-10-06) | The .NET game with `PROTON_ZINK=1` alone. The title copied the new Mesa into prefix 1, the DLLs loaded native, and there was no fault. 5,175 frames and 10,399 submissions, no fence drain, no wedge; host peak 50.9 MB, and the 32-bit host window's high water 5.46 MB, the same as with `ZINK_DESCRIPTORS=db` and not the 3.77 MB of descriptor sets. The captures show the splash at frame 0, the intro's mountain scene at frame 600 and the title screen at frames 1200..4800 (first recorded as "the title screen at frames 600..4800"). It closed on request and returned to the launcher; the runner's verdict is FAIL only for the exit status 1. |

Zink in a helper stays opt-in (`PROTON_ZINK=1`). No .NET game run went past the title screen: loading a save or
starting a new game is not yet tested.

#### Playing the .NET game in a helper: input does not arrive, 2026-10-06 (evening)

run zink-load-01 (2026-10-06) loaded a test save with a pad script, on a save cycle
([GAME_LIFECYCLE.md](GAME_LIFECYCLE.md)), with `PROTON_ZINK=1`. Every scripted press
was applied by the title (cross 25 s ... The left stick 70..83 s, stop 100 s, all logged), and the game closed on the
stop. The owner's captures, every 300 frames, show the splash, the intro and then the title menu, unchanged from frame
1200 to 4800: no press reached the game. The .NET game's `game.json` sets `SDL_XINPUT_ENABLED=0`, so SDL looks for
the DualSense through raw HID and Windows.Gaming.Input (the helper loaded `hid.dll`; `RoGetActivationFactory` for
`Windows.Gaming.Input.RawGameController` is a stub). A helper gets XInput from the owner's input page and no raw HID
reports, the gap recorded in [GRAPHICS_PROCESS_BROKER.md](GRAPHICS_PROCESS_BROKER.md). The test save was unchanged,
and `restore` put the 6 held save folders back, 12 files identical.

After this run the control payload stopped answering on 9111: it accepts a connection and replies to nothing. Its
log's last line is a `procs` command at 16:48 (console time), the request cleanup timed out, and
`tools/console_guard.py` recorded the console as dropped. The console still answered ping, FTP and `websrv` on 8080.
The runner reported `runner_error`. No launch has been made since.

Two runner changes came out of these runs. `tools/run-console.py` no longer counts the owner's
`PS5_PROTON_OWNER_BOOTSTRAP FAIL` against a game that the title asked to close and that closed by itself, provided the
broker stopped: the .NET game's own exit status is 1. A probe's status stays its verdict, and a game that had to be
ended still fails. Checked against the logs: the four .NET game runs above would pass, `zink-sprite-2026-10-06-09`
still fails and `zink-sprite-default-2026-10-06-01` still passes. The evidence now records `initial_status` and
`closed_on_request`. `tools/deploy-wine.py` re-uploads a file whose size on the console differs from the staged one,
even when its deploy manifest says the file is current, and says how many were changed on the console. It reads one
listing per folder, and its parser was checked on the host against the console's listing format. It has not run
against the console yet: it asks the control payload whether the console is idle first.

#### The title stuck in its own exit after LoadExec, 2026-10-06 (evening)

The control payload's hang after run zink-load-01 (2026-10-06) was not the payload's own. After the game closed, the
title saved the registry and asked to restart into the launcher. The run's last klog lines are
`restart: to=launcher cycle=1 reason=wine-ended`, `[SceLncService] sceApplicationExitSpawn3(0x0000c018)`,
`[Syscore App] Kill for LoadExec(0xdc)` and `Terminating pid=220`.

An hour later, a read-only payload sent through `websrv` (`tools/console-processes.py`) listed every process with
the event it waited on. It walks the kernel's process records at their own size, 1096 bytes, not the SDK's 1088:

| Process | State |
| --- | --- |
| `eboot.bin` (pid 220, the title, started 16:46:53) | asleep on `exithold`: in its exit, held there |
| `coredump.elf` (pid 225, started 16:49:36) | A zombie |
| `SceSysCore.elf` (pid 55, parent of ShellCore and ShellUI) | stopped; klog had shown `mDBG: Sending signal(pid: 55 ...)` |
| `SceShellCore` (pid 58) | its main thread waiting on an IPC reply (`IpmRcv0`) |
| The control payload, in its `procs` command since 16:48 (pid 114) | waiting on an IPC reply (`IpmRcv0`) |
| A second control payload started on port 9112 (pid 226) | waiting in `sceUserServiceInitialize` (`IpmWSv0`) |

`websrv`, `elfldr`, `ftpsrv` and `klogsrv` were waiting on their sockets as usual, and the console answered ping, FTP
and `websrv`. The title's exit went no further, and the system services that the control payload calls waited with
it. The title and SysCore were not touched; the console needs a restart. The three earlier .NET game runs that
day, and every probe run, left through the same path.

#### The queue after the restart, 2026-10-06 (night)

The console was restarted, the hardened control payload (PS5_Vulkan `50b16f8`) loaded, and the title folder and the
runtime deployed together ([CONSOLE_QUEUE.md](CONSOLE_QUEUE.md)). Each of these runs ended through the title's own
paths and returned to the launcher. None of them hung in its exit:

| Run | Result |
| --- | --- |
| `queue-wine-probe-2026-10-06-01` | PASS in 10.3 s |
| `queue-d3d11-probe-2026-10-06-01` | PASS in 10.7 s |
| `queue-zink-sprite-2026-10-06-01` | PASS in 12.1 s: the five kinds of draws read back, 60 textured window frames |
| run title-01 (2026-10-06) | passed: 5,199 frames presented through the broker, closed on request |
| run input-01 (2026-10-06) | passed: scripted cross opened New (loading at frame 1,500, the character screen from 1,800) |
| run menu-hid-01 (2026-10-06) | passed: scripted right, right, left, left moved the selection Co-op, Exit, Co-op, Load |

In the title run the broker copied each 1920x1080 frame in 5.61 to 5.71 ms (`copy_us`, sampled every 600 flips), a
third of a 60 Hz frame (the stutter seen at the console is not yet measured against it). The pad script waited for
the broker's first flip, 13.57 s after the start (menu-hid), and the broker took every capture it asked for.

**A real pad's presses did not reach the game, twice.** In run title-01 (2026-10-06) and
run sound-03 (2026-10-06) the person at the console pressed the D-pad, cross and circle on the title menu.
They saw no reaction. The title logged every press, and it publishes them in the same report as scripted ones. In -03
the frames from 2,400 to 9,600 all show the menu with Load selected. Later runs, with a `+hid` trace, did not reproduce
it:

* In run pad-04 (2026-10-06) the person pressed the PS button first, so the system took the pad and winebus
  removed the device and created it again 1.07 s later, as had happened in menu-hid. hidclass then delivered 85
  separate presses. With them the person loaded the test save and walked on the farm (the run's pictures).
* In run pad-05 (2026-10-06) nobody touched the pad. The device the game opened at its start was the only
  one: it was created once and never removed. The game read its reports at 74 a second from the first frame on, and
  the selection followed all five scripted presses, Co-op, Exit, Co-op, Load, Co-op.

So neither the reconnect nor the device the game finds at its start explains the two runs without a reaction. Those
runs have no `+hid` trace. Not understood.

**Pages a payload cannot read back.** With helper audio on, run sound-01 (2026-10-06) and `-02` ended 17
and 19 s after the launch. The game's helper died first. klog shows `self_pager_activate: sceSblAuthMgrAuthHeader
returned unexpected error` and two `pager error`s in it. In -02 they fell in win32u's text (`+0xc000`) and in
ntdll's at `+0x3c550`, the code that saves a signal context, so the fault could not even be reported. Those pages
had been touched at load, so the kernel had evicted them since (its `VM Stats` showed `swap out` grow from 68 to 334
during -03), and it could not authenticate the fake-signed modules again to read them back. The payload loader touched every text page once at load, which is no help after an
eviction. It now wires every segment of each module it loads (`mlock`, after raising `RLIMIT_MEMLOCK` to its
maximum; native/proton_payload_child.c).

**A dead helper ended the title.** After the helper died, the title died of SIGPIPE, writing to the helper's socket.
wineserver's `main()` and every Wine client ignore SIGPIPE, and in the launcher layout the title runs neither, so the
owner now ignores it before it starts the server (native/proton_wine.cpp).

run sound-03 (2026-10-06), with both fixes and helper audio on: every module of both helpers was wired, 4 of
4 segments each, with no error and no limit. The game's helper wired 34.4 MB in 12 modules, 24.3 MB of it RADV
(`libvulkan-helper.prx`) and 3.6 MB ntdll. The title logged `SIGPIPE ignored status=0`. No `pager error` appeared in
the 204 s run. The game played its music for 190 s ([GRAPHICS_PROCESS_BROKER.md](GRAPHICS_PROCESS_BROKER.md)),
closed on request and returned to the launcher. The test save was untouched and all 12 held files came back
identical.

Then the console refused to start the title (2026-10-06; record removed from the tree; in git history): every launch path returned
`0x80940033`, and ShellCore logged `LNC_CHECK::0x80940033 (m_is_pkg value is false)`. `/system_ex/app/PPSA99170`
was an empty folder. ShadowMountPlus, which mounts the title folder there at each launch (`[LINK] nullfs mounted` in
its log for every launch that worked), logged nothing for these attempts. Nothing was deployed between the run that
launched and these attempts. The console is left for its owner to restart.

The console's owner restarted it and moved to ShadowMountPlus 1.7beta4, which mounts `/data` into a title's sandbox.
The title then launched, but run pad-02 (2026-10-06) ended after 4 s with `wineserver:
/data/ps5proton/prefixes/1 is not owned by you`. The title took a visible `/data` as already granted, so it never asked
the Lapy daemon and ran as its own user. The prefix belongs to the elevated title, which the daemon makes root. File
descriptors could not be parked either (`replacing a descriptor with a socket does not work here`). The title now asks
unless it is root. A request counts only once the daemon has made it root, because the client's proof, a write and read
in `/data`, passes at once with the loader's mount (ps5/src/proton_launch.cpp). In
run pad-03 (2026-10-06), the first launch after the restart, the title started the daemon. Its one request
after that went unanswered, so that process went on without `/data` and returned to the launcher. The launcher's
request, to the same daemon, then made it root. The daemon is now asked up to three times after it is started. That
path has not run on the console since. In -04 and -05, with the daemon running, the title logged `elevation: ok`, Wine
accepted the prefix and descriptors were parked again (63).

#### Closing the title from the system menu, 2026-10-06 (late night)

Closing the title from the PS menu while a game ran in a helper panicked the console. ShellCore's `BlockingKill`
ends the title, its helpers live on as payload processes, and the kernel then deletes the title's budget
(`deleting bigapp ... container 1`). Memory the title had made that a helper still mapped was still charged to the
title, and the console went down after the warning:

- run freeze-base-02 (2026-10-06) and run pad-queuefix-03 (2026-10-06):
  `VM resource leak: set:1, res:0, amount:0x8000`. That is two 16 KiB pages, the size of the input page and a
  helper's audio ring, which the title made and sent to each helper.
- After helpers made those two themselves (0b93fe4), run panicfix-02 (2026-10-06): `FMEM leak is detected
  in cnt 1` for many pages of one object, and a panic.

What else the title made for its helpers:
- The in-process server's anonymous sections (patch 0730 backs them with `SHM_ANON` objects made in the title), its
  session memory, which every client maps and which grows, and its user shared data page.
- The GPU broker's host-visible buffers, which it made with `shm_open` and sent to each client.

**The fix: helpers own every page they share with the title.**
- The title starts a **memory helper** first, a payload process that runs no Windows program
  (`src/pw_memory_helper.{h,c}`, `PW_CHILD_MEMORY`). It answers two requests on its control socket. `alloc` makes an
  object. `touch` grows one if needed and writes through every page of a range, so the pages are the helper's
  before the title writes them.
- The server takes every anonymous section from it (patch 0939). It has the helper write through the session memory
  as it is set and whenever it grows, and the user shared data page.
- The broker takes each client buffer from it and has the pages written through before the GPU grant (PS5_Vulkan
  `4d7e053`, `ps5_gpu_broker_set_shared_memory`).
- Without a helper both fall back to making their own, and the log says which.

run memhelper-01 (2026-10-06), the first launch after a boot, with helper sound on:
- The memory helper answered 112 requests, and left when the title was gone.
- The person at the console closed the title from the PS menu after 43 s. The kernel logged `deleting bigapp ...
  container 1` and `budget_appid_clear(18, 4)=3`, with no leak warning.
- The console went on to the home screen. It did not panic.

The same run was the first launch after a boot with the new elevation (cf0cc4e). The process that started the
daemon restarted itself once, and the new process logged `elevation: ok`.

#### The controller in a helper: no program found it, 2026-10-06 (night)

On the controller-input branch, patch 0940 counts the DualSense's input at each stage: winebus, hidclass, dinput and
xinput.

run padpath-01 (2026-10-06):
- winebus handed on 5120 reports, 698 of them with a press.
- A hidclass read was already waiting for 5117 of them, and the longest unread queue was 3. Nothing piled up.
- None of hidclass's or dinput's lines came back. Their modules had not reached the console: the build's list of
  Windows modules to take from the patched tree was kept by hand, and neither was on it. The list is now derived
  from the trees (`tools/wine-patched-modules.py`, d764f71).

run padpath-02 (2026-10-06). The person at the console saw no controller input. In the game's process:
- hidclass got every report: 4096, 616 with a press.
- For the first 3072 reports, about 28 s, no file was open on the device. No program in the game had opened the
  controller.
- SDL had loaded `hid.dll`, dinput8 and windows.gaming.input at 3439, about 1 s after start, and none of them opened
  the device. The .NET game turns SDL's XInput off, and xinput was never asked about controller 0.
- The first file opened at 3469, after the system menu had taken the pad and the bus had made the device again. That
  file never sent a read, so it was most likely Wine's raw-input list, which holds a file open. A second file opened
  4 s later.
- The start-up wait had said `DualSense ready after 34 ms`.

**Why, from Wine's code.** These steps are read from the code and fit every count above. A run with the fix tests
them.
- Every Windows program runs its own HID stack in its own process (patch 0870), and all of them use the title's one
  wineserver.
- wineboot ran just before the game, and its bus made a DualSense too. A device interface's symbolic link is a
  permanent server object, so wineboot's link outlived wineboot.
- When the game's hidclass enabled its interface, `IoCreateSymbolicLink` (`OBJ_OPENIF`) opened the old link instead
  of making one, and returned `STATUS_OBJECT_NAME_EXISTS`.
- `IoSetDeviceInterfaceState` took that as a failure, deleted the interface's `Linked` value and left it disabled.
  Every program that lists present HID devices (SDL, dinput, Windows.Gaming.Input) then skipped it.
- The wait looked in the registry, where wineboot's interface was still listed, so it saw a controller that the game
  could not open.
- Nothing looked again: device notifications need the plug-and-play service, which the PS5 does not run (`failed to
  open RPC handle, error 1722`).
- Whether wineboot's bus made its DualSense before wineboot ended decided whether the game had a controller. That
  is the failure that came and went from run to run.

**The fix, patch 0941:**
- ntoskrnl replaces a link left behind when it enables an interface, and logs `replacing a link left behind`.
- The start-up wait waits until this process has enabled the DualSense's interface.
- The PS5 bus makes the DualSense at its start and keeps it until it stops. A pad the title loses (the system menu,
  or the pad turned off) reads as released until it is back.
- Wine's own programs in system32, wineboot among them, start no HID bus.

The first run with it stopped at launch with the known `0x80940033`, which a reboot clears.
run ownpad-02 (2026-10-06), the first launch after that reboot:
- wineboot started no bus.
- The game logged `DualSense ready after 26 ms`, now meaning its own interface was enabled.
- 1.2 s after the game started, programs looking for controllers opened the device: four files opened and closed
  again, and one stayed open.
- DirectInput acquired it 10 s later and read every report: 1793 reads, 1792 reports delivered, none left unread. It
  parsed 2048 reports, 78 with a button down and 316 that changed the state.
- The person at the console: "Everything works now!"
- Closing the title from the system menu after 42 s left no leak warning, and the kernel deleted the title's budget.

Not exercised in that run: no link was left behind, so nothing was replaced, and the menu-and-back round trip was
not tried.


## Manual launch defaults, 2026-10-06

The automated .NET game runs had supplied `PROTON_ZINK=1` and
`WINE_PS5_HELPER_AUDIO=1`. A normal launch supplied neither: Wine's built-in
OpenGL failed at MonoGame graphics-device creation, and helper audio was off.
The loading screen also closed at GPU broker startup, before any game frame.

The owner/broker layout now defaults to Mesa WGL/Zink and the existing helper
PCM ring/mixer path. Explicit zero overrides remain available for diagnostics.
The title hands its loading display off when the native presenter opens
VideoOut; Wine PRX presenters retain their existing release_display callback.
The broker initializes the GPU while the loading screen remains visible.
No game-name condition, profile edit or Wine patch was added.

| Run | Console result |
| --- | --- |
| run manual-default-01 (2026-10-06) | Graphics default only: title-screen pixels correct, loading closed at presentation after 13.8 s and memory released. Audio was still disabled. The runner reported `exited` before a delayed launcher status-0 line reached its drain; its original failing result is preserved in evidence. |
| `manual-default-d3d11-2026-10-06-01` | D3D11 PASS; loading handed off after 3.5 s. |
| `manual-default-audio-2026-10-06-02` | No graphics/audio overrides; capture at frame 600 showed the correct title screen. Loading handed off after 16.2 s, all three VideoOut cleanup calls returned 0 and memory was released. Non-silent stereo at 48 kHz logged; the person at the console confirmed music audible. Game closed on request (its exit status was 1), broker stopped, launcher ended with status 0. Native helper PIDs 207, 208 and 209 were absent afterward. Runner PASS, 104.7 s total. |
| `manual-default-audio-d3d11-2026-10-06-02` | Same deployed build, no overrides: D3D11 PASS, launcher status 0, 10.8 s total. |

The final signed title SHA-256 is
`94b0ffcba37453a9e3da5e3dd6c359124e751d096f6c0174ad9fdc5648fb672f`.
Four focused host tests passed with ASan/UBSan: audio mixer, audio ring,
Wine launch staging and Wine audio sink. The runner now records the drained
launcher end status and accepts an otherwise normal `exited` result when that
captured status is 0; crashes, timeouts, forced game closure and failed broker
cleanup remain failures.

The automated tests selected the installed profile through a one-shot request,
using production graphics/audio defaults. After the request files were removed
and the console was idle, the person launched Proton normally and selected
the .NET game through its menu: loading screen, game menu and music all worked.
A fresh Wine prefix
still needs wineboot before its system32 exists for Mesa staging. Neither
fresh-prefix first launch nor FNA game or game launcher acceptance is established here.

## The console's system memory (2026-10-07)

Before the application allocator below, every helper was a payload process whose anonymous memory drew from what
the console's system services share. The payload loader starts each one outside any app's budget (it sets budget 0
before `execve`, "Helpers outside the count" above). That budget does not prevent explicit application-pool
allocations through `sceKernelAllocateDirectMemoryForApp`.
On 2026-10-07 the game launcher's helpers filled that memory, and the console swapped until it stopped answering and had to be
restarted (2026-10-07; record removed from the tree; in git history).

### What can be read

While an app runs, SceShellCore writes what every system process holds to klog every 10 to 30 s (none at idle): an
`FMEM` line per process with two pairs of figures, and a `VM Stats: RSS a b` line whose `a` and `b` sum each pair's
first figure. Our helpers have figures only in the first pair, as do the other payloads (elfldr, websrv, kstuff,
ftpsrv, klogsrv, ShadowMountPlus) and the system daemons; ShellUI's processes hold theirs mostly in the second.

`tools/console-memory.c`, a payload that only reads, found SceShellCore's pair for any process:
`sceKernelGetResidentFmemCount(pid, &resident, &mapped)`, in 16 KiB pages (websrv: 7.5 and 20.9 MiB, its FMEM line's
`7.5/ 20.9`). Both pointers are needed: without the second the call refuses with `0x8002000e` (EFAULT) and writes
neither. The first probe called it with one and was answered only because its third register happened to hold a
writable address; the guard's first census, built the same way, counted nothing (`gate-2026-10-07-07`), and a probe
that tried each shape settled it (`console-memory-2026-10-07-06`, `-07`). The resident figure matches SceShellCore's
process by process to 0.1 MiB (SceShellCore 91.4 against 91.5, elfldr 21.2 against 21.2, ShadowMountPlus 23.7
against 23.7, orbis_audiod 20.1 against 20.1), and its sum over every process, 248.0 MiB, the 244.5 of
`VM Stats: RSS` (`console-memory-2026-10-07-03`, against the report in `console-memory-2026-10-07-01-klog.log`).
`sceKernelGetResidentCount` takes the same three arguments and gives the whole resident set. Nothing else is there
to read: the kernel answers `ENOENT` for every `vm.stats.vm.*` counter, `vm.kmem_size` and `hw.physmem`, its name
tree cannot be walked, and only `vm.swap_total` (10.8 GB) and `vm.swap_reserved` are left (`console-memory-2026-10-07-05`).
The kernel library's code cannot be read to see which system call a function makes either: its text is execute-only,
and reading `sceKernelGetResidentCount`'s first bytes ended the probe (`console-memory-2026-10-07-01`).

### Where it gives out

| Run | System memory (the census), MiB | Swapped out | Outcome |
| --- | --- | --- | --- |
| idle, 2026-10-07 | 244 to 248 | none | |
| gate cases, `gate-2026-10-07-06` | at most 562 (`launcher-render`), 424 (`payload-eight`), 334 (`launcher-desktop`) | none | |
| The .NET game, the 2026-10-06 queue runs | about 500, 852 to 857 while it loads, then 800 to 825 | about 140 MB more | it plays |
| The game launcher, run fix-01 (2026-10-07) | 706, 915, then 865 to 970 for about three minutes | 59 MB, then up to 1,115 MB | The console stopped answering |

So the console starts swapping at about 850 MiB. A game that passes through is fine; one that stays above swaps the
console to a stop. The game launcher's helpers held 298.5, 244.7, 108.1, 83.9 and 72.3 MiB at the end, 808 MiB between them.

### The guard

The memory helper (the helper the title starts first, `src/pw_memory_helper.h`) takes that census every half second:
it lists every process (`kern.proc`), asks each one's figure, and writes the sum, the title left out, with its own
share and its largest helper's, into a page the title maps read-only. The title's loop reads the page and never
waits on the helper, so a console that has started swapping cannot hold up the pad or the way back to the launcher.
`src/pw_memory_guard.h` decides from the samples' own times: a census that has stood at the limit (870 MiB) for 20
of the last 30 s asks the game to close as a user would (the same request as a stop, so a game that does not close
is ended 10 s later), and one that stands 70 MiB past it for 3 s ends the game at once through the server. Each is
said once, in klog, with the figures. The .NET game's run never reaches either; the game launcher's would have been asked to
close about 20 s after it first passed 900 MiB, minutes before the console stopped. Every 10 s the alive line reports
the census, its peak, and the seconds at the limit. `PROTON_SYSTEM_MEMORY_LIMIT` (MiB, 0 for none) and
`PROTON_SYSTEM_MEMORY_END` move the two marks.

### On the console

`gate-2026-10-07-08` passed all seven gate cases on build `40b645436713929d`, the guard on in every one. The census
read 161 to 184 MiB at the start of each case and followed SceShellCore's figure as the helpers grew. In the
`memory-guard` case (the limit lowered to 450 MiB, the end mark out of reach) a window in a helper took 304 MiB: the
census went from 161 to 543 MiB (SceShellCore's `VM Stats` said 539.7 at the same time), the alive lines counted 8,
then 18 s at the limit, and at 20 s the title said "the console's system memory stood at 450 MiB or more for 20 of the
last 30 s (now 543 MiB; helpers 388 MiB in 3, the largest 381 MiB in process 371): asking the game to close before the
console swaps". The window received the close while it held its 304 MiB, ended with status 0, and the title returned
to the launcher. `gate-2026-10-07-07`, the same guard with the census call missing its second pointer, read 0 MiB in 0
processes throughout and failed that case, which is how the call's shape was found; its `launcher-render` failure was
the process probe counting 55 handles before two rendering children and 53 after, two of Wine's own handles that
closed meanwhile (the four gate runs before counted 53 and 53); the probe now counts once the figure holds still.

## Application memory in payload helpers (2026-10-07)

The payload loader supplies Wine with `sceKernelAllocateDirectMemoryForApp`, a native `libkernel_sys` API found in
the user-supplied 10.01 libraries. It takes the application's direct-memory pool explicitly, unlike
`sceKernelAllocateMainDirectMemory`, which selects the caller's partition. Helpers retain budget 0; no budget
assignment, kernel data patch or change to the title's imports is required. The static module hash and API addresses
are in `evidence/payload-application-memory-static-10.01.json`; no firmware is included.

The console reports 11,712 MiB (11.4375 GiB) of application direct memory. That pool is shared by the title, graphics
and every helper, not granted afresh to each process. Native PRX code, the payload's bootstrap and shared transport
can still consume system memory, so its existing guard remains enabled.

The callback reuses Wine's existing direct-memory mapper: Windows reservations stay virtual until committed,
committed pages are zeroed, and decommit/release returns backing. Wine's Unix heap uses that same pool. The payload
SDK still supplies executable protection, including the split needed to preserve adjacent writable pages. All
helpers and descendants adopt this route by default. `WINE_PS5_PAYLOAD_DMEM=0` selects the previous flexible-memory
route for diagnostics; the gate's system-memory-guard test uses it deliberately to exercise that guard.

| Run | Observed outcome |
| --- | --- |
| `payload-direct-2g-2026-10-07-01` | The ordinary allocator refused 2 GiB in a budget-0 payload (`0x80020023`). |
| `payload-direct-forapp-2g-2026-10-07-01` | PASS: the application allocator mapped 2 GiB; first/last bytes of every 16 KiB page were written and verified, then unmap/release succeeded. System residency stayed at 3,504 KiB. |
| `payload-direct-forapp-owner-end-2026-10-07-01` | PASS: one page remained valid after the original owning title PID disappeared, and was explicitly released. This checks an owner-process transition, not an arbitrary late release after PS-menu closure. |
| `payload-forapp-vm-2026-10-07-01` | FAIL before Windows startup: the loader's new callback was missing from the PRX descriptor. The matching export was added; the helper exited without a console fault. |
| `payload-forapp-vm-2026-10-07-02` | PASS: 32 code executions, 480 adjacent writes/queries, 32 sparse zero cycles, spanning decommit preserving neighbours and a 16 GiB virtual pool committed in 64 KiB pieces. |
| `gate-payload-forapp-2026-10-07-01-payload-memory` | PASS: two Windows payload processes held 2 GiB each simultaneously, verified every 16 KiB page, decommitted/recommitted a page with zero contents and intact neighbours, released their allocations and repeated the cycle. Root and child logged 2,075 MiB peak direct backing, 14 MiB system residency and zero allocator failures. |
| `gate-payload-forapp-2026-10-07-01` | Eight of nine cases passed; the guard case failed because its diagnostic opt-out was missing from the inherited native environment. Its 304 MiB went into application memory, leaving the system census at 202 MiB. The setting was added to helper inheritance; this gate did not authorize a commit. |
| `payload-forapp-guard-2026-10-07-02-memory-guard` | PASS after that correction: system residency rose to 537 MiB, the 450 MiB diagnostic limit held for 20 seconds, the title requested closure and the Windows program exited with status 0. |
| `gate-payload-forapp-2026-10-07-02` | PASS: all nine mandatory cases on production build `cf48858aa8d43bcd`, including the capacity test, executable protections, eight children, descendants, graphics, TLS, desktop input and the diagnostic guard. |
| `payload-forapp-shell-close-2026-10-07-01-validation` | PASS: shell closure with a Windows graphics helper holding 2 GiB (2,101 MiB total direct backing, zero failures). Both live native helpers and the already-ended bootstrap helper were absent afterward (3/3 recorded PIDs), control reported idle and no CPU/GPU fault was logged. The underlying program-run evidence has no ordinary completion verdict because the shell deliberately interrupted it. |
| run forapp-01 (2026-10-07) | PASS for the bounded lifecycle: normal command line, 120-second budget, status 0 on the close request and return to launcher. System memory peaked at 721 MiB without triggering its 870 MiB guard. The human reports unusual UI on both console and host Wine; full game launcher operation and visual correctness remain unproved. |
| run forapp-01 (2026-10-07) | PASS for the bounded default OpenGL run: loading screen handed over after 15.8 s; the passive captures show a rendered world. Helper direct backing peaked at 1,024 MiB with zero mapper failures; system memory peaked at 246 MiB. It closed on request and returned to the launcher (game status 1, the same normal close status as earlier runs). |

The game launcher used up to 2,714 MiB of the application pool, including the owner and graphics. The logged swap-out counter
was 62.9 MiB at the start and 62.7 MiB at the end; this run did not add the earlier sustained swap growth. One browser
helper logged a `wbemprox.dll` `.text` protection warning and one mapper operation failure, without a physical
allocation refusal. That warning needs isolation before claiming complete Windows memory semantics or the game launcher
acceptance. The memory summaries retain it (2026-10-07; record removed from the tree; in git history), rather than treating every
mapper operation as successful.

The bounded synthetic owners used by the native probes returned status 1 when their hold expired; their fixture
evidence is retained separately from each native payload verdict. These are memory and process proofs, not a claim
that game launcher or every game is compatible.
