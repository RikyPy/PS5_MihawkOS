> Historical snapshot of `docs/ELEVATION_VALIDATION.md`, archived on 2026-10-07 from `9b07ebb`.
> Defaults, next steps, counts and failure classifications below belong to their original runs.
> Use the [current document](../../../ELEVATION_VALIDATION.md) for present guidance; see the [archive index](../README.md).

# Elevation lifecycle validation — 2026-10-04

The user confirmed a kstuff-only boot. The payload identified firmware 10.01.
The launcher startup asset-path fix is deployed: resources use `/app0` before
an elevation grant and the resolved installation path afterwards. Both modes
rendered 300 frames at 59.9 FPS and closed through the title's own exit.

## What was measured

| Capture | Path | Elevations | Donors reaped | Daemon records | Result |
| --- | --- | ---: | ---: | ---: | --- |
| `elevation-repeat-01` | Launcher | 1 | 3 | 27 | PASS |
| `elevation-exit-01` | PE64 immediate exit → launcher | 2 | 5 | 43 | PASS |
| `elevation-exit-02` | Repeat immediate exit → launcher | 2 | 5 | 43 | PASS |
| `compatibility-pe64-01` | FP/AVX signal state, memory, JIT, threads/events | 2 | 5 | 43 | PASS |
| `compatibility-d3d11-01` | 180 D3D11 frames and GPU readback | 2 | 5 | 43 | PASS |
| `compatibility-xinput-01` | Controller 0 present, controller 1 absent | 2 | 5 | 43 | PASS |

All six runs had complete numbered daemon records, `/data` write/read proof,
expected donor wait statuses, successful detach, and post-title root counters
back at **hold=55, use=54**. No fatal-signal or GPU-fault signatures were captured.
The full title captures also have no driver-error signatures. Each launcher
return rendered 300 frames at 59.9 FPS. The fresh 1280×720 D3D11 GPU readback
was visually checked: the expected RGB triangle on a dark background.

The aggregate is [`evidence/elevation-validation-20261004.json`](../../../../evidence/elevation-validation-20261004.json).
Individual evidence files contain build hashes and captured lifecycle records.
Logs and pictures stay in ignored `klog/<run>/`; the final harness also archives
the exact payload ELF and its complete manifest there. The final deterministic
service ELF is `d4948dfb4fdb40cfe9189be3c4b839c0f835b0fae9c2dcdadb03d6b5a469d790`;
the XInput trial exercised that exact artifact. Earlier trials record their own
ELF hashes, before the linker build-ID was made deterministic.

## Startup time and normal launches

A launch from the home screen had no daemon: the validation payloads are finite
and exit with their last title. The title asked for `/data` anyway and waited the
contract's full 10 s (200 polls of 50 ms) on a black screen, because the request
is its first act, before anything is drawn. It then read only its sandbox and
logged "No games installed", though the eight games were still installed.

The daemon picks a request up within 100 ms, and 15 recorded grants finished
0.35-0.41 s after pickup (the client saw `/data` working by 0.51 s). The title now
uses the contract's paths with a 3 s wait (`proton_data_grant`, 60 polls of 50 ms).
An empty list without `/data` is logged as `Library unavailable: /data not granted
(<status>)`, and the screen says the library is unavailable instead of empty.
The harness now records when each startup line reaches the host, counted from the
kernel's `EXEC /app0/eboot.bin` line (klog has no timestamps).

| Run | Daemon | Elevation verdict | Library | UI drawn |
| --- | --- | ---: | --- | ---: |
| `startup-no-daemon-01`, `-02` | none | 3.04 s, 3.05 s (timeout) | unavailable, 0 games | 3.22 s, 3.21 s |
| `startup-daemon-01` | finite one-shot | 0.36 s (ok) | 8 games | 0.54 s |
| `startup-resident-01`, `-02` | resident service | 0.55 s, 0.47 s (ok) | 8 games | 0.73 s, 0.63 s |

The 10 s case is by construction (200 x 50 ms) and was not timed with the new
harness. All five runs rendered 300 frames at 59.9 FPS and ended through the
title's own exit, with no fault records.

