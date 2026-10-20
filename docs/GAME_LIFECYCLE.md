# Game lifecycle validation

Reviewed 2026-10-07. A complete lifecycle means launch, useful interaction,
save where applicable, normal close, relaunch and reload. Rendering a menu or
returning once is a smaller claim. Preserve user saves and settings throughout.

## Current payload/Zink results

- A .NET (MonoGame) game's manual menu launch showed loading, menu and audible music, confirmed
  by the person at the console. Human controller input was also confirmed.
- A 2026-10-07 run of the same game (record removed from the tree; in git history) captured a world using production Zink defaults,
  handed off loading after 15.8 s, then closed on request and returned to launcher.
  Helper direct backing peaked at 1,024 MiB; system memory peaked at 246 MiB.
- Shell closure with a graphics helper holding 2 GiB left 3/3 recorded native
  helper PIDs absent and no CPU/GPU fault. That verifies a focused cleanup case,
  not a complete save cycle or recovery from every failure.
- A game launcher's latest normal-command-line run stayed up for 120 s and closed normally.
  No complete login/library/game-launch lifecycle has been accepted.

## Earlier game baseline

The 2026-10-04 runs of the same game used the earlier title architecture. They verified
day 1 → 2 save, relaunch/load on day 2, then a 30-minute session and automatic
save to day 3. The session held about 59.9 FPS and returned through the launcher.
All twelve held user-save files were restored byte-identical.

Earlier Vulkan runs of a Java game loaded worlds, rendered and returned to the
launcher. Generated areas reached 113–120 FPS; new-terrain performance was lower.
These runs used direct launch and do not prove today's Java-launcher/payload chain.

The exact scenarios, measurements and first failures are retained in
[the lifecycle history](history/2026-10-07/docs/GAME_LIFECYCLE.md) and
[the historical plan](history/2026-10-07/docs/PLAN.md).

## Tools and next checks

`tools/run-console.py --pad-script FILE` can apply a bounded controller script
and collect captures. Scripts live in `tests/pad-scripts/`. Use passive capture
when checking a person's own interaction, so scripted input is not mistaken for
human input. Owner/broker frame timing and captures must be verified on the actual
build before relying on a script designed for the earlier title path.

Hold user saves aside and check them by hash around a test. Never publish game data, save content or private paths
in evidence. Restore held user state and verify hashes before declaring the test done.

Repeat a game's save/reload and a longer session on the current memory build; then
verify PS-menu closure and an actual launch of another homebrew. Validate a Java game
through its launcher and Java, with real input/sound, normal exit and relaunch.
See [PLAN.md](PLAN.md) and [WINDOWS_PROCESSES.md](WINDOWS_PROCESSES.md).
