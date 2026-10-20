# Engineering handoff

Current checkpoint, 2026-10-07. This replaces the earlier agent-specific handoff;
the [original optimization notes](history/2026-10-07/docs/CLAUDE_OPTIMIZATION_HANDOFF.md)
remain historical reference, including their obsolete priorities and defaults.

## Start here

Read [AGENTS.md](../AGENTS.md), [PLAN.md](PLAN.md) and
[ARCHITECTURE.md](ARCHITECTURE.md). The user-requested branch is
`main`; qualified production code is `9b07ebb`. Push only as
AGENTS.md rule 10 allows. Never modify sibling repositories incidentally, include game
data or clear the console drop guard without a real confirmed restart.

## Working baseline

- Every executable defaults to separate payload Windows processes with one owner
  title/wineserver/GPU broker. Eight simultaneous children passed.
- Mesa/Zink and helper audio are defaults, not test-only switches.
- Helpers/descendants use Sony's application allocator for Windows commits and
  Unix heap: 11,712 MiB shared pool, no kernel budget patch.
- Shared object ownership is outside the title's disappearing budget where needed.
- DualSense/HID, desktop pointer/clicks, DNS/HTTPS and clean closure have evidence.
- The exact-source nine-case gate passed. Re-run it after code changes.

## Open acceptance work

A game launcher's full UI/login/library/game launch; remaining nonfatal DLL-protection warning;
current-build save/reload of a .NET (MonoGame) game; a Java launcher's chain into a Java game; first-launch prefix/Zink
staging; PE64-compatible inputs for an FNA game; refreshed broad conformance baselines.
The person reports unusual launcher UI on host Wine too; compare before assigning cause.

Use [NEXT_CONSOLE_SESSION.md](NEXT_CONSOLE_SESSION.md) for sequencing and
[WINDOWS_PROCESSES.md](WINDOWS_PROCESSES.md) for measurements. Consult the dated
archive for failed routes rather than repeating them. A title cannot safely be
treated as an unrestricted payload; the bare title rfork probe remains disabled.

## Validation and reporting

Build/deploy matching production artifacts, add new required checks to `GATE_CASES`,
and pass the gate before committing code. Keep bounded lifecycle proof distinct
from game acceptance. Report actual pixels, human input/audio, memory, exit and
native cleanup. Preserve failed evidence; raw pictures/logs stay ignored.
