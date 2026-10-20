> Historical snapshot of `docs/CONFORMANCE.md`, archived on 2026-10-07 from `9b07ebb`.
> Defaults, next steps, counts and failure classifications below belong to their original runs.
> Use the [current document](../../../CONFORMANCE.md) for present guidance; see the [archive index](../README.md).

# Conformance tests

A game that fails shows one symptom of one broken behaviour, and the same behaviour breaks the
next game differently. So the runtime is measured against tests of the behaviour itself, the way
the Vulkan driver is measured against the Vulkan CTS, and a game run is the last check, not the
way bugs are found.

| Layer | Tests | Where they run |
| --- | --- | --- |
| Windows API and runtime (memory, files, threads, synchronization, exceptions, loading) | Wine's own conformance tests (`dlls/*/tests`), built from the pinned source | one title launch per test file on the console, and the same files on the pinned Wine on Linux |
| OpenGL | the Khronos GL CTS (PS5_OpenGL: the full 4.6 run), plus probes for what it does not measure | the console |
| Vulkan | the Vulkan CTS (PS5_Vulkan) | the console |
| Direct3D 9/10/11 and DXGI | Wine's tests with the DXVK DLLs loaded | the console |
| Direct3D 12 | VKD3D-Proton's own tests | the console |
| A whole game | repeatable scenarios | the console |

## Wine's tests

```
tools/build-wine-tests.sh ntdll kernel32 kernelbase     # .deps/wine-tests/exe/<dll>_test.exe, about 25 s each
tools/run-wine-tests.py host ntdll                      # the baseline: the pinned Wine on Linux
tools/run-wine-tests.py console ntdll                   # one launch per test file, paced
tools/run-wine-tests.py compare ntdll
```

A Wine test program takes the name of one test file; the ones built here also take a list
(`virtual,path,env`) or `--all` and run those files in turn in one process, printing
`winetest-batch: begin NAME` and `winetest-batch: end NAME STATUS` around each. The console has no
child processes (patch 0550), so a test program cannot be run by another one; and a title launch
is expensive: it opens and closes the video output twice, and back-to-back launches have cost
power-offs and a loss of the console's services. So the runner launches many test files per
launch (eight by default), reads the output, and starts the next launch with the files that did
not get to run, after the one that crashed or hung. A launch is paced (90 s apart, a longer pause
every third). The program's standard output goes to `/data/ps5proton/logs/wine-stdout.txt`
(patch 0900, selected by `WINE_PS5_STDOUT`, which `run-console.py` sets) and the run tool reads the
file back over FTP. A test that leaves the title hung is asked to stop through the title's stop
request and, if it ignores it for a minute, killed with `tools/kill-hung-title.py` (reported).

Each test file ends as one of `complete` (it printed its summary), `incomplete` (it ended without
one: a crash, or an exit in the middle of the test), `timeout`, `skipped` (named in
`tests/wine-tests-skip-ps5.json` with the reason) or `no output` (the run itself failed; it is run
again). Results go to `evidence/wine-tests/<dll>-<host|console>.json`.

### Reading the comparison

A failure on the baseline too is Wine's or the test's, not the port's. The baseline is the pinned
Wine on Linux with `fork()` made to fail (`tools/noproc.c`, `--noproc`), because that is what the
console is: a test that needs a child process fails there for the same reason, and is not the port's
defect. Of the rest, a failure is **expected** when `tests/wine-tests-expected-ps5.json` names it
(test file, `file:line`) with the reason; `tools/classify-wine-failures.py <dll> --write` rebuilds
that file from the rules at its head (test file, range of lines, reason) and prints what no rule
explains. The reasons so far:

* **no child processes**: `CreateProcess` fails on the console (patch 0550). Tests that start the
  test program again, and everything they then do with the child, fail.
* **16 KiB host pages**: the console's pages are 16 KiB and Windows' are 4 KiB. Write watches are
  reported for the whole host page, a guard region is a host page, a part of a host page cannot be
  freed on its own.
* **polled directory changes**: the server polls (25 ms) because the console has no inotify; a wait
  with a zero timeout right after the change is too early.
* **no hard links, symbolic links, reparse points, hidden or system flags**: the console's file
  system (FAT-like) has none.
* **no hardware debug registers**, **segment selectors**: the CPU state a program may read differs.

A reason that starts with `OPEN:` is a defect of the port that is known and not yet fixed. It is
listed apart in the comparison and leaves the file when its fix lands. Anything the comparison
reports and no rule explains is a defect to reduce to a small reproducer, fix in the layer that
owns it, and keep as a probe or test (`tests/*-probe.c`).

### Where it stands

