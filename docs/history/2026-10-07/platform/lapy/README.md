> Historical snapshot of `platform/lapy/README.md`, archived on 2026-10-07 from `9b07ebb`.
> Defaults, next steps, counts and failure classifications below belong to their original runs.
> Use the [current document](../../../../../platform/lapy/README.md) for present guidance; see the [archive index](../../README.md).

# Lapy validation payload

This directory contains patches against PS5-Lapy-JB-Daemon
`5b8397b9f2b5f12a7bc2f9c8745a00d1c2dd01ad` (MIT; see `LICENSE`).
The source is exported into ignored `.deps/lapy-validation`; the sibling
repository is read-only. No legacy elevator is restored.

The applied series is:

1. `stdout-mirror.patch`: mirror numbered ps5log records to the loader's stdout,
   captured through websrv `/elfldr`. The upstream network logging gate remains.
2. `donor-wait.patch`: historical diagnostic experiment replacing `pause` with
   `usleep`. Its comment records an inference that the next run disproved:
   the fault persisted during SIGUSR1 delivery, including while in `usleep`.
3. `donor-release.patch`: remove the user signal handler and release only the
   daemon's disposable donor children with SIGKILL. Accept precisely the expected
   wait status 9. Native teardown still releases the donors' filedesc references;
   target-title shutdown is never forced. The root-transfer code is unchanged.
4. `service-lifecycle.patch`: finite service validation captures an initial
   baseline and waits for the last target to exit and counters to return to that
   baseline. A finite service also times out if its next request never arrives.
   An upstream service-limit result alone did not prove post-title cleanup.
5. `deterministic-build.patch`: select a SHA-1 linker build-ID instead of the
   SDK default random UUID. Two full rebuilds then produced byte-identical ELFs.
6. `resident-log.patch`: the prepared resident mode writes its mirrored diagnostics
   to `/data/ps5proton/logs/lapy-service.log`, so it does not need a PC stdout connection.
   Finite validation modes still use the captured stdout stream.

Build without fetching anything:

```sh
python3 tools/build-lapy-validation.py --sdk-zip /path/to/ps5-payload-sdk-v0.40.zip
python3 tools/build-lapy-validation.py --sdk-zip /path/to/ps5-payload-sdk-v0.40.zip --service-requests 2
```

The builder pins the SDK archive and the read-only prospero-win ps5log header
by SHA-256. The upstream manifest records every source and SDK input hash, build
identity, mode and ELF hash. Keep these payloads as attended validation artifacts;
no unbounded service or automatic boot installation is claimed by these trials.
See `docs/ELEVATION_VALIDATION.md` for measured results and limitations.

## Normal-use service

`--resident` builds an uncapped service restricted to title PPSA99170.
`tools/build-production.sh` checks the build against its pinned ELF hash and ships it
in the title folder as `lapy-root-daemon.elf`. The service has no boot hook and does
not survive a restart; the title starts it (`proton_data_grant`): when its request is
still unread after the 3 s wait, no daemon is running, so it posts the ELF to websrv's
`/elfldr` on the loopback, waits a second (the daemon ignores a request older than its
own start) and asks again, once more if that request also goes unread. A request that
a daemon did read is never followed by a second daemon.

`tools/start-lapy-service.py` starts it by hand: it checks the console is idle,
validates the exact ELF, refuses a duplicate or uncertain start in the same recorded
boot, and waits for the `service_baseline` record in the console log. Since the title
may already have started one, it also requires `--no-launch-this-boot`.

Started by the title on 2026-10-06, firmware 10.01, after a restart with nothing
started by hand (`evidence/autostart-daemon-2026-10-06-01.json`): one `daemon_start`,
one completed request, `client_result data_rw=1`, the library drawn 4.9 s after exec.
The next launch (`-02`) used that daemon: no new start, `/data` at 0.51 s.

Started once on 2026-10-04, firmware 10.01, with my authorization: two ordinary title
launches through it were granted `/data` and listed the game library, and root
counters were at the service baseline before each request. See "Startup time and
normal launches" in `docs/ELEVATION_VALIDATION.md` and `evidence/resident-service-01.json`.
