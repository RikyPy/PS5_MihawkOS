> Historical snapshot of `docs/OPTIMIZATION_LEDGER.md`, archived on 2026-10-07 from `9b07ebb`.
> Defaults, next steps, counts and failure classifications below belong to their original runs.
> Use the [current document](../../../OPTIMIZATION_LEDGER.md) for present guidance; see the [archive index](../README.md).

# Optimization ledger

One entry per completed round of the compatibility and optimization work
(`docs/CLAUDE_OPTIMIZATION_HANDOFF.md`): what was believed, what changed, what the
console showed. Raw logs stay in `klog/` (ignored); each run's small summary is in
`evidence/<run>.json` (`tools/run-console.py` writes it, `--reparse` rebuilds it from
the logs). Times are the console's unless a line says host.

Scope as of 2026-10-03: the FNA game and a native 64-bit game are set aside at my instruction; a .NET (MonoGame) game
Valley (OpenGL) and the PE probes are the regression targets, and the six
optimization priorities are the work.

## Status of the six priorities

| # | Priority | State |
| --- | --- | --- |
| 1 | PS5-native Windows synchronization | device in place, opt-in (`WINE_PS5_NTSYNC=1`): its freeze (a SIGUSR1 handler running while its thread held the device lock) traced and fixed, the .NET game 10 closes of 10 (R3); R6 runs the server's sync and handle requests on the calling thread by default |
| 2 | Windows-to-PS5 thread priorities | implemented and console-tested, opt-in (`WINE_PS5_SCHED=1`); strict-priority starvation remains a limit |
| 3 | GPU submission and presentation overhead | measuring |
| 4 | Unified memory | measuring |
| 5 | Shader and pipeline caching | in progress |
| 6 | Asynchronous GPU compute | not started |

## R1 — measurement and console tooling

- **Hypothesis:** optimizing needs per-subsystem counters that cost nothing on game
  threads, a console harness that ends a run the moment it is over and never leaves a
  title running, and Wine patch iterations measured in seconds, not full rebuilds.
