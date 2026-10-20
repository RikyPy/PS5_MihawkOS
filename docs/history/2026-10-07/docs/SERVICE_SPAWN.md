> Historical snapshot of `docs/SERVICE_SPAWN.md`, archived on 2026-10-07 from `9b07ebb`.
> Defaults, next steps, counts and failure classifications below belong to their original runs.
> Use the [current document](../../../SERVICE_SPAWN.md) for present guidance; see the [archive index](../README.md).

# Sony budget and local-process lifecycle

## Finding and limits of the evidence

**Sony's local-process service created a working headless homebrew helper three times**
on 2026-10-04: `service-native-07`, `-08` and `-09`. Each fresh executable completed an
identity/native-PID socket handshake, sent its completion message, and was closed
through the service with its registry entry removed and the parent's descriptors
balanced. No new spawning payload was used. Follow-up native lifecycle and
experimental headless Windows process results are in
[WINDOWS_PROCESSES.md](WINDOWS_PROCESSES.md): four native 1 MiB allocation-reuse
cycles, application closure with a live helper, and focused shared-wineserver
PE64 semantics and owning-title-brokered descendant chains passed. A subsequent
experimental Windows graphics child rendered in the original title's GPU context;
it does not establish GPU initialization in the Sony-managed helper or all-budget
leak freedom. See the graphics section of WINDOWS_PROCESSES.md.

The route is `sceSystemServiceAddLocalProcess`, associated with the existing
application. Sony owns the native spawn/budget setup; the title does not clone
its Wine threads or import privileged budget operations. These observations
apply to the current elevated-title/kstuff setup and the pinned local firmware
inputs, not all firmware or arbitrary executable permissions.

The direct title `rfork_thread(0x14, ...)` panic remains unexplained, and that
probe remains disabled. See [CHILD_PROCESSES.md](CHILD_PROCESSES.md).

Seven exact file hashes, 26 call edges and selected exports/absences are in
[`service-spawn-static-10.01.json`](../../../../evidence/service-spawn-static-10.01.json).
Recheck the local inputs without contacting the console:

```sh
python3 tools/verify-service-spawn.py --system /path/to/10.01/system
```

The initial result was 47 static checks; the expanded checker now passes
**55 checks**, including export library namespaces and Sysmodule startup call edges. It
verifies input hashes, exports and selected direct calls through their import relocations. It does not
prove the manually traced argument layouts or complete dispatch graph. Firmware
bytes and disassembly are not stored in this repository. RVAs below belong only
to these pinned local files; they are not runtime function pointers.

## Title-facing service contract

In `common/lib/libSceSystemService.sprx`, the public entry is at `0x54c0`.
Register use establishes this provisional PS5 call shape:

```text
int AddLocalProcess(int app_id, const char *path,
                    const char *const argv[], const void *options);
```

This is a reconstructed ABI, not a SDK declaration to import without validation.
The public PS4 [OpenOrbis declaration](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/blob/master/include/orbis/SystemService.h)
uses an integer third argument and an argument-array fourth argument. **That
declaration does not match these PS5 instructions.** There is no safe reason to
reuse it unchanged.

The wrapper builds a 72-byte (`0x48`) internal options object. It swaps the two
32-bit input fields at offsets `4` and `8`, copies the pair at `0xc`, copies
`0x18` and `0x20`, copies a bounded 32-byte string at `0x24`, and copies `0x44`.
Its internal defaults begin `(72, 1, -1, -1)`. The public input's size field is
not checked by this wrapper; those defaults are not a validated public initializer.

The descriptor fields can be followed further:

| Public input offset | Static interpretation | Basis |
| --- | --- | --- |
| `0x4` | source descriptor | Field swap, then ShellCore's `socket_info.fd` assignment |
| `0x8` | crash-report setting | Swapped into internal `0x4`, then copied to `process_param.enableCrashReport` |
| `0x18` | preload PRX flags | ShellCore's field use and diagnostic label |

