# Conformance and regression validation

The validation loop promoted from `win-32bit` to local `main` compares identical PE32 and PE64 Wine tests against a host baseline, then
checks the PS5-specific payload and GPU-broker contracts. Upstream suites find API defects; small probes
isolate a defect; game acceptance proves the complete workload. None substitutes for the others.

## Start here

| Question | Command or source |
| --- | --- |
| Does Windows memory, exception or process behavior differ? | `tools/build-wine-tests.sh`, `tools/run-wine-tests.py` |
| Did a runtime change break production? | `tools/gate.py run`; required cases are defined in `tools/run-battery.py` |
| Can Zink upload large textures and reuse memory? | battery cases `gl-upload32`, `gl-upload64`; `tests/gl-upload-probe.c` |
| Does the full game work? | `tools/run-console.py` with displayed-frame evidence, input/audio checks, exit and relaunch |
| What did older broad suites show? | [historical report](history/2026-10-07/docs/CONFORMANCE.md), `evidence/wine-tests/` legacy records |

## Paired Wine tests

The builder compiles the same source for both architectures: `<dll>_test.exe` and `<dll>_test32.exe`.
It strips debug information before generating `.deps/wine-tests/manifest.json`, so the baseline and
staged console executable have the same digest. Both use the same generated batching/watchdog header.
No test source is hand-edited; the existing build-time Direct3D guards remain diagnostic harness changes.

```sh
bash tools/build-wine-tests.sh ntdll kernel32 kernelbase
# Stage/deploy as part of the production loop, after the build and before console runs.
bash tools/build-production.sh
bash ps5/tools/deploy.sh
python3 tools/deploy-wine.py

# Cheap first slice. Repeat these commands with --arch x86_64.
python3 tools/run-wine-tests.py host ntdll virtual thread --arch i386 --timeout 120
python3 tools/run-wine-tests.py console ntdll virtual thread --arch i386 --per-launch 2 --file-timeout 60
python3 tools/run-wine-tests.py compare ntdll virtual thread --arch i386

# Then processes/pipes/loading, exceptions/WoW64/synchronization, filesystem/registry,
# CRT, networking, audio/input and graphics. Run a failure alone before assigning its cause.
python3 tools/run-wine-tests.py host kernel32 process pipe loader --arch i386
python3 tools/run-wine-tests.py console kernel32 process pipe loader --arch i386 --per-launch 2
python3 tools/run-wine-tests.py compare kernel32 process pipe loader --arch i386
```

Results live in `evidence/wine-tests/<arch>/<dll>-<where>.json`. Each entry carries the test digest,
Wine revision, batching-header digest and architecture. Host entries also record the loader digest and
an ignored raw-log path. The initial payload receives the run's stdout destination; descendants keep their inherited/redirected
Windows handles without reopening that file. `WINETEST_` controls reach payloads too. Console batches record their unique run names; `run-console.py` writes their
individual lifecycle and kernel records. Resuming never reuses another test build's results.

`compare` writes `<dll>-comparison.json`. Missing baselines, mismatched provenance, unfinished/skipped
files and extra failures make its verdict fail. Failures at the same line are compared with multiplicity.
An explicit list after the DLL limits the comparison to that list; its verdict records that scope.
A matched Wine failure is not proof of Windows correctness: Windows results provide an additional
reference where available. A complete test with failures differs from an unfinished test.

Historical `--noproc` records and old line-number allowances are not modern baselines. The child-dependent
`process`, `pipe` and `toolhelp` skips were reopened. Debugger, fiber and threadpool cases retain isolated-
qualification requirements; consult `tests/wine-tests-skip-ps5.json`. Those skips do not count as passes.
A known `OPEN:` defect never becomes a passing limitation. If a genuine platform limitation is requalified,
record it in `<arch>/<dll>-expected.json` with `context` equal to the console entry's context and `tests`
containing the reviewed per-test `file:line` reasons. Mismatched qualifications are refused.

Direct3D tests use native DXVK/VKD3D DLL overrides. The host loader defaults to the pinned runtime;
its headless configuration cannot qualify graphics. Supply `--host-wine` with a display-capable build of
the same revision, and record how it was built. The runner stages production's architecture-matched
DirectX DLLs into its separate graphics prefix and records the loader hash. It no longer silently chooses
system Wine. DXGI host display-mode tests still require the explicit existing opt-in. Console Direct3D
slicing remains available (`count`, then `--slices`); run those exact ranges on the host too.
The comparison refuses different slice ranges. Slicing is a diagnostic subset, not a full-suite pass.

## Texture-upload check

