## What changed

<!-- One topic. What does this change do, and why? -->

## What was proven

<!-- What a run showed, with numbers: host tests, the build, console runs (name the evidence file). Or say
"no console run". Report failures at the same detail as passes. -->

## Checklist

- [ ] `bash tools/host-tests.sh`: all pass
- [ ] `bash tools/build-production.sh`: succeeds (if the change touches anything that runs on the console)
- [ ] `python3 tools/publication-check.py all`: `verdict: clean`
- [ ] Console battery `python3 tools/gate.py run --name <name>`: all cases pass on exactly this build, and
      `evidence/<name>.json` is committed. Or: no console, a maintainer will run it.
- [ ] New logic has a host test; a check every commit needs is in `GATE_CASES`
- [ ] No game data, keys, firmware, addresses or other private information; new files carry an SPDX line; any new
      upstream input is pinned and listed in `NOTICE.md`