The native experiment maps the public layout to the exact internal defaults
built by the JVM wrapper: 72-byte zero-initialized input, source FD at `0x4`,
crash reporting `1` at `0x8`, and `-1` at public `0xc` (internal `0x10`). The
internal source PID at `0xc` stays the wrapper default until the service supplies
it. The remaining copied fields initially use zero; the working headless request
overrides public `0x18` with `0x8000000000000002`. This establishes the
request initializer; it does not establish permission or image acceptance.
Other option meanings and executable-type branches in ShellCore remain incomplete.

**Correction after the deeper 2026-10-04 trace:** public offset `0x8` is not a
source PID. The source PID is internal offset `0xc`, initially `-1`. ShellCore's
request handler at `0x6311d0` obtains the IPC caller's PID and overwrites that
field at `0x6313ef`, before queuing the copied options. The service controls
descriptor ownership; callers do not supply an arbitrary source PID. The handler
also checks the caller's app association against the requested app ID and requires
a separate privilege check for another application.

The inner function `0x7de0` rejects a null pathname or options pointer. It
serializes a null-terminated argv array into at most **4096 bytes**, counting
every string's terminating NUL. A null argv is allowed at this layer. It needs
an initialized launch client; otherwise it returns `0x80940001`. The serializer
at `0xdb00` sends selector `0x30004` with the app ID, pathname, serialized argv,
and 72-byte options. The completion query at `0xdbc0` uses `0x30005`. A successful
result is a service local-process identifier; keep it distinct from a native PID.

Additional entries:

- `AddLocalProcessForJvm` (`0x1590`) gets the current app ID from app status,
  accepts only `/app0/` or `/hostapp/` path prefixes, and reads the same source
  descriptor/crash-report pair from offsets `4`/`8` of its third argument.
  Both JVM functions export from **`libSceSystemService_jvm`**, while the pinned
  SDK stub groups them under `libSceSystemService`. In `service-native-01`, both
  imported addresses were zero; the test stopped before spawning and restored
  production with no fault. This is an import-library mismatch, not a service
  denial. The revised test uses the ordinary public APIs and the equivalent
  initializer. No Wine Java support follows from the wrapper's name.
- `GetLocalProcessStatusList` (`0x5690`) takes an output array, capacity and count
  pointer. Records are 36 bytes: a 32-bit identifier and a 32-byte string. The
  inner function rejects capacities above 16. The identifier/name fields alone
  are not an exit code or proof of a native PID.
- `KillLocalProcess` (`0x55d0`) forwards **two integer arguments** to the service,
  unlike the one-argument PS4 stub. The JVM wrapper at `0x1720` obtains the current
  app ID, then passes `(app_id, local_process_id)` to the same inner function.
  Cleanup and identifier-to-native-PID correspondence still need runtime proof.
- `GetParentSocket` (`0x57a0` -> `0x64b0` -> `0x8ca0`) simply returns **3**. It
  does not create or validate a socket. A child must verify that descriptor 3
  is actually a live connection before treating it as one.
- `GetAppId` is not a no-argument current-app getter: its implementation checks
  a supplied string of length 9. Use the separately traced app-status query to
  investigate current application association.

## Service, budget and execution ownership

ShellCore's reviewed function at `0x5cf4b0` looks up the application, resolves
the executable, checks its process type, builds a `0xc40` process-parameter
block, and passes the source PID/FD pair to `sceApplicationAddProcess2` at
call site `0x5d0608`. It records the returned application-local process ID in
its application object. The `libSceSysCore` wrapper serializes the request using
selector `0xaa110003`. The two IPC hops are distinct; do not call the privileged
SysCore layer directly from the title.

SysCore's budget paths show:

1. Resource limits come from `sceKernelGetResourceLimit`, selected by native
   process profile. The lookup at `0x2fff0` reads 24-byte resource records.
2. Budget creation at `0xc627` supplies ten entries, resource IDs
   `1,2,4,10,5,6,7,12,13,14`. Another path at `0x31ca4` supplies nine, omitting 14.
   Resource names and the numeric limits are not yet established. A budget is
   not merely an unlimited-memory switch.