The resident service (`--resident` build, ELF `3479be42...`, build ID `7f677846...`)
was started once on firmware 10.01 with my authorization, by
`tools/start-lapy-service.py`. Its console log (41 records, no errors) shows two
requests completed with the client's `/data` proof. Before each request the root
counters equalled the service baseline, hold=74 and use=73, so the first title's
exit returned them. Five disposable donors were reaped with status 9
(`evidence/resident-service-01.json`).

Not proven: the counters after the last title exited (the service samples only
before a request), long uptimes, and anything across a reboot. The service has no
boot hook; since 2026-10-06 the title starts it after a restart when no daemon takes
its request (`platform/lapy/README.md`, "Normal-use service"). A request whose client gives up also ends the
service (`await_client_result` failure); the 3 s wait is six times the slowest
recorded grant.

## Capture and verdict rules

`tools/validate-lapy.py` opens klog before loading a finite payload, saves replay
separately, and captures numbered daemon stdout through websrv `/elfldr`.
`tools/run-launcher.py` adds a random test identity that the title prints in its
results. A replayed title success cannot satisfy that identity. Logs are assembled
by complete lines before UTF-8 decoding and address redaction, including addresses
split across received chunks. An interrupted run retains an incomplete, failing
record. Existing capture directories and evidence are never overwritten.

The daemon verdict requires the expected build ID, contiguous record sequence,
clean layout preflight, exact request count, successful native-root transfers,
all donor exits with expected status 9, each client's `/data` proof, title exit,
and final counters equal to the initial baseline. Finite services wait for the
last title to exit; reaching the request limit alone cannot pass. Any error,
held ownership, incomplete log, counter mismatch or unexpected captured fault
fails the run. The probe verdict must appear in the live capture; the Wine file
is supplementary. Title results, screenshots and kernel diagnostics are also
checked, and successful validation ends with an idle-console check.

`tests/test_lapy_capture.py` accepts complete one-shot and two-request fixtures,
rejects twelve missing/stale/corrupt/failure cases, and checks redaction across
split UTF-8 and address chunks. These are host parser checks, not extra console runs.

## Failures preserved

The earlier `kstuff-lapy-lifecycle-01` and `02` preflights each captured one donor
SIGSEGV at `0x1b0`. Neither launched or elevated Proton. Replacing `pause` with
`usleep` did not fix it: the second trace had SIGUSR1 in the argument register and
faulted during delivery. Removing the user handler and releasing the disposable
donors through native SIGKILL teardown passed. This change applies only to daemon
children; the target title is never force-closed. The root-transfer algorithm is
unchanged. The earlier `03` one-shot success predates the six-run table above.

The daemon patches, their order, licences and dependency pins are documented in
[`platform/lapy/README.md`](../platform/lapy/README.md). Sibling repositories remain
unchanged. No payload or source was published.

## Running the checks

Build both finite payload modes with `tools/build-lapy-validation.py` as documented
in `platform/lapy/README.md`, then, with the console idle:

```sh
python3 -B tests/test_lapy_capture.py
python3 -B tools/validate-lapy.py --name unique-launcher-run
python3 -B tools/validate-lapy.py --name unique-exit-run --probe exit-probe.exe
python3 -B tools/validate-lapy.py --name unique-d3d-run --probe d3d11-probe.exe
```

The normal `ps5/tools/run.sh` single-screen aliases also use the identity-aware
launcher harness. `tools/run-console.py` remains the generic game/performance
runner; its result alone is not complete daemon lifecycle evidence. Only one
console job runs at a time. The launch-count cap is removed as requested;
60-second pacing and the disconnect guard remain. Runs remove their own test
hooks when the title is idle. They never force-close a title.

## What remains unproven

The six-run table covers finite attended trials, not an indefinite service soak.
The resident service started for normal launches (above) has no boot hook, and
only two launches through it are recorded. Existing console-local ps5log configuration was
left unchanged; the mirrored loader stream supplied the measured records.
Klog has no sequence numbering, so a clean capture cannot prove the underlying
kernel transport never dropped a message.

Next coverage is longer repeated sessions, real-game launch/return, gameplay,
save/load, audio listening and remaining graphics/API conformance. This round
adds no claim that all Windows games work, or that every earlier console drop
has been explained.