`gl-upload32` and `gl-upload64` use the same source through the production WGL/Zink path. Six
4096×4096 RGBA8 textures remain alive together (384 MiB); uploads are issued without a GPU wait between
them. Two rounds verify five pixels per texture, change and verify a subimage, issue 60 window swaps per
round, then release and recreate all textures. Each upload logs progress and time before/after driver
calls. A missing final verdict remains unfinished; a PASS following a watchdog stop is rejected.
The initial paired run exposed a 32-bit message-processing stall after successful uploads; see
[the analysis](../evidence/conformance-upload-analysis-2026-10-07-01.json). It is not a playable-game pass.

```sh
python3 tools/run-battery.py --name conformance-upload-RUN --only gl-upload32 gl-upload64
```

A probe PASS means 120 pixel checks, 120 swap calls and two release/recreate rounds completed. Compare
its maximum upload/presentation times and the owner's actual displayed-frame counters separately;
API completion is not proof of smooth or complete game rendering. No game configuration or GPU-budget
override is installed by the probe. `gl-upload32` and the focused `input32` case are required in
`GATE_CASES` after the message-stall fix below; their PE64 counterparts remain paired diagnostic cases.

## Keep the loop short

1. Run `python3 tests/test_wine_conformance.py` for parser/verdict changes; `python3 tests/test_wow64_input.py`
   compiles Wine's actual patched input converter on the host. Use host CPU oracles for decoder/interpreter changes.
2. Run the selected Wine groups on the host and console, in both architectures. Preserve skips, crashes and timeouts.
3. Reduce each new difference to one small probe with an explicit oracle.
4. Build production, deploy and pass the entire mandatory gate on exactly the code being committed. Never bypass the hook.
5. At milestones, widen the API suites and validate real games with pixels, sustained presentation, input, audio, closure and relaunch.

Console conditions and pacing remain in [NEXT_CONSOLE_SESSION.md](NEXT_CONSOLE_SESSION.md). `run-console.py`
owns stopping and cleanup; this runner never kills a later app or clears a disconnect guard. Sibling
repositories remain read-only. Commits are pushed only as AGENTS.md rule 10 allows. Full upstream-suite perfection is not a prerequisite
for measured game acceptance, and a successful game does not imply full API conformance.

## First paired foundation measurements (2026-10-07)

| Test | PE32 PS5 | PE64 PS5 | Comparison |
| --- | --- | --- | --- |
| ntdll `thread` | complete, 49 checks, no failure lines | complete, 49 checks, no failure lines | matches each architecture's host result |
| ntdll `virtual` | watchdog timeout, 23 failure lines | complete, 2,498 checks, 47 failure lines (summary counts 49 failures) | PE32 has 21 lines absent from its host baseline and is unfinished; PE64 has 47 lines absent from its host baseline |

Evidence: [PE32 comparison](../evidence/wine-tests/i386/ntdll-comparison.json),
[PE64 comparison](../evidence/wine-tests/x86_64/ntdll-comparison.json). The exact cause of the virtual-memory
failures and PE32 timeout is unqualified. Do not automatically explain all of them by the 16 KiB host pages.
The host PE32 `exception` and `wow64` files also ended incomplete and cannot yet serve as completed baselines.
Host process/pipe/loader files completed in both architectures; their current console qualification is still pending.

The upload probe's
observed readback and timing separated working large uploads from a presentation-loop problem. It did not
establish that a real game is playable.

The repeated upload run on the corrected runner confirmed the distinction:
[PE32](../evidence/conformance-upload-2026-10-07-02-gl-upload32.json) failed after
221.3 seconds with watchdog intervention; [PE64](../evidence/conformance-upload-2026-10-07-02-gl-upload64.json)
passed autonomously in 9.9 seconds. Both eventually verified 120 pixels and issued 120 swaps.
PE32's maximum whole-frame time was 35,803 ms, versus 27 ms for PE64; each architecture's maximum
`SwapBuffers` call was 27 ms. PE32 logged 182,077 unknown-message-type-zero errors. Neither run
recorded a broker allocation refusal. These observations locate the slowdown outside the upload
and swap calls; they do not yet identify the exact faulty message/input crossing.
See [the paired analysis](../evidence/conformance-upload-analysis-2026-10-07-02.json).

Host validation: all 26 sanitizer-enabled C tests and the Python conformance-verdict checks passed.
The C suite required execution outside the sandbox because LeakSanitizer could not inspect processes inside it.

The final production build passed all **13 mandatory gate cases**:
[gate-conformance-2026-10-07-01](../evidence/gate-conformance-2026-10-07-01.json).
Its exact source fingerprint is `da8343761da748b2991f67bb972b397be7e63f65388617d22a878c5a907a12f3`.
Every case recorded return to the launcher, with zero recorded CPU/GPU faults or process-protocol errors.
This covers the working tree, including its pre-existing memory/foundation changes, stdout/control forwarding,
paired-test tooling and stricter probe verdict. The failing upload probe is an optional diagnostic case;
the mandatory gate passing does not erase that failure or qualify the unfinished API files.