3. The generic spawn at `0x2e62` and application-process spawn at `0x17e88` use
   `rfork_thread` flags **`0x1030`** (`RFPROC | RFMEM | RFCFDG`): a fresh descriptor
   table and a temporary shared address space until exec. Both prepare a 16 KiB
   stack. This is Sony's service context, not a recipe for retrying in our title.
4. The generic callback `0x33c0` waits for a parent semaphore, clears inherited
   budget association, applies root/app-mount and app-info state, duplicates
   explicitly supplied descriptors into `3+i`, installs the target budget, then
   `execve`s. The parent registers its kqueue event before releasing the child.
5. The separate application callback `0x18de0` sets up sandbox/root paths and
   standard descriptors, calls `sceKernelRdup(source_pid, source_fd)` at `0x18eab`,
   assigns a supplied budget at `0x18fa6` when present, and synchronizes exec with
   a semaphore. Do not conflate this callback with the generic descriptor-list
   callback. Their full upstream dispatch relationship is still being traced.
6. Failure paths unwind mounts, store errors and close the failed native child.
   Budget destruction is explicit (`0xcbdd`, `0x3243e`), with profile/state
   cleanup. Those observations do not yet establish normal-child reaping or
   parent-title shutdown behavior for our executable.

Ordinary `libkernel` exports `rfork_thread` and `execve` but not the reviewed
Create/Get/Set/DeleteBudget or Spawn APIs. Elevating the title's UID does not
prove that budget, application and descriptor ownership are correctly prepared.

## Minimal route to Wine children

Use Sony to create a fresh native executable, then attach that process to the
**parent's existing in-process wineserver**. A second independent wineserver
would break shared handles and Windows parent/child relationships.

The source PID/FD handoff and child descriptor 3 are a plausible bridge to
Wine's existing `WINESERVERSOCKET` initialization path. This is an architectural
inference; socket type, lifetime, authentication and protocol must be proved.
The native parent may be SysCore, so the title must not assume POSIX `waitpid`
on the local-process identifier. Wine owns Windows process state; Sony owns
the native process lifecycle. Exit status needs a verified protocol and native
cleanup observation, including failure before Wine initialization.

Validation order:

1. A separate, one-shot **service preflight**: check imports, app status and
   local-process list with durable logs; exercise a client-side invalid request
   that cannot spawn. Resolve the remaining options and kill ABI first.
2. Launch one original headless native helper from the installed application:
   identity/PID handshake over the transferred socket, bounded lifetime, normal
   completion, then check service state, descriptors and budget cleanup. Prove
   a failed executable request also cleans up. Preserve the production backup.
