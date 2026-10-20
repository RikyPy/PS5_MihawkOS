> Historical snapshot of `docs/CONSOLE_DROPS.md`, archived on 2026-10-07 from `9b07ebb`.
> Defaults, next steps, counts and failure classifications below belong to their original runs.
> Use the [current document](../../../CONSOLE_DROPS.md) for present guidance; see the [archive index](../README.md).

# When the console drops off the network

Current policy (2026-10-04): the user removed the per-boot launch limit.
Launch counts remain diagnostic; the 60-second cooldown and refusal after a
recorded disconnect remain. The limits described below are historical precautions,
not measured failure thresholds or current requirements to reboot.

The later kstuff-only validation captured six complete runs: 11 elevations,
28 disposable donor exits and root counters restored to 55/54, with no captured
faults. These finite results and the earlier signal-delivery failures are in
[`ELEVATION_VALIDATION.md`](ELEVATION_VALIDATION.md); they do not prove the cause
of every historical disconnect or indefinite stability.

A drop is the console going away mid-session: FTP, klog and the control payload stop answering (ping may or may not),
and it comes back only after a restart by hand and a reload of its services. Every drop is treated as caused by my run,
and no launch follows one until I know why or the console has been restarted.

| When | What was running | What I know |
| --- | --- | --- |
| 2026-10-03, first boot | the GL vertex footprint probe | a version that rendered without presenting a frame is the likely cause; the versions that present, and the transform-feedback-only one, ran safely |
| 2026-10-03 | the end of a `change` test launch | a title teardown (the video output is released) after many launches; nothing was killed |
| 2026-10-03 | kernel32 launches 5 and 6 | after a hung title had been killed with `tools/kill-hung-title.py`; a kill leaves the display live, which has preceded power-offs |
| 2026-10-03 | a pause between kernel32 launches 6 and 7 | after 13 launches since a restart, a normal self-exit and nothing killed; the klog ends at the exit |
| 2026-10-03, third boot | the fourth launch of the boot: kernel32 `virtual` with the page-protection `FlushProcessWriteBuffers` (patch 0907) | about 920,000 `mprotect` calls in 30 s on a page that came from a plain `mmap`; the console dropped after the second run; the title's stack overflow in those runs was `test_stack_commit`, which also ends the title without the patch (and without a drop), so the patch is a suspect, not a proven cause; it is gone |
| 2026-10-04, fifth boot | nothing launched by me: read-only probes (an FTP listing, a klog capture, a control-payload query) | the boot already had 7 launches against it and etaHEN's kstuff was mounting a title from `/data/etaHEN/games`; the console went away between two probes and did not answer again; recorded with `tools/console_guard.py drop` |

| 2026-10-06 | the user's first launch of an FNA game's OpenGL build from the library, by hand | the console restarted. The game is a 32-bit-required .NET program (i386, CLR flags 3; 2026-10-06 installation audit (record removed from the tree; in git history)) and the runtime is x86-64 only. The Lapy grant for the game instance completed (`client_result data_rw=1`); Wine was building the new prefix (wineboot, services, winedevice, rundll32, then `control.exe` installing Wine Mono) when the title process went away, about 14 s in: all seven helper logs end with their owner disconnecting. `wine-last.log` was empty and klog was lost, so whether the 32-bit image or the title's first prefix creation is the cause is not proven. Since then the launcher refuses 32-bit programs (`src/pw_pe_arch.c`), which also covers a 32-bit native game (never launched); recorded with `tools/console_guard.py drop` |

What came out of them, and is in force:

* no title is killed by a test run: every Wine test program ends a stuck test file itself (a 60 s watchdog per file, the
  Direct3D files and the VKD3D-Proton tests have their own), so the title closes through its normal exit;
* `tools/console_guard.py`: after a drop no tool launches until the console has been restarted by hand and `new-boot` is
  run, and a boot gets a capped number of launches (eight at first; five since the likely cause, below, was found);
* an experiment that touches kernel interfaces in a loop (page protection, thousands of calls a second) does not run on
  the console: the idea is first run on the host, in the same code path of a Linux build (`-DPS5_ALERT_PTHREAD` and
  `-DPS5_FLUSH_DELAY` for `__PROSPERO__` in a copy of the pinned Wine), where Wine's own tests judge it;
* the first run of anything new on the console is the smallest one (a probe, then one test file), not the suite.

Unknown: why a launch count matters. The drops come at a title's teardown, which closes the video output; nothing in the
klog says more than that it ended.

## The likely cause, found on 2026-10-04

The fifth drop (the seventh launch of a boot, at the end of the launcher instance, before the game started) came with nothing
that was new: the runtime was the verified one, and the console had run a .NET (MonoGame) game, the thread probe and four test launches
cleanly before it. What every drop has in common is not what was run but how many times the title was started. Every title
instance asks for `/data` with an elevator payload that the title carries (`[sandbox-elevator]` in the klog, firmware 10.01
here), which overwrites the process's root and jail pointers, and the klog shows the kernel complaining each time it exits
(`called fdescfree(), but remain BUDGET_FD_FILE` — a warning any process that exits with a filedesc budget entry produces,
not one special to the elevator: the 2026-01-04 Lapy run produced it when `coredump.elf` exited). The README of the Lapy daemon, which does the same job with balanced root
references, records that very approach, in its legacy form, as "associated with a panic after roughly 20-29 elevations
per boot". A launch is two instances (the title, and the launcher it returns to), so two elevations; the drops came at about
13 launches (26 elevations), and at fewer when the user's own launches by hand before a session are counted. It is a
fit, not a proof: no kernel dump exists.

The guard initially capped a boot at ten elevations, five launches, while that elevator was in use. The way out was the Lapy
daemon, loaded through etaHEN: on 2026-01-04 it ran on this console (firmware 10.01, which upstream had never validated) and
it did not refuse. One launch of `exit-probe.exe` logged `elevation: ok (filesystem access)` and `data: granted` on both
instances, the launcher listed `/data/ps5proton/games` (8 entries), the probe printed
`PS5_PROTON_EXIT_PROBE PASS`, and the title ended itself with `ends: status 0` — 7.2 s, one launch, no drop
(`evidence/lapy-elev-02.json`). The title's legacy elevator is gone from the build, so those elevations no longer happen at
all. At that stage the cap was retained pending the daemon's own reference counters: its log channel wanted a listener, the
console does not reach this host today, so the daemon was given a console-local one and its
`daemon_start`/`target_state`/`target_preflight` events were not captured.