Console runs of 2026-10-03 and 2026-10-04 (the latest with patches 0914-0916), with the programs ending a stuck test
file themselves (a 60 s watchdog per file: nothing was killed), against the baselines on Linux. A count is of distinct
failing lines (file:line) the baseline lacks; `tools/run-wine-tests.py compare <dll>` prints them by reason. None is
unexplained, and no test file ends differently from the baseline.

| Program | Test files | Console's limits | Open defects of the port |
| --- | --- | --- | --- |
| ntdll | 23 complete, 2 incomplete (`exception` is one), `threadpool` skipped | 197 | 6 (all the mount manager) |
| kernelbase | 6 of 6 complete | 4 | 0 |
| kernel32 | 26 complete, 5 ended by the watchdog or skipped (`debugger`, `pipe`, `process`, `sync`, `toolhelp`: child processes), 2 incomplete (`fiber`, `virtual`) | 146 | 78: 64 the mount manager, 7 `\KnownDlls`, 4 builtin DLL headers, heap order, a mailslot name, a pipe name |

What 2026-10-04 settled:

* **Patch 0916, BOOLEAN syscall arguments.** A BOOLEAN reaches a syscall as one byte of a 64-bit slot; the PE side
  stores only the byte (`movb $0x0, 0x20(%rsp)`), and the console's Unix side, built by clang for the System V ABI,
  tests the whole register (`testl %r8d, %r8d` in `NtQueryDirectoryObject`). kernelbase's `GetLogicalDrives` therefore
  restarted its scan of `\DosDevices` on every call and never ended, which is why kernel32 `drive` and `path` were
  ended by the watchdog with no output (`path` since kernelbase became a patched module). The dispatcher now extends
  the narrow arguments of the 36 ntdll syscalls that have them (events' initial state, waits' alertable flag,
  `NtQueryDirectoryFile`'s restart, ...) from their low byte. `drive` runs through for the first time (4 lines, the
  volume names) and `path` completes again (17 lines, all also on Linux); the PE64 probe passes with it.
  win32u's narrow arguments (WORD, BYTE) are not covered yet.