3. Repeat the native lifecycle and prove parent-title closure handles the helper.
   A public [PS4 daemon experiment](https://github.com/LightningMods/PS4-daemon-writeup/blob/main/writeup.md)
   reports parent-linked local processes; that is precedent, not PS5 evidence.
4. Connect one PE64 child to the existing server and test inherited handles,
   process events, exit status and termination. Resolve PS5 adapters that assume
   `pid == getpid()` before widening the Wine process guards.
5. Extend to real launcher helpers; validate graphics ownership separately.

If the properly formed title request is denied, a title-scoped broker should
request this same service lifecycle and return only owned handles/status. A
payload cloning itself and injecting Wine is not established as equivalent to
a mounted, authenticated application process. Keep failures explicit and retain
the current Wine restrictions until the native and PE64 proofs exist.

## Console service preflight (2026-10-04)

[`service-preflight-01.json`](../../../../evidence/service-preflight-01.json) records a
successful one-shot run before Wine or graphics initialization:

- All four ordinary title imports resolved: app status, local-process list,
  AddLocalProcess and GetParentSocket.
- Public app status returned zero and application ID 24600. The 16-byte output
  guard stayed intact. The process-list query returned zero with **one entry**
  and intact buffer guard. This count includes something present before any
  child was created; it cannot be treated as a count of our helpers.
- Null options and 4097 serialized argv bytes each returned `0x80940005`, as the
  local client validation predicts. No valid spawn request was submitted.
- GetParentSocket returned 3; this did not validate a socket at that descriptor.
- Open-descriptor counts were **12 before and 12 after**. The parent closed
  through its shell path, the console became idle, and zero fatal-signal/GPU-fault
  records appeared. The signed production launcher was restored and its
  executable segments matched the local production ELF.

The probe has a separate OFF-by-default build flag. Its build excludes the
panicking direct-child code. The existing isolated builder/runner accept
`--kind service-preflight`; the direct mode's panic refusal remains active. The
runner checks the requested kind against the build manifest and preserves the
signed production backup. Recovery understands all isolated probe hooks. The preflight's
success proves client ABI/query behavior, not permission to execute our helper.

## Service execution boundary (2026-10-04)

`service-native-02` resolved the ordinary spawn and kill imports, then stopped
because its precondition wrongly treated the local-process identifier as a
native PID. Descriptor counts stayed 12/12, with zero fault records. That
assumption was removed; only the service-returned identifier is used for close.

[`service-native-03.json`](../../../../evidence/service-native-03.json) records the first
accepted service request. Parent native PID 130 had application ID 57368 and
service identifier `0x1e018260`. AddLocalProcess returned `0x1e018271`;
SysCore executed `/app0/service-native-helper.bin` as native PID 133, under the
same application. This disproves the claim that service-created homebrew
processes are inherently impossible.

The helper was a second copy of the full launcher executable. Its VideoOut
module reported `gbase_gvmsw_open` error 6978, then the loader raised
`0xa0020104 (PRX_PROCESS_STARTUP_FAILURE)` before helper entry. Sony's crash
handling closed the application, interrupting the parent's trace. The runner
recorded two fatal-signal lines, no normal parent shell exit, and **FAIL**. It
restored the signed production SELF and removed the synthetic helper. The
console remained reachable. No handshake, child completion, service close or
budget-reclamation result can be claimed from this attempt.

The revised helper is original freestanding C with only ordinary `libkernel`
as a declared dependency. It avoids the launcher CRT, platform workers,
heap, graphics and SystemService dependencies. Its socket reply carries a
native PID and an explicit unqueried app field (`-1`); the parent verifies the
returned service identifier in its current application's process list. It
reports completion over the socket, then waits for the parent to close that
owned identifier through KillLocalProcess. Completion value 37 is a protocol
message, not proof of a native exit status. Production builds exclude both the
parent experiment and the helper.

`service-native-04` retained only `libkernel` in its declared dependencies but
still loaded VideoOut automatically and raised the same startup exception.
`service-native-05` added an image process-parameter preload pointer at `0x50`,
relocated to the libc-only mask `0x8000000000000002`; this alone also failed.
Both records remain **FAIL**, with production restored and helper removed.

The separately traced **service request** preload field at public `0x18` passes
through ShellCore `0x61d520` into process parameters. Sysmodule's zero-mask path
at `0x58f0` selects the default startup list, including VideoOut. Its negative
mask path selects modules by the resource masks in its pinned selector table;
libc is module 5, bit `0x2`, and VideoOut is module 13, bit `0x100`. The image's
`0x50` control is conditional on the native process-type flags; it is not an
unconditional replacement for the service field. No privileged budget patch is
part of this experiment.

[`service-native-06.json`](../../../../evidence/service-native-06.json) puts the same
libc-only mask in the **service request**. It returned service ID `0x160182f1`,
and SysCore executed native PID 155 under application ID 24600. No startup fatal
signal occurred. The socket handshake timed out, so the overall test is still
**FAIL**. The parent then closed its returned identifier through KillLocalProcess
(status zero), verified the child absent and process-list count restored to one,
and returned through its normal shell path. Parent descriptors were **12/12**,
with zero fatal-signal records. This proves the tested timeout/close path, not
helper completion, transferred socket correctness or budget reclamation.

The helper now logs its entry stages to stderr and matches the explicit final
two arguments, so a service-supplied argv[0] cannot silently bypass its protocol.
The optional converter patch records an image preload pointer without changing
ordinary output: the default helper ELF remained byte-identical to the original
converter, and the selective pointer's relative relocation and mask were checked
offline. These checks do not prove that the image control is needed in addition
to the service field.

## Working native helper (2026-10-04)

[`service-native-07.json`](../../../../evidence/service-native-07.json) and
[`service-native-08.json`](../../../../evidence/service-native-08.json) both passed:

| Observation | First run | Repeat |
| --- | ---: | ---: |
| Parent native PID | 157 | 162 |
| Helper native PID | 160 | 165 |
| Helper service ID | `0x18018321` | `0x1a018351` |
| Identity-bound handshake / completion | yes / yes | yes / yes |
| Socket at descriptor 3 | SOCK_SEQPACKET (5) | SOCK_SEQPACKET (5) |
| Service close status | 0 | 0 |
| Helper present after close | no | no |
| Process-list count before / after | 1 / 1 | 1 / 1 |
| Parent descriptors before / after | 12 / 12 | 12 / 12 |
| Post-launch fatal-signal records | 0 | 0 |
| Parent shell exit / production restored | yes / yes | yes / yes |

The final build repeated these checks in
[`service-native-09.json`](../../../../evidence/service-native-09.json): parent native PID
171, helper PID 174, helper service ID `0x1e0183a1`, matching identity/completion,
service close status zero, child absent, list count 1/1 and descriptors 12/12.
It closed normally with zero fatal signals. After restoring production,
[`service-native-restored-02.json`](../../../../evidence/service-native-restored-02.json)
and its inspected capture show eight library entries, 120 frames in 2.36 seconds
(59.9 FPS after the first half), normal shell exit and zero fault/driver errors.

The same application association is checked through the parent's current-app
service list, not an app-status query in the kernel-only helper. Its reply's app
field is explicitly `-1` (unqueried). The service/native identifiers remain
separate. SysCore's captured NOTE_EXIT and application process-deletion events
corroborate removal of the owned native helper. Completion 37 is a socket message;
it is not a POSIX wait status or Windows exit code. No Wine modules were loaded.
The robust argument matcher succeeded; the exact child argc was not captured,
since the helper's stderr stage messages did not reach this klog stream.

[`service-native-missing-01.json`](../../../../evidence/service-native-missing-01.json)
requests only the synthetic `/app0/service-native-never-exists.bin`, checked
absent before deployment. Sony returned `0x80020002`. The main process remained
listed, count stayed 1/1 and descriptors 12/12. It passed the expected-failure
check, closed normally and restored production, with zero fatal signals.

Build/run the isolated experimental title and original helper:

```sh
python3 tools/build-child-probe.py --kind service-native
python3 tools/run-child-probe.py --kind service-native --build .deps/child-probe/BUILD_ID --name UNIQUE_RUN
# Separate expected-failure run, with a different evidence name:
python3 tools/run-child-probe.py --kind service-native --missing-image --build .deps/child-probe/BUILD_ID --name UNIQUE_FAILURE_RUN
```

The builder verifies the helper has only ordinary `libkernel` as a declared
dependency. The runner validates the signed artifacts and selective image
metadata before console access, checks idleness, saves production, verifies
uploads, captures fresh results, and restores production/removes its helper.
Startup hooks and deployment state are persisted for interrupted-run recovery.
All probe flags remain OFF in ordinary builds; bare direct spawning is still
blocked after its recorded kernel panic.

Remaining gates: isolate the need for the image preload pointer, prove closure
of a still-live helper when the application ends, measure budget reuse, then
attach one PE64 process to the existing wineserver and validate inherited
handles/events/exit status/termination. The native proof does not remove Wine's
current process-creation guards or grant a secondary process graphics ownership.
