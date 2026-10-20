# Filesystem elevation and lifecycle validation

Reviewed 2026-10-07. The title uses the platform client and a pinned Lapy owned-root
daemon for access to `/data` and elevated prefix ownership. It no longer uses the
legacy elevator. kstuff-only operation was tested; etaHEN is not required.

## Production startup

The request is made before threads/devices that conflict with the daemon's
preflight. Visible `/data` alone is not enough: a mounting payload may expose it
without changing user identity, while the existing prefix belongs to the elevated
user. Current startup requests elevation unless it is already root.

If no daemon reads the request, the title posts its pinned resident ELF to the
local websrv/elfldr, then restarts/retries through its launch path. A daemon that
has already accepted a request is not duplicated. The service has no automatic
boot hook and does not survive a console restart.

If access cannot be obtained, the launcher reports the library unavailable.
It must not mislabel an inaccessible game directory as an empty library.

## Measured results

| Checkpoint | Result |
| --- | --- |
| Kstuff-only lifecycle / 2026-10-04 | Six complete runs, 11 elevations, 28 disposable donors reaped, root counters restored to 55/54, no captured faults |
| Resident service / 2026-10-04 | Ordinary library launches granted filesystem access with counters at the service baseline |
| Automatic service start / 2026-10-06 | First library drawn 4.9 s after exec; next launch reused service and obtained access in 0.51 s |
| Later first-boot startup / 2026-10-06 | Daemon-starting title restarted once; replacement logged successful elevation |

Finite validation is not indefinite stability. Original failed donor-release
experiments, exact hashes, log/verdict rules and measurements are in
[the elevation history](history/2026-10-07/docs/ELEVATION_VALIDATION.md).

## Tools and boundaries

[platform/lapy/README.md](../platform/lapy/README.md) describes the pinned patches
and builder. Normal launches let the title start the service. A manual
`tools/start-lapy-service.py` run needs its own idle/no-duplicate checks; do not
start another daemon because a title already started one.

The launch cap was removed at the user's request. Tools retain pacing and the
post-disconnect guard; see [CONSOLE_DROPS.md](CONSOLE_DROPS.md).
Never treat elevation alone as proof that private system imports, raw spawning
or GPU initialization are safe. Those contracts have separate evidence.