* **Patch 0914**: CreateProcess of a missing file fails with ERROR_FILE_NOT_FOUND (`process.c:1131`, `1136` gone).
* **Patch 0915**: NtReadFile on a directory is an invalid device request (`ntdll file.c:197` gone).
* The refreshed probe prefix removed 16 of the 50 builtin-DLL header lines in `loader`; `LdrGetDllPath`'s four
  `\\?\c:\` lines no longer fail; `om` run alone has none of its 11 object-directory lines (a batch effect).

The open defects (`OPEN:` in `tests/wine-tests-expected-ps5.json`) are being fixed. Verified on the console: patch
0903 (a process object's own memory), 0904 (volume information and the file id's volume), 0905 (process memory
counters, also for a handle to the process's own id), 0906 (processor times), and the trailing-slash rule in
`pw_wine_cwd_libc.c` (ntdll `path`: 0 failures).

`NtFlushProcessWriteBuffers` was a stub on the console, and kernel32's store buffer litmus test counted 30 reordered
stores with it: a hazard for .NET programs. The first fix I tried (the page-protection trick CoreCLR uses where it has no
`membarrier`: dirty a page, then take its access away) made about 920,000 `mprotect` calls in 30 s on a page that came from a
plain `mmap` that Wine does not know about, and the console dropped off the network after the run, so I removed it. (The
stack overflow that ended the title in those runs was not its doing: it is `test_stack_commit`, below, and happens without
it. Why the console dropped stays unproven; the patch stays removed.) Patch 0911 does it without any kernel interface: the
CPU fences and waits 5 microseconds, far longer than a store takes to leave its store buffer, which gives every thread that
reads after the call the stores that came before it. That rests on an argument about x86 store buffers, not on the test: on
the host the litmus test passes with the wait and also with none at all. On the console, with patch 0911, the litmus test
reports no reordering (it counted 30 with the stub).

kernel32 `virtual` still ends without its summary line: its last test, `test_stack_commit`, moves the thread onto a stack it
allocates and grows it through guard pages, and the title ends with STATUS_STACK_OVERFLOW (c00000fd) in the test's
`stack_commit_func`. A guard page is a 4 KiB Windows page and the host page is 16 KiB, so the guard of a stack that is not the
thread's own cannot be set; programs that run on a stack of their own and rely on guard-page growth (some coroutine and
fiber libraries) cannot work, and ones that commit their stacks can.

The largest group of what is left has one cause: the processes that Windows starts at boot cannot start. The mount
manager is a driver of `winedevice.exe`, so there are no DOS device links (`GetLogicalDrives` returns 0, `QueryDosDevice`
and volume names fail, `drive` waits for a drive that never comes, `volume` fails throughout); `wineboot` creates the
volatile registry keys at every start (the computer name, the user profile) and creates the known DLL sections. Running
that work inside the title is the next design item.

The Direct3D tests (`d3d11`, `d3d10core`, `dxgi`) are built with every test function guarded, so one that crashes is
reported and the next one runs, and they run with one test thread and quiet DXVK. Their baselines on Linux (the system
Wine with the same DXVK DLLs, in a Wine virtual desktop) are recorded for `d3d11` (4094 failing lines) and `d3d10core`
(1404); `dxgi` has none, because it switches display modes and went fullscreen on the real screen. It runs on the host
only with `--allow-dxgi-on-host`. None has run on the console yet.

A title launch costs the console something. It has dropped off the network three times: twice after about 13
launches since a restart (once after a normal exit, with nothing killed), and once on the fourth launch of a boot, in
the run of the `FlushProcessWriteBuffers` experiment described above. A ten-launch run (the second boot of 2026-10-03)
did not drop it. At that stage I stopped at ten launches per boot, and every test file that waits for a child process (`debugger`,
`drive`, `pipe`, `process`, `sync`, `toolhelp`) costs a launch of its own, which is why they are on the skip list
from now on, keeping their results.

Every launch goes through `tools/console_guard.py`: after a drop the tools refuse to launch until the console has been
restarted by hand and `tools/console_guard.py new-boot` is run. At the user's request on 2026-10-04,
there is no longer a per-boot launch limit in either elevation mode. Launch counts remain diagnostic,
and launches are still paced 60 seconds apart. The tools never run `new-boot` themselves.

### The C runtimes and Winsock on the console (2026-10-04)

ucrtbase (9 files, 8 per launch) and msvcrt (13 files) compare with **no unexplained failure**. Their first batches
showed thousands of lines that were not the console's: several test files share one process on the console, and
some assume a fresh one. ucrtbase `string` checks `tolower` in the C locale, which Wine answers on a fast path only
until any `setlocale` picks another locale (an earlier file did), so 7581 lines; alone it passes all 787,359 tests.
msvcrt `string` (EILSEQ after another file's locale) and `environ` (`_wenviron` already built), ucrtbase `misc`
(an invalid-parameter handler already set, `clock()` counted from the process start) and `printf` pass alone or match
Linux. A file that fails in a batch is rerun alone before its lines are classified.

**ws2_32 is the open one**: 227 lines Linux lacks (`afd` 24 against 17, `protocol` incomplete, `sock` ended by the
watchdog where Linux completes, `WSAConnectByName` failing with WSAEFAULT). The console has no DNS, and the tests'
loopback connections and name lookups have not been taken apart yet; they are left unexplained, not labelled.

### Direct3D 12 on the console (2026-10-04)

Wine's `d3d12` file completes on the console: 671 tests, 8 failure lines (13 on the Linux baseline). VKD3D-Proton
creates its device and swapchains through winevulkan and RADV. One line was the console's own (`d3d12.c:955`): DXCore
enumerates adapters through wined3d, whose OpenGL adapter cannot start there ("Failed to get a GL context for
adapter"), so its adapter list was empty. The title now starts every program with `WINE_D3D_CONFIG=renderer=vulkan`
(a game's `env` can say otherwise), and with it the file runs 673 tests with 6 failure lines and none of its own. The morning's empty record was
that run's alone: the same file's output came back this time.

The console's own log shows a GPU reset sequence at every title start and end (the switch between the home screen
and a title), and a line "gc_reset_gc_blocks ... GPU reset failure" at some of them: in runs of a .NET (MonoGame) game before any of
today's patches as in the Direct3D runs. The console carried on each time; it is recorded here, not explained.

### `d3d10core` in four slices (2026-10-04)

All four slices ran (4514, 3720 and 41444 tests in the first three; the fourth ended early), and no launch ran out of
descriptors: patch 0910 holds. Every failure line also fails with the same DXVK on Linux except two guarded test
functions that crash with an access violation (offsets 0x7be0 and 0x9d90 of the test program, which carries no
symbols; both crashed on 2026-10-03 too). The fourth slice ends when a worker thread runs 35 KB past the bottom of
its 1 MiB stack and its exception cannot be dispatched ("Exception frame is not in stack limits"); Linux completes
the file. A nested fault, each signal frame carrying the console's large XSAVE area, would look like this; it is not
yet traced. Both are `OPEN:` in the expected list.

### First Direct3D run on the console (2026-10-03, third boot)

`d3d10core` ran for 900 s and ended itself (the watchdog), with the console healthy and nothing killed: 397 failing lines,
3 of them crashes caught by the guard, then the test got to `d3d10core.c:10502` of about 19,800 and stopped making progress.
The klog shows why: `kqueue failed with error: 24 (Too many open files)` from thread id 0x420 on, tens of thousands of times.

0x420 is the 264th thread-id slot (ids go up by 4), and a console process can hold only about 256 files and kqueues
together (`ps5platform/fdv.h`), whatever `getrlimit` says. Wine keeps one kqueue per thread-id slot for its thread alerts
(`NtWaitForAlertByThreadId`, under every critical section, SRW lock and condition variable) and never closes it, so
the slots a program ever waited in used up the 256, and a program that needs its files has fewer to open them with.
`tests/thread-leak-probe.c` first showed it was not a plain leak per thread (3,000 threads created and ended, 200 alive at
once: all fine), and its fourth phase, threads that wait once, is the one that fails on this build. Patch 0910 replaces
the kqueue with a mutex, a condition variable and a flag per slot, which cost no descriptor. Before it goes to the console
it was run on the host: the algorithm under load (`tests/test_pw_thread_alert.c`), and the patch itself in a copy of the
pinned Wine built for Linux with the same code path forced (`-DPS5_ALERT_PTHREAD` for `__PROSPERO__`), where Wine's own
all 25 `ntdll` files that complete and 19 `kernel32` files give the same failures as with the futex alert, and so does
a fresh prefix: `wineboot --init` runs through with the pthread alert and starts `services.exe`, both `winedevice.exe` hosts
and `plugplay.exe`, which is a heavy user of waits. (Two traps in that comparison: a prefix first initialized by an
earlier, broken build stays broken, with no services and so no mount manager; and the working directory's path length
changes what `kernel32` `file` tests, so both builds must run from the same directory.) That run found a real fault
on the way: the blocks that hold the entries were sized as 64 KiB divided by the entry, which stopped being a whole number of
host pages once the entry was a mutex, a condition variable and a flag, and `anon_mmap_alloc` asserts on it. The console
run is still to come.

The Direct3D files run in slices from now on: `d3d11@40-79` in the test list runs the guarded calls 40 to 78 of that file
(counted in the order the file makes them), so that what the calls leave behind stays within one launch of the console.
`tools/run-wine-tests.py count d3d11` records how many guarded calls a file makes (`tests/d3d-guarded-counts.json`: 111 for
`d3d10core`, 215 for `d3d11`), and `tools/run-wine-tests.py console d3d10core --slices 4` runs it in four launches, which
`compare` merges back into one record. On the host the three slices of `d3d10core` together give the failing lines of the
whole file (87 against 86; the difference is test order).

### VKD3D-Proton's own tests

`tools/fetch-vkd3d-tests.sh` builds VKD3D-Proton's `tests/d3d12.exe` (574 tests) at the revision GE-Proton11-7 ships, with a
pinned llvm-mingw (clang) unpacked user-locally, plus two small changes to its harness kept as `tests/vkd3d-test-range.patch`:
`VKD3D_TEST_RANGE=from-to` runs a slice of the list by position, and `VKD3D_TEST_TIMEOUT` ends the program, after naming the
test, when no test has made progress for that long. `tools/run-vkd3d-tests.py host` runs it one test per process on the host
(the system Wine in a virtual desktop, Intel graphics, VKD3D-Proton's DLLs): of 574 tests 572 complete, 3 time out
(`test_fence_pending_signal_cpu_rewind_shared`, `test_fence_wait_robustness_shared`, `test_open_heap_from_address`), 25.2 million
checks run, and 9 tests have failing lines (the border colour limits, structured buffer addressing and a few more: that
graphics driver's, not ours). `tools/run-vkd3d-tests.py console --slices N` runs it on the console, N launches, each a slice, a
test that ends the title recorded and skipped by the next launch. Nothing has run on the console yet: Wine's small
`d3d12` file (13 failing lines on the host) goes first, because it decides whether Direct3D 12 works there at all.

### Dry runs of the console runners

A bug in a console runner costs a launch, and a launch costs the console something, so the runners are tried first with
`PW_STUB_CONSOLE=1`, which swaps `tools/run-console.py` for `tools/stub-console.py` (it runs the program on the host the way
the host baselines do and writes the same `klog/<run>-stdout.txt`), and `PW_EVIDENCE_DIR=<scratch>`, which keeps the
evidence files of a dry run apart from the real ones (copy the baseline there for `compare`). The sliced Direct3D run
(`console d3d10core --slices 3 --gap 0 --pause 0`) was done this way: three launches, merged into one record and compared
with the baseline.
