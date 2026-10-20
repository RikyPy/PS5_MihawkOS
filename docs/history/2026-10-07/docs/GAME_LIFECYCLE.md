> Historical snapshot of `docs/GAME_LIFECYCLE.md`, archived on 2026-10-07 from `9b07ebb`.
> Defaults, next steps, counts and failure classifications below belong to their original runs.
> Use the [current document](../../../GAME_LIFECYCLE.md) for present guidance; see the [archive index](../README.md).

# A game's lifecycle, tested with nobody at the controller

Roadmap step 1 (`docs/PLAN.md`): launch a real game the way the library does, play, save, close, relaunch
and load that save, with the same lifecycle evidence as the probes (the daemon's grant records, frame
pacing, memory, descriptors, the way back to the launcher). A .NET (MonoGame) game is the game.

## The tools

* **A pad script** (`src/pw_pad_script.h`, host test `tests/test_pw_pad_script.c`): a one-shot
  `/data/ps5proton/pad-script.txt` of timed steps (a button held for a time, a stick or trigger position,
  a capture, a stop), with times from the start or from the game's first presented frame. The title
  applies it over the real pad in the game's feed loop, so XInput and the DualSense HID report both carry
  it, and logs every change of what it holds. A capture asks win32u for the next presented frame
  (`WINE_PS5_CAPTURE`) and is numbered. `tools/run-console.py --pad-script FILE` uploads it, fetches the
  pictures as PNG next to the run's log, and always removes the hook.
* **The scripts** were game-specific (a sleep-and-save script, a relaunch-and-load script with a picture of
  the in-game date, a 30-minute session); they were removed from the tree (in git history).
* **The player's saves are never written**: a save-cycle tool (removed from the tree; in git history) hashed
  them, moved them into a holding folder on the console (an FTP rename) and added one test save, a copy with
  its own game id; afterwards it removed the test save, moved the saves back and compared every file with the
  first hashes. A second tool did the same for a Java game's world (a copy played through quick-play; the
  player's world is only hashed).

## What the first runs found (2026-10-04)

Each was a defect a player would have met.

| Found | Cause | Fixed by |
| --- | --- | --- |
| Launching five library entries (four games and the Java test) from the library ended at once ("failed to open") | their `game.json` named `C:\data\...`; `C:` is the game's prefix (`dosdevices` `c:` is `../drive_c`) | the five files corrected to `Z:`; the launcher now checks `C:` executables against the prefix and names the `Z:` path that exists |
| The game saved, but its load menu said "No Saved Games Found" (six day-1 characters had been made this way) | the prefix had no `AppData\Roaming`: `wineboot` makes the user folders and cannot run here, so .NET's `GetFolderPath(ApplicationData)` was empty; the game saved relative to its folder and the load menu, combining that relative folder with the relative names it listed, found nothing | the title makes the 24 standard user folders in every prefix before Wine starts (`pw_wine_launch_profile_dirs`); the six saves and `startup_preferences` moved to the game's `AppData\Roaming` folder, all 12 save files byte-identical |
| The first press on LOAD after the intro is ignored | the title menu's buttons are still animating in | the scripts press again a few seconds later; with the slot selected by default, repeated presses walk title, menu, slot |

## Proven on the console

Firmware 10.01, the resident Lapy daemon, profile 1 launched as the library does:

| Run | What | Result |
| --- | --- | --- |
| 2026-10-04 run 1 | load the test save (Mon 1), out of bed and back in, "Go to sleep for the night?" Yes, "Saving", Tue 2 morning | the save's day 1 -> 2, `millisecondsPlayed` 624 -> 11520, the game's `_old` backups written; first frame 8.3 s, 57.2 FPS, p99 17.75 ms; closed through WM_CLOSE in 122.7 s, registry saved, back to the launcher |
| run 2 | relaunch, load the slot | in-game Tue 2, 6:00 am; 58.65 FPS; closed in 68.5 s |

| run 3 | 30 minutes on the test save: the day runs out, the player character passes out at 2 am, the game saves on its own, Wed 3 begins | 1813 s, closed through WM_CLOSE, back to the launcher; 59.9 FPS, p99 17.75 ms (worst 5 s interval 64 ms); direct memory flat at 991-998 MiB after loading, heap 39.1 MiB, descriptors 465 in 318 of 356 samples (450-488, no trend), no refused allocation; the save's day 2 -> 3, `millisecondsPlayed` 857600 |

The held saves were unchanged after every run, and `restore` put all 12 files back byte-identical. The daemon served 20 requests in this boot, with the root
counters at the service baseline (74/73) before every one, every client's `/data` proof, and no error record
(2026-10-04 (record removed from the tree; in git history), from `tools/lapy-service-log.py`).

## A Java game (2026-10-04)

The world tool copied the player's world on the console and wrote a second argument file whose quick-play names the
copy; a pad script was to walk and turn. The run (record removed from the tree; in git history) loaded the world and drew about 13 FPS, then the JVM stopped
after 45 s for lack of native memory: the OpenGL driver held 9005 MiB of buffers from a renderer mod's chunk arena (see `docs/PLAN.md`, roadmap step 3). The process ended by itself and the
title returned to the launcher in 53 s. The player's world was unchanged by hash and the copy was removed.

## Not proven

Gameplay beyond the first room (tools, other maps), audio by listening, a save across a crash or a
power loss, sessions longer than 30 minutes, and any other game.

## In the launcher layout (2026-10-06)

Since `2586b55` the game runs in a helper process, and three things work differently there:

* A pad script's `anchor frame` never started. The title counted the frames its own Wine presents, and the game
  presents from the helper. A launcher-layout copy of the load script used times from
  the start, about 16 s later than the frame-anchored ones (the first frame came 13-15 s after the start).
* A pad script's captures were not taken. The helper's win32u never sees the game's Vulkan presents. The owner's
  captures (`PROTON_GPU_CAPTURE=1`, `PROTON_GPU_CAPTURE_EVERY=N`) were, as `gpu-child-<cookie>-<n>.ppm` in the
  title's folder.
* Both are built since, not yet run on the console. In the launcher layout, the title also takes the GPU broker's
  first shown frame as the first frame (`ps5_gpu_broker_flips`). A capture asks the broker for the next frame it
  shows, written as `game-frame-NN.ppm` as before (`ps5_gpu_broker_capture_next`), so the frame-anchored script should
  run there unchanged.
* The game's presses do not reach it. Its `game.json` turns SDL's XInput off (`SDL_XINPUT_ENABLED=0`), and a
  helper gets the pad only through XInput (2026-10-06 (record removed from the tree; in git history), [WINDOWS_PROCESSES.md](WINDOWS_PROCESSES.md)).
