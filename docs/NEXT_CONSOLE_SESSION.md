# Next console session

Current sequence as of 2026-10-07. Production payload spawning, Zink/audio
and application-backed helper memory remain the foundation. The user-selected
`win-32bit` workstream adds paired API tests and input/upload regressions; read
[CONFORMANCE.md](CONFORMANCE.md) and [IA32_DESIGN.md](IA32_DESIGN.md) for the current
measured results. Its mandatory gate now contains 15 cases.

## Before launching

1. Confirm the console is idle and its kstuff, websrv/elfldr and development
   services are ready. The title starts its pinned Lapy service when needed.
2. Read `python3 tools/console_guard.py status`. There is no per-boot launch cap.
   `new-boot` is used only after an actual, confirmed restart, never merely to
   clear a disconnect or because TCP connections succeed.
3. Check for leftover helpers and conflicting tests. Capture logs before a
   reboot, which can rotate diagnostics. Keep addresses in ignored `.env` files.
4. If code changed, build and deploy the matching title/runtime together.
   Preserve game data, saves, profiles and prefixes during deployment.

## Current acceptance sequence

1. **A game launcher's client:** establish a normal-command-line visual baseline
   and compare its UI with equivalent host Wine. Isolate the nonfatal
   DLL-protection warning before claiming complete memory behavior. Then verify
   login, library, a supported game launch, closure and relaunch. Do not disable
   certificate checking or introduce client-specific acceptance bypasses.
2. **A game:** repeat load/play/save/relaunch on today's payload/Zink build.
   Confirm real controller input and audible sound. Test PS-menu close and an
   actual subsequent launch of another homebrew.
3. **Java:** repeat the small ProcessBuilder chain on today's loader, then a Java
   launcher → Java → a Vulkan Java game, input/audio, clean exit and relaunch.
   Earlier direct-launch FPS is only a historical baseline.
4. **32-bit .NET:** qualify x86-required .NET installations through WoW64, with
   their matching native dependencies and backend. Do not change executable
   headers or substitute a PE64 launch to claim this acceptance.
5. **Compatibility:** refresh selected Wine/DirectX tests with process creation
   enabled; investigate differences from matching host baselines.

## Build and mandatory regression gate

```sh
bash tools/build-production.sh
bash ps5/tools/deploy.sh
python3 tools/deploy-wine.py
python3 tools/gate.py run --name <unique-run-name>
```

This sequence deploys to the console. It needs the configured services and a
matching production build. The gate must pass the exact code being committed;
all 15 cases are required. The hook has a documented exemption for notes and
evidence only. Never skip the hook or substitute a previous build's pass.

## Outcomes to record

Record displayed behavior, human audio/input observations, elapsed time,
application/system memory, exit status, native cleanup and follow-up homebrew
launches where relevant. Keep a crash, timeout, shell interruption and ordinary
completion distinct. A successful two-minute launcher lifecycle is not complete
launcher acceptance. Stop console tests if it becomes unresponsive, preserve the
incident, and require a real restart before clearing its drop guard.
