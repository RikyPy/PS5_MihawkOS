# Console disconnects and recovery

Current policy, reviewed 2026-10-07: **there is no per-boot launch cap**.
Launch counts are diagnostic. Tools retain 60-second pacing and block launches
after a recorded disconnect until a real restart is confirmed.

## Recovery contract

1. Stop further launches when the console stops answering or powers off.
2. Preserve available klog, control, mount and native-helper records. A powered-off
   console, a kernel panic and an unresponsive service are different observations.
3. Record the incident with `tools/console_guard.py drop "<brief reason>"`.
4. After the user has actually restarted the console and restored its services,
   use `tools/console_guard.py new-boot`. Successful TCP connection alone is not
   proof of a new boot; never clear the guard to get around it.
5. Confirm idle state and absence of leftover payload helpers before testing again.
6. A klog storage-damage line (`[IOD] process_completion: error`, `bfs_device_* [...] -> errno=5`,
   `[fserr] 0x..,0x..,ssd0`, `ReportCorruptPackageError`, `Decrypt error in SELF block`) fails the run and
   blocks every launch and write until a person runs `tools/console_guard.py clear-storage "what was checked"`;
   `new-boot` keeps the block.
7. `new-boot` saves klogsrv's replay to `klog/boot-<UTC>.log` and scans it. The replay starts minutes after
   boot, so boot-time fsck lines may be missing. `new-boot` is refused while ps5vkctl still answers with this
   boot's pid.
8. A ps5vkctl that answered this boot and is gone, or answers with another pid, counts as a drop.
9. Every deploy (`ps5/tools/deploy.sh`, `deploy-wine.py`, `deploy-folder.py`, `console-setup.py`)
   and the probe tools' uploads (listed in `tools/console_guard.py`) go through `begin_write`/`end_write` (ps5vkctl
   `sync`), and a launch waits 60 s after the last such write. Test runners' setup writes are not bracketed or
   synced: `run-console.py`'s pre-launch control files, the prefix-fresh and launcher-tls fixtures
   (`run-prefix-baseline.py`, `run-tls-fixture.py`) and the other run-* runners. The title is read back by sha256 with SELF off after each deploy and each `new-boot`
   (`tools/verify-title.py`); the gate also compares the runtime's deploy manifest.
10. Runs default to `PROTON_STOP_TERMINATE=0`, which now only turns off the server fallback: a game in helpers
    that does not close is ended by SIGKILL to its helpers.

Prefer the game's WM_CLOSE/normal-exit route. A shell-close experiment needs its
own cleanup verdict; it is not ordinary program completion. Manual recovery and
shell closure are not categorically forbidden, but must stay distinct from a
passing game-run verdict.

## Findings, not blanket explanations

| Incident family | Evidence-backed disposition |
| --- | --- |
| Bare elevated-title `rfork_thread` | User-confirmed kernel panic, no backtrace; direct probe remains disabled |
| Old repeated-elevation drops | Historical launch-count correlation; not proof of a universal five-launch limit |
| Shared pages charged to deleted title | Reproduced shell-close leak/panic; helper-owned shared pages/memory-helper route passed later closure tests |
| Payload system-memory exhaustion | A game launcher previously filled the small system pool; shared application backing and the remaining system-memory guard now have focused validation |
| Lost mounting hooks | A separate ShadowMount launch-after-exit failure, not necessarily a Proton process leak |

The latest gate and live-2-GiB-helper closure passed without a recorded CPU/GPU
fault. Those finite results do not establish indefinite stability or explain
every earlier disconnect. The original incidents and hypotheses are preserved
in [the disconnect history](history/2026-10-07/docs/CONSOLE_DROPS.md),
[process history](history/2026-10-07/docs/WINDOWS_PROCESSES.md) and
[LAUNCH_AFTER_EXIT.md](LAUNCH_AFTER_EXIT.md).
