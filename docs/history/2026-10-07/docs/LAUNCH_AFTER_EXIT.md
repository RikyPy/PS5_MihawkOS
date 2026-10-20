> Historical snapshot of `docs/LAUNCH_AFTER_EXIT.md`, archived on 2026-10-07 from `9b07ebb`.
> Defaults, next steps, counts and failure classifications below belong to their original runs.
> Use the [current document](../../../LAUNCH_AFTER_EXIT.md) for present guidance; see the [archive index](../README.md).

# Homebrew launches after Proton exits

Investigation on firmware 10.01, 2026-10-06. Branch: `launch-after-exit`.

The failure was reproduced in the mounting service, after a game returned to
Proton's launcher and the launcher closed. Samples then failed to launch with
`0x80940033`. ShadowMountPlus remained alive and its rendezvous socket existed,
but its ShellCore launch hook had reverted to the original instructions. Its
sandbox hook still called the bridge, and the bridge cave still contained code.
Proton's three payload helpers were already absent. No console restart or rest
cycle occurred during these tests.

| Sequence | Result before the mounting-service fix |
| --- | --- |
| User closes Proton from the PS menu after 36 s | Samples launched successfully, reported by the user |
| Shell closure through LncUtilKillApp after 263 s | Samples rendered 120 frames in 3.33 s and exited with status 0 |
| Game returns through LoadExec to the launcher, then launcher closes | Samples refused to launch; launch hook had reverted |

The third sequence is the reproduced failure. An empty application list alone
would have missed it: the ordinary game runner reported successful completion.
The separate follow-up verdict records the launch failure.

## Fix ownership and mechanism

`compat/shadowmount/0001-retain-shellcore-hook-pages.patch` changes ShadowMount's
hook installation, rather than Proton's game exit. On firmware above 8.20,
ShadowMount writes ShellCore instructions through the kernel's physical mapping.
This bypasses the normal VM write path. Reloading a clean file-backed text page
is the suspected explanation for the original instructions returning; pageout
was not directly instrumented.

Before writing hooks, the patch asks kstuff's existing remote-syscall service to
run `mlock` in ShellCore for the bridge and each hook site. It uses the same ABI
as ShadowMount's existing remote `mprotect`. A failed lock aborts installation
before any code is changed. The locked pages remain for ShellCore's lifetime,
because shutdown deliberately leaves an inactive bridge that a thread may still
be executing. Replacement payloads reuse those pages. On 10.01 there are three
16 KiB pages, totaling 48 KiB. Older firmware keeps its existing mdbg write path.

The first patched-service run repeated the failing launcher-return sequence in
113.6 s. Samples then rendered 120 frames in 3.36 s and exited normally, without
restarting ShadowMount between the two titles. Native process capture showed no
remaining Proton helpers. This is a live-service replacement result: the three
pages had already been manually locked during diagnosis. It does not establish
fresh-boot installation or rest-mode behavior.

The exact upstream PR patch also built on `1.7` parent `4cde42a2293756132735d8e4280eb71f089865ee`
and completed a longer sequence in 265.4 s. Samples then rendered 120 frames in
3.32 s and exited with status 0. Both hooks were still installed and payload
PIDs 146/147/148 were absent; ShadowMount was not restarted between titles.
This build's ELF SHA-256 was
`f33af0b11341a71f3a30f2cb265691c6188c8cb6f45abd70854e8496a8d0fd44`.
The source change has 43 added lines across three files. Its guard matches
`sm_remote_process_write` exactly (firmware above 8.20); only 10.01 was tested.
The user explicitly requested publication of this mounting-service fix;
[upstream PR #139](https://github.com/drakmor/ShadowMountPlus/pull/139) contains
only the three ShadowMount files, with reproduction, validation and limits.
Proton's commits remain local on the requested investigation branch.

The upstream host config-test command failed to link
`is_supported_game_title_id` on both unmodified `4cde42a` and the patched source.
The hook patch does not change that test's inputs. Full payload builds passed.

## Build and diagnostics

`python3 tools/build-shadowmount.py --fetch` explicitly fetches two hash-pinned
archives and builds the patched standalone payload. Subsequent builds need no
network. Output is `build/shadowmountplus.elf`, with input/output hashes in
`build/shadowmountplus.manifest.json`. Source, build libraries and raw console
logs remain ignored. This does not build or replace the Proton title.

Use `--launch-diagnostics` with `tools/run-console.py` to preserve control,
mount, sandbox and ShadowMount log state before and after a run. Then run a
bounded rendering test in another homebrew and require its actual frame verdict
and normal exit. Keep the failure logs before rebooting, which rotates the
service's diagnostics. The three original runs and the first fixed run are
recorded in `evidence/launch-after-exit-*.json`, including the longer upstream
validation. The fixed payload is active for this boot. The user's normal boot
loading path has not yet been identified, so next-boot persistence is unproved.
The exact console-tested PR build is also saved locally as
`build/shadowmountplus-upstream-pr.elf` and on the console as
`/data/shadowmount/shadowmountplus-hook-lifetime.elf`, verified by download/hash
comparison. Saving that file does not configure the user's boot loader.

The deployed Proton title differed from the local build artifact; these tests
qualify the mounting-service change against the installed title, not a new
Proton executable. Its deployed title SHA-256 was
`d8887d03dcca3c5ff8aac9cdfec0665e4e1c7f423b165c65182a94d980effafa`;
the payload loader matched the local build, SHA-256
`7e68e76a546734713132870f54d7e12ad50588f75e76a2f2f687c8d60310dc4b`.