- **Parent:** `19215d8`. **Wine:** `1c78596…` + 55 patches (new: `0880`).
- **Changed:** `0880` counts every server round trip by request, `server_select` and the
  time spent blocked in it, `NtWaitFor*Objects` by shape, `NtDelayExecution`, and both
  presentation paths (EGL swap, `vkQueuePresentKHR`: interval histogram in 250 µs
  buckets and the call's own time) into `wine/ps5/pw_wine_perf.c`, with relaxed atomic
  adds and a TSC read; the title snapshots them every 5 s from its own thread and prints
  `perf:` lines (frames, p50/p95/p99/max, server calls, top five requests by time, and
  memory: direct memory, the Unix heap's live and mapped bytes, allocation traffic,
  contention on the heap's mutex, host VM calls). `wine-run.txt` accepts one-shot
  `env=NAME=VALUE` overrides. `tools/run-console.py` runs a probe or a game, ends on the
  title's "ends" line, a crash record, the title's exit or an exit stall (a probe that
  printed its verdict but is not back in the launcher within 15 s is asked to return
  through `stop-game` and fails), and writes `evidence/<run>.json`.
  `tools/host-tests.sh` builds and runs every host test with ASan and UBSan. New probes:
  `sync-probe.exe` (Windows sync semantics, then cost), `exit-probe.exe` (exits during
  HID start). `tools/audit-pe.py` reports PE/CLR architecture, flags and imports.
- **Build:** the patched source is computed in a private git index and only files whose
  content changed are rewritten; generator outputs rewritten with identical bytes keep
  their time (`tools/preserve-mtimes.py`); the build directory survives patch changes.
  configure now runs only from the script: make's own `config.status --recheck`
  inherited the targets' `LDFLAGS` (`--warn-unresolved-symbols`), every link test passed,
  and `config.h` claimed `prctl`, `sched_getcpu` and macOS attribute calls — observed
  once, fixed. No-change rebuild 21.5 s (full: 88 s); one-patch iteration about 30 s.
- **Correctness:** 8 host tests pass (ASan/UBSan); `sync-probe.exe` passes every check
  on the host's Wine 11.18. On the console the counters print as designed
  (`klog/r1-exit-diag1.log`).
- **Found:** the short-process exit stall reproduces every time once the probe prints
  through `OutputDebugStringA` before `ExitProcess`: `r1-exit-02`…`04`, 3 of 3 stalled,
  returned only through `stop-game` after 15 s. Server-side tracing showed
  `terminate_process` completing and SIGQUIT sent to the two running PnP threads; the
  exiting thread then never logged again. With tracing off (`WINEDEBUG=-all`) the same
  probe returned in 1.7 s. A killed thread held the title's output-sink mutex.
- **Limitations:** the .NET game baseline is not in this round: the console left the
  network (rest mode, it appears) before it ran.
- **Disposition:** kept. **Next:** fix the exit stall at its cause.

## R2 — exit robustness: threads end at the user-mode boundary, the registry survives

- **Hypothesis:** a thread killed inside Wine's Unix side keeps its locks forever in a
  PS5 title, because every Wine thread, the in-process server and the title share one
  process; ending such a thread only on its way back to user mode removes the stall
  without changing what a game observes.
- **Parent:** `84b9e52`. **Wine:** + `0885`, `0886` (57 patches).
- **Changed:** `0885`: `quit_handler` marks a thread that is inside a syscall or Unix
  call, and the syscall and Unix-call dispatchers end it on return
  (`ps5_dispatcher_terminate`); a killed thread's closed server connection now returns
  `STATUS_THREAD_IS_TERMINATING` instead of aborting in place, so callers unwind and
  release what they hold; `abort_process` exits through `exit`, so the title returns to
  the launcher. The title's output sink assembles lines per thread, with no shared lock,
  into an `O_APPEND` log. `0886`: each registry branch is synced before the rename that
  publishes it, and the title asks the in-process server to save (a byte on its client
  channel; the server thread saves and answers) and waits before every LoadExec,
  including `stop-game`, which until now lost every registry change since the last save.
- **Incident:** at 00:15 the console left the network right after a probe run; the
  probe prefix's `system.reg`, `user.reg` and `userdef.reg` were 0 bytes, all stamped
  that minute: the rename of an unsynced save had reached the disk and the data had not.
  `r2-exit-01`…`03` ran into it (`'…probe' is a 32-bit installation`). The probe prefix
  (test state only) was restored from the staged copy; the .NET game's prefix was untouched.
- **Correctness:** `exit-probe.exe` with full tracing: `r2-exit-04`…`06` PASS, back in
  the launcher in 3.4, 2.3 and 2.1 s (stalled 3 of 3 before), registry saved in
  148–157 ms. `wine-probe.exe` (`r2-pe64-01`) and `d3d11-probe.exe` (`r2-d3d11-01`,
  60 fps, p99 17.0 ms) PASS. The runs used one build with `0885`, `0886` and `0890`
  (`0890` is off unless asked for).
- **Disposition:** kept. **Next:** the in-process synchronization device.

## R3 — Windows synchronization on a device inside the title

- **Hypothesis:** every wait, SetEvent, ReleaseSemaphore and ReleaseMutex is a round
  trip to the in-process server thread over a socket pair; Wine's in-process
  synchronization path (server/inproc_sync.c and the inproc paths of ntdll, written for
  Linux's ntsync device) takes them off the server, and a PS5 title needs no kernel
  device for it: the server and every thread are in one process.
- **Parent:** `26883ea`. **Wine:** + `0890` (58 patches).
- **Changed:** `src/pw_ntsync.c` implements ntsync's documented semantics in the process
  (one lock over a table of numbered objects, a condition variable per waiting thread on
  CLOCK_MONOTONIC, FIFO service): semaphores, recursive and abandonable mutexes, manual
  and auto-reset events, pulse, wait-any by lowest index, atomic wait-all, alerts,
  realtime and monotonic deadlines, objects kept alive by the waits that use them.
  `include/wine/ps5_ntsync.h` (patch `0890`) maps the ntsync requests Wine's code makes
  onto it, so that code runs unchanged; ntdll links the device and hands its operations
  to the in-process server; object numbers travel in the reply field Proton's fsync uses,
  with a reference taken for the client, and are released instead of closed.
  `WINE_PS5_NTSYNC=1` turns it on; off, every object stays in the server as before.
- **Host:** `tests/test_pw_ntsync.c` — semantics, wait-all atomicity under 6 threads
  (80 000 wait-alls, counts conserved), FIFO single wakeups, pulse, abandonment,
  alerts, timeouts, lifetime past the last holder — passes under ASan/UBSan (gcc and
  clang) and ThreadSanitizer.
- **Console (`sync-probe.exe`, same build, two runs each):**

  | µs per operation | server (`r3-sync-server-*`) | device (`r3-sync-device-*`) |
  | --- | --- | --- |
  | event ping-pong round trip | 37.2, 36.9 | 9.85, 9.44 |
  | wait on a signalled event | 16.58, 16.45 | 0.960, 0.963 |
  | SetEvent + ResetEvent | 29.3, 29.2 | 0.200, 0.199 |
  | semaphore release + wait | 31.1, 31.3 | 1.07, 1.08 |
  | mutex take + release | 31.2, 31.4 | 1.08, 1.08 |
  | wait-any / wait-all of 4 | 16.6 / 16.8 | 0.99 / 1.00 |

  Every semantic check passes on both paths; the whole probe takes 2.9 s instead of
  6.0 s. Contended critical sections (0.07 µs) and Sleep are not on this path.
- **Limitations:** not yet run under a game; it stays opt-in until the .NET game and the
  probes pass with it on.
- **Disposition:** kept, opt-in. **Next:** the .NET game with the device on, then default.
- **The .NET game with the device on (2026-10-04, with R6's direct server requests):** the device is in use ("ntsync: up
  and running (in-process device)") and `sync-probe.exe` passes every check with both on: wait on a signalled event
  0.99 µs, mutex take + release 1.09, semaphore release + wait 1.08, SetEvent + ResetEvent 0.20, wait-any/all of 4
  1.02/1.04, event round trip 10.1. The .NET game at the title screen (2026-10-04; record removed from the tree; in git history): 59.9 FPS, first frame 8.31 s,
  closed cleanly; server requests 2810 a second instead of 4642, but a new one, `get_inproc_sync_fd` (about 375 a
  second at 16.5 µs, a handle's device object asked of the server through the pipes), keeps the server time about
  equal (35 against 34 ms a second). The save lifecycle: loading the test save, sleeping and saving
  (day 1 -> 2) closed cleanly; the relaunch loaded Tue 2 (2026-10-04; record removed from the tree; in git history) but at
  the scripted close the game froze: the WM_CLOSE was delivered (1 of 1), then no frame and no server request for
  two minutes, every thread waiting. The title left it running, as it must, and it was ended with
  `tools/kill-hung-title.py` (the console stayed up). The same close works without the device, so the device has a
  missed wake-up or a deadlock under a game's shutdown that only shows some of the time.
- **Disposition:** back to opt-in.
- **`get_inproc_sync_fd` on the calling thread (patch `0920`):** on the PS5 the handler looks the handle up, reads
  the object's device number and takes a device reference; no descriptor is passed and nothing sleeps. With the
  device on (2026-10-04; record removed from the tree; in git history): 869 server requests a second at 5.45 µs, about 4.7 ms of server time a second
  (35 before 0920 with the device, 34 without it), 59.9 FPS, first frame 8.18 s; the top requests are now the
  in-process HID driver's and input. But that run's close froze too, at the title screen this time: two closes of
  four have frozen with the device, none without it. Ended with `tools/kill-hung-title.py`; the console stayed up.
  With the device off ntdll does not ask this request, and the .NET game runs as before (2026-10-04 (record removed from the tree; in git history): 7.2 µs on
  average, clean close).
- **The freeze traced (patches `0921`, `0922`, 2026-10-04):** the device now keeps who holds its lock (the kernel
  thread, where it is and in which call, until the lock is released for good), its blocked waits, counters, object
  tags from the server (type and thread id or handle), and with `WINE_PS5_NTSYNC_TRACE=1` a ring of the last 65,536
  operations; Wine's SIGINT, SIGQUIT and SIGUSR1 handlers tell it when a signal lands on a thread inside it. The
  server records its lock's holder and prints the watchdog's snapshot on demand, or, when its lock does not move,
  the thread list read without it. The title prints both 10 s and 30 s into a close that has not ended. Cost, with
  the device on (`sync-ntsync-0921-01`, `sync-ntsync-trace-01` against `sync-ntsync-0920-01`): wait on a signalled
  event 0.981 -> 0.990 µs (0.985 with the trace), SetEvent + ResetEvent 0.196 -> 0.216 (0.230), event round trip
  10.06 -> 10.32 (10.48). The .NET game at the title screen with the device and the trace: runs 01 and 03 closed in 73 s;
  02 and 04 froze. Neither froze at the close: 02 stopped 12 s before it, in a burst of touchpad input (194 mouse
  inputs posted, 9 delivered; 78,182 waits in that 5 s against about 8,800), 04 about when it came. In both, neither
  lock moved in 20 s and nothing ran but the audio mixer, which needs neither. Run 04 names them:
  thread `0050` holds the device's lock inside a wait, and the run's one signal that reached a thread inside the
  device is a SIGUSR1 (30) to `0050` there; thread `0034` holds the server lock in `create_event`, whose device
  object it is creating. So the suspend handler on `0050` makes its server call (a `select`, on the calling thread
  since R6) while holding the device's lock, and waits for the server lock; `0034` holds that and waits for the
  device. Linux's ntsync is a kernel driver: no signal handler runs while its lock is held. Here one can. `02`
  was the same deadlock, seen before the holder was kept through the unlock: the device lock was held by a thread
  between clearing its name and unlocking, the server lock by another. No coredump, fatal signal or crash line in
  either klog; both titles were ended with `tools/kill-hung-title.py` and the console stayed up.
- **The fix (patch `0923`, 2026-10-04):** a thread is "inside" the device from before it asks for the lock until
  it has let it go (a slot per kernel thread, reached with atomics only); Wine's SIGINT, SIGQUIT and SIGUSR1
  handlers ask the device first, and a signal that landed inside is noted, the handler returns, and the thread
  raises it again once out. Waits sleep on a semaphore of their own without the lock (`sem_post` from the waker,
  under the lock; a wait that times out as it is served takes its post), so a blocked thread holds nothing and
  is suspended at once. Host test: a thread spinning in the device, signalled 400 times by a handler that itself
  calls the device, finishes (2-8 signals deferred and raised each run); the same test without the deferral
  hangs. Waits that time out while being served: 20,000 semaphore units taken exactly once. Console:
  `sync-probe.exe` passes every check (`sync-ntsync-0923-01`: event round trip 9.51 µs, from 10.32: a woken
  waiter no longer contends for the lock on its way out). **The .NET game, title screen, device and trace on:
  ten runs (2026-10-04; records removed from the tree; in git history) all closed and returned to the launcher (72.9-73.3 s, 59.9 FPS)**, where 4 of 8
  froze before. The Java game with the device (2026-10-04 (record removed from the tree; in git history), below): 51 signals landed inside the device, were
  deferred and raised again, and the game closed cleanly; server requests 25,485 -> 1,979 a second (884 -> 94 ms
  of server time a second), frame rate unchanged (median 84 FPS while walking).
- **Disposition:** fixed. Opt-in still (`WINE_PS5_NTSYNC=1`); a candidate for the default after a longer
  session and the probes with it.

## R4 — a game ends cleanly: three console power-offs and their fix

- **What happened:** on 2026-10-03 the console powered off three times, each time at a
  switch between processes after a run: twice as the launcher closed (00:15, 00:57) and
  once as the running .NET game was replaced by the launcher (07:09). I first took the first
  two for rest mode and kept testing; they were power-offs. The console's data survived:
  the .NET game's prefix registry is complete (96 keys) and its save `aaa_450671602` is
  unchanged since before these runs (checked over FTP, read-only).
- **Cause, as far as the evidence goes:** every one of those switches ended a process
  whose display was live. `stop-game` replaced a game at once while it was rendering,
  and even a game that exited by itself left the PS5 OpenGL SDK's presenter (or RADV's
  swapchain) open, never through the teardown the SDK documents. And the title declared
  120 Hz (`attribute3` `0x80040`), so RADV switched the TV to 119.88 Hz in every
  launcher process and the next process opened the output again within a second: the
  churn the SDK guards against within one process (`lifecycle-reopen.md`), with nothing
  guarding it across processes. My earlier 8K power-offs (ProsperoEden) were display
  teardowns too. This is not proven to be the only cause.
- **Parent:** `12e18d2`. **Wine:** + `0892` (59 patches without `0896`).
- **Changed:** `stop-game` asks the game to close as a user would: the title posts a close
  request, the PS5 driver turns it into WM_CLOSE to the game's window on the thread that
  pumps its messages, and the game ends through its own exit; a game still running
  after 10 s gets NtTerminateProcess the same way, and only one still running after
  15 s is left. On every exit, `exit_process` runs the drivers' exit hooks
  (`pw_wine_add_exit_hook`): OpenGL makes no context current, destroys every EGL surface
  and context it created (they are tracked: the SDK's `eglTerminate` refuses while any
  is left) and terminates EGL, which drains the flips, restores the output and frees the
  presenter; every Vulkan device that presents gets `vkDeviceWaitIdle`. The title then
  stops the audio sink (once no grain is in flight) and closes its AudioOut port, saves
  the registry, and returns. `param.json` declares no 120 Hz, so every process stays at
  59.94 Hz. `tools/run-console.py` starts no run within 8 s of the previous one.
- **Correctness (console):** the .NET game stopped five times through the new path
  (`r6-clean-01`…`05`): WM_CLOSE, the game's own exit, then `presenter closed: release 1,
  surfaces 1/1, contexts 2/2, terminate 1 (0x3000)` from the second run on (the first,
  before the EGL objects were tracked, showed `terminate 0`), audio closed, registry
  saved in 140–261 ms, back in the launcher. `d3d11-probe.exe` exiting with its device
  and swapchain alive (`r6-clean-d3d11-02`): `Vulkan device idle at exit: 0`.
  `exit-probe.exe` PASS. The console stayed up through all nine runs. Host: 10 tests
  (sink: close and terminate requests, exit hooks once and newest first, a bounded
  audio stop). These runs used a build that also carried the R5/R6 changes, with the
  shader caches off and the scheduling switch off.
- **Limitations:** a game that neither closes nor pumps messages is still replaced live
  after 15 s. 120 Hz output is off until switching between processes at 120 Hz is shown
  to be safe.
- **Disposition:** kept. **Next:** thread scheduling, then shader caches.

## R5 — native thread scheduling, with bounded tests and thread lifetimes

- **Parent:** `95fb410`. I resumed the pending scheduling work, kept patch `0896`
  opt-in, and added unregister-before-pthread-exit. Registry lookup holds its lock
  through priority changes and signals so an exiting thread cannot invalidate a
  pthread handle during the operation. The title control loop runs at 690, above
  the Windows mapping (695–700), to preserve input and the stop request under load.
- **Mapping:** Windows threads use native round-robin. Above-normal Windows
  priorities map conservatively upward; normal and lower priorities stay at 700.
  The native scheduler strictly starves lower priorities under full load, so
  lowering background threads would not reproduce Windows' starvation boosts.
  No CPU affinity is changed. `WINE_PS5_SCHED=0` retains the original path.
- **Probe correction:** the first reviewed run failed the equal-priority check
  (`r5-sched-reviewed-priority`): its one-second deadline included thread creation.
  I added a shared start event to separate setup from the measured load, without
  relaxing any pass criterion. The bounded off/on comparison now gives:

  | Measurement | off | on run 1 | on run 2 |
  | --- | --- | --- | --- |
  | normal periodic iterations | 0 | 385 | 379 |
  | time-critical iterations | 0 | 497 | 500 |
  | time-critical p99 lateness, µs | no progress | 8 | 6 |
  | below-normal / idle workers | no progress | both progress | both progress |
  | low-priority lock holder to high-priority waiter, ms | holder never starts | 56.1 | 51.5 |

  Evidence: `r5-sched-barrier-off-01`, `r5-sched-barrier-on-01` and `02`.
  The off-path failure is intentional baseline evidence; all runs exit cleanly.
- **Regression:** `r5-sched-reviewed-game` holds 59.9 FPS median, p99 17.75 ms;
  worst interval maximum 48.75 ms, so I do not claim reduced game stutter.
  First frame 9345 ms. Audio has four short startup grains, then four ten-second
  intervals with zero missing frames and zero nonzero submission returns.
  `r5-sched-reviewed-d3d11`: 180 frames, 3041 ms, PASS and clean exit.
  The R4 cleanup path remains active. No new listening or controller-input claim.
- **Host/build:** the existing ten sanitizer tests pass; the additional registry
  test passes 1034 thread lifetimes with concurrent priority changes and exits.
  Title and all Wine modules build with no unresolved functions in the build report.
- **Earlier evidence:** the carried-forward R3 game runs, `r4-*` and
  `r5-*-default-*` runs predate this review; some used different defaults and the
  pre-barrier probe. They remain historical evidence, not proof of this exact build.
- **Disposition:** kept, opt-in. Saturating all CPUs with higher-priority workers
  can still starve normal threads or the server; no Windows dynamic boosting is
  claimed. Normal/background distinctions are deliberately collapsed, and there is
  no claimed speedup in a v-synced game. Next: persistent per-game shader caches.

## R6 — server requests on the calling thread (from prospero-win)

- **Hypothesis:** with the server in the title (patch 0110), a request still crosses two pipes and two thread
  switches; the requests that only touch sync objects, handles and file opens can run on the calling thread under a
  lock the server thread holds except while it sleeps in kqueue. prospero-win measured it on GTA IV (its #341, #342).
- **Wine:** + `0918` (86 patches), taken from prospero-win `1532474b` and rebased: the thread id comes from the TEB,
  the TEB is the native one `init_thread` sent, and the counters of patch 0880 count direct requests too.
  `pw_wineserver_call_direct` is a new `wineserver.prx` export; `WINE_PS5_SERVER_DIRECT=0` keeps every request on the
  pipes. Requests on the calling thread: `select`, `event_op`, `release_mutex`, `release_semaphore`, the create and
  query requests of events, mutexes, semaphores and keyed events, `close_handle`, `dup_handle`, `create_file`,
  `get_handle_fd`.
- **Console, same build, on against off:**

  | µs per operation (`sync-probe.exe`) | pipes | calling thread |
  | --- | ---: | ---: |
  | event ping-pong round trip | 37.5 | 17.6 |
  | mutex take + release | 31.6 | 7.1 |
  | semaphore release + wait | 31.4 | 7.1 |
  | SetEvent + ResetEvent | 29.5 | 6.8 |
  | wait on a signalled event | 16.7 | 3.7 |
  | wait-any / wait-all of 4 | 16.8 / 16.7 | 3.7 / 3.8 |

  Every semantic check passes both ways. The .NET game (profile 1, 50 s): server requests 4632 against 4646 a second,
  average 9.6 against 16.5 µs (about 44 against 77 ms of server time a second); `select` 3.3 against 16.3 µs;
  first frame 8.28 against 8.38 s and 59.9 FPS either way (the game is not server-bound). ntdll's whole conformance
  set with it on: 23 files complete, `exception` and `virtual` incomplete as before, no unexplained failure.
- **Left on the pipes:** `remove_completion` (about 900 a second at 15 µs in the .NET game), the in-process HID driver's
  `get_next_device_request` and `set_queue_mask`, and input (`send_hardware_message`).
- **Disposition:** on by default.
- **Completion ports (patch `0919`):** `add_completion`, `remove_completion`, `get_thread_completion` and
  `query_completion` join the list: they move messages between a port's queue and its waiters, wake waiters as
  `event_op` does, never sleep in the server and carry no application buffer. The .NET game (.NET's thread pool runs on a
  port): `remove_completion` leaves the top requests, the average request falls from 9.6 to 7.25 µs (16.5 with
  everything on the pipes), about 34 ms of server time a second against 44 with 0918 alone and 77 without either;
  first frame 8.27 s, 59.9 FPS, clean close. Completion ports in the conformance files: ntdll `file`, `pipe`, `sync`
  and ws2_32 `afd` give the same lines as before; kernel32 `sync` now completes (344,770 tests, no line Linux lacks),
  where every earlier console run was ended by the watchdog, and leaves the skip list.
- **Next:** R3's device for the objects themselves; the in-process HID driver's requests and input stay on the pipes.

## R7 — the CPUs a game is shown

- **Found (2026-10-04; record removed from the tree; in git history):** Wine took its processor count from `sysconf`, 16 on the console, and
  Java sized its collector and the Java game its chunk workers by it. Sixteen threads sampling where they run, at
  the lowest priority and sleeping between samples, ran only on CPUs 0-12; the kernel's own answer
  (`cpuset_getaffinity`, which takes an 8-byte set here and refuses 32 and 128 with ERANGE) is the same,
  `0x1fff`. (A first version of the sampling spun at the title's priority: under the console's FIFO
  scheduling it never gave the CPUs back and the title froze before Wine started.)
- **Changed:** at each game launch the title asks the kernel for the CPUs the process may run on (sampling
  only if the kernel does not answer) and sets Proton's `WINE_CPU_TOPOLOGY=13:0,...,12`, CPUs by number since
  the console has no `/sys` topology; a game that sets its own keeps it. `PROTON_CPU_REPORT=1` also logs the
  sampling.
- **Measured:** the Java game with the sync device, fresh copies of the same world: 16 shown (2026-10-04; record removed from the tree; in git history) ->
  13 shown (2026-10-04; record removed from the tree; in git history): mean 77.6 -> 86.7 FPS while walking, worst 5 s interval 23.5 -> 52.9 FPS, p95
  frame 14.9 -> 13.3 ms, p99 45.6 -> 17.8 ms. The .NET game (2026-10-04; record removed from the tree; in git history): 59.9 FPS, p99 17.75 ms, a clean
  close, as before.
- **Disposition:** kept. Java still warns "Attempt to get process group affinity failed" (about 500 times a
  run): Wine's `GetProcessGroupAffinity` is a stub; Java then uses the processor count, now 13.
