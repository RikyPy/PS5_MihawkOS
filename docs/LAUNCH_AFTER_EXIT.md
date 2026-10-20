# Launching another homebrew after MihawkOS exits

Reviewed 2026-10-07. A reproduced launch-after-exit failure belonged to
ShadowMountPlus's ShellCore launch-hook lifetime. It was separate from the later
payload shared-memory closure panic and system-memory pressure issues.

## Reproduction and fix

On 10.01, game return through LoadExec followed by launcher closure left Samples
unable to launch (`0x80940033`). Proton's recorded helpers were absent; the mounting
service remained alive, but its launch hook had reverted to original instructions.
Direct PS-menu closure did not reproduce that failure in the first tests.

The cause the evidence points to: ShellCore's launch-hook pages are paged out and reloaded with their original
instructions. Keeping those pages resident before installing the hooks made the longer sequence complete on tested
10.01 (Samples then rendered 120 frames and exited with status 0); pageout and reload were not instrumented directly.
MihawkOS does not ship or carry a patched ShadowMountPlus: it waits for an upstream fix, and until then the user
restarts the console when another homebrew refuses to start.

The original reproduction, hashes, baseline host-test failure and earlier
user-authorized upstream publication record remain in
[the launch-after-exit history](history/2026-10-07/docs/LAUNCH_AFTER_EXIT.md).
That past publication does not authorize publishing Proton work now.

## Validation contract

Use `tools/run-console.py --launch-diagnostics` to capture control, mount and hook
state around a game run. Then launch another homebrew and require actual rendered
frames and normal exit. An idle app list or a successful Proton close alone does
not prove the next homebrew will start. Record the mounting-service build and
Proton build separately. Saving a payload file does not configure boot persistence.

The current allocator's live-2-GiB-helper shell-close test passed independently;
see [WINDOWS_PROCESSES.md](WINDOWS_PROCESSES.md). A fresh follow-up-homebrew launch
is still required when accepting changes to mounting or teardown behavior.