## Message-stall fix (2026-10-07)

The focused `input32`/`input64` probe injects the HID driver's internal hardware request without graphics:
20 HID packets with verified bytes and 40 device-change messages with verified parameters.
On the unpatched runtime, [PE32](../evidence/input-message-before-2026-10-07-01-input32.json)
failed at its first reply; [PE64](../evidence/input-message-before-2026-10-07-01-input64.json) passed all 60 checks.

WoW64 converted the outer `send_hardware_input_params` but passed its inner `INPUT32` unchanged.
The union begins at offset 4 in PE32 and offset 8 in PE64: the native reader interpreted the report's
zero `wParam` as its message number. Repeated real HID reports became unknown message zero, filling
the hardware queue. Patch 0955 uses one `input_32to64` helper for the internal call and public `SendInput`,
including mouse, keyboard and hardware fields. This is a shared conversion fix, without game configuration changes.

The patched production build passed all 60 checks in
[PE32](../evidence/input-message-after-2026-10-07-01-input32.json) and
[PE64](../evidence/input-message-after-2026-10-07-01-input64.json).
The [PE32 upload probe](../evidence/input-message-after-2026-10-07-01-gl-upload32.json) then completed naturally
in 11.4 seconds, with 120 pixel checks, 120 swaps, maximum whole-frame time 44 ms and zero unknown-message errors
(previously 221.3 seconds with watchdog intervention and a 35,803 ms maximum frame).
The [PE64 upload probe](../evidence/input-message-after-2026-10-07-01-gl-upload64.json) passed in 10.0 seconds,
maximum frame 27 ms. All 26 host regression tests and the actual converter's host check passed.

The virtual-memory differences remain open. Rerun the unfinished file alone with progress instrumentation,
and qualify process/pipe/loader on the current runtime after game validation and the expanded regression gate.

## Game validation after the fix

The 2026-10-07 fix/game comparison (record removed from the tree; in git history) records
both automated observations and human confirmation:

- A native game's repeat run displayed 4,317 frames. Inspected captures show active gameplay;
  the person confirmed picture, controls and sound. The scripted stop returned to
  the launcher. Its roughly 39-second startup gap remains a separate issue.
- An FNA game's OpenGL repeat run displayed 3,470 frames and progressed into a level.
  The title recorded non-silent stereo submission and normal closure after 80 seconds.
- The same FNA game's DirectX first run finished prefix setup and reached the display stage;
  its captured frame was black. The relaunch failed with status 1 during loading,
  with a native read fault in `libmono-2.0-x86+0x25ec80` at `0xffffffef`.
  This is a failing automated relaunch, not an accepted gameplay test.
- A .NET (MonoGame) game's PE64 control run displayed 4,112 frames, loaded a world and changed the
  character's position. Non-silent audio was submitted and the normal stop returned
  to the launcher, with application status 1 as in previous stop tests.

The person subsequently confirmed that all games run with input, sound and video,
and asked to make the accepted work the local `main` baseline. That report is
recorded alongside the earlier DirectX failure; it does not establish repeatability,
full operation of a game launcher, or completion of a Java game's launcher chain.

The exact production build passed all **15 mandatory gate cases** in
[gate-message-fix-2026-10-07-01](../evidence/gate-message-fix-2026-10-07-01.json),
source fingerprint `f27ba1ac28864daa5a4cc96ba863d16ea334f00df72ee562a52e17fc087d3ed5`. Every case returned to the launcher; the battery
recorded zero CPU/GPU fault records and zero process-protocol errors. The gate
includes the focused PE32 input and large-upload regressions. All 26 sanitizer host
tests, the actual Wine converter check and the conformance-verdict check passed.
The API differences and prior DirectX crash remain open independently of this pass.

### Owned TLS gate without public internet

`launcher-tls` uses `tools/run-tls-fixture.py`: two ephemeral host LAN TLS
endpoints and one marked throwaway prefix. The PE program commits a unique root
to that prefix, checks an HTTPS answer and both certificate chains, and commits
removal of the root; the runner removes its owned prefix and test entry. It
never writes certificates into game or Mihawk prefixes. `tls-probe.exe` without
fixture arguments remains the separate public-network diagnostic.

Hosts with an inbound firewall can select unused permitted ports through
`--trusted-port` / `--untrusted-port`, or `PROTON_TLS_TRUSTED_PORT` /
`PROTON_TLS_UNTRUSTED_PORT` for a whole battery. Default 0 chooses free ephemeral
ports. The runner does not modify firewall policy or disable TLS identity/root
checks. Host IP identities also carry a matching DNS SAN because the pinned
Wine SSL policy checks DNS SAN entries for numeric hosts; this fixture does not
qualify IP-only SAN support. Test validity covers 2000–2040; the existing clock
diagnostic/date-only retry remains explicit in its verdict.
