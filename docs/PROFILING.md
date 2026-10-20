# Profiling a helper process on the console

What a Windows program's process (a "helper": a game or a launcher's client) does, without a debugger. Every
number below comes from the helper's own log (`klog/<run>-wine-child-<pid>.log`, written by `tools/run-console.py`) and was
measured on the console; each tool names the run it was written for. Nothing here samples the title's own process.

| Setting (pass with `--env`) | Where | What it prints |
| --- | --- | --- |
| `WINE_PS5_SAMPLE=1` | `wine/ps5/pw_wine_threads.c`, `native/proton_wine_child.c` | Ten times a second every registered Wine thread is signalled (SIGPROF) and stores the interrupted instruction and stack pointer. Per 10 s: `wine-child: thread tid=… samples=… in_call_stub=… signal_blocked=…` (a sample in the loader's call stubs below `0x800020000` is a thread inside a kernel call; `signal_blocked` is a thread that did not answer within 3 ms, inside a server call or another region that blocks signals), `hot rip=… n=… tid=…` (code running, 64-byte buckets), `wait rip=… rax=… top=…` (the stubs, `top` is the return address), and `nt tid=… call=0x…` (the Windows system call the thread is inside, patch 1113; map the number with `dlls/ntdll/ntsyscalls.h`, `ALL_SYSCALLS64`, the table index is in bits 12 and up). |
| always | patch 1112 | `wine-child: file read|create|open|attributes|directory|information calls=… ms=… slowest_ms=… KiB=…` (totals since the process began). |
| always, when a section view exists | `wine/ps5/pw_wine_section_ps5.c` | `wine-child: section views=… fault_calls=… fault_ms=… mapped=… evicted=… peak=… cached=… map_/flush_/unmap_/reserve_ calls and ms`: the demand-mapped shared-section cache (docs/ARCHITECTURE.md). |
| always | `native/proton_wine_child.c` | `wine-child: census seconds=… slept_ms=… sampler_ms=… other_ms=…` and `census_ms fds=… image_stats=… vm_census=…`: where each 10 s period of the helper's telemetry thread went. `vm_census` was 9 to 16 s per period before the memory walk got a 100 ms budget (`WINE_PS5_CENSUS_MS`, 0 skips the resident count) because `mincore` over a 14 GiB shared section held the address-space lock (2026-10-13; record removed from the tree; in git history). |

The heavy part of the census (descriptor walk, image statistics, mapping walk) runs every 10 s for the first minute and then every
`WINE_PS5_CENSUS_SECONDS` (default 60, at least 10): on every 10 s it showed as a 300 to 660 ms gap in a game that otherwise held 58 fps
(the pacing windows of a 2026-10-13 run; record removed from the tree; in git history). The cheap counters above keep their 10 s period.

`WINE_PS5_SECTION_PAGES` (64 to 65,536, default 4,096 pages of 16 KiB) and `WINE_PS5_SECTION_CLUSTER` (1 to 64, default 1) set the
section cache's size and how many pages one fault may map (patch 1114); the section lines above say what they did.

The thread CPU clock is not used: `pthread_getcpuclockid` returns the caller's own clock on the console.
Reading the profile: a thread whose samples are nearly all `in_call_stub` is waiting; `nt` says in which Windows call; a thread
with many `hot` samples is running, and its `rip` is resolved with the module bases in the log (`payload module=… text=…` for
native modules, `WINEDEBUG=+loaddll` for PE modules).

`tools/profile-report.py <run>` reads a run's helper log and broker log and prints all of this as one report (threads waiting or
running, the code they ran by module, the Windows calls, the telemetry thread's periods, the file and section counters, and the
seconds the game went without a frame); `tests/test_profile_report.py` pins its format to the lines the helper prints.
