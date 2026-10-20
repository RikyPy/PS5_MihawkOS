> Historical snapshot of `docs/CONSOLE_QUEUE.md`, archived on 2026-10-07 from `9b07ebb`.
> Defaults, next steps, counts and failure classifications below belong to their original runs.
> Use the [current document](../../../CONSOLE_QUEUE.md) for present guidance; see the [archive index](../README.md).

# What waits for the console (2026-10-06)

Work built and tested on the host since the console stopped answering after
a 2026-10-06 game run (record removed from the tree; in git history) (the title held in its own exit, SceSysCore
stopped: [WINDOWS_PROCESSES.md](WINDOWS_PROCESSES.md)). Each step names its
command and what counts as a pass. Run them in order: each depends on the one
before it. A step that fails stops the queue until it is understood. Mask the
console's address in anything pasted.

## Where it stands (2026-10-06, night)

The runs and their numbers are in [WINDOWS_PROCESSES.md](WINDOWS_PROCESSES.md), "The queue after the restart".

| Step | Result |
| --- | --- |
| 0, 1 | done: restarted, control payload `50b16f8` loaded, `new-boot` recorded, title and runtime deployed together |
| 2 | passed, all four runs |
| 3 | passed: 48 polls of `health`, every 5 s through the input and menu runs, all answered `busy=none` in 7 to 27 ms |
| 4 | passed: scripted presses with and without a reconnect, and a person who loaded the save and walked (`pad-04`, `-05`). The runs where a person's presses had no effect are explained (WINDOWS_PROCESSES.md, "The controller in a helper: no program found it"): a link wineboot's DualSense left kept the game's from being enabled. The fix, patch 0941, passed a 2026-10-06 run (record removed from the tree; in git history): DirectInput read every report, and the person at the console said everything works |
| 4b | passed: the script waited for the broker's first flip (13.57 s), and the broker took every capture |
| 5 | passed with helper audio enabled by default: `manual-default-audio-2026-10-06-02`; non-silent 48 kHz stereo logged and music heard by the person at the console |
| 6 | passed: a .NET (MonoGame) game and D3D11 with no Zink override; loading display handed off at first presentation. See the manual-default evidence below |

## 0. The console

1. Restart the console, load kstuff, then elfldr. The title starts its own Lapy
   daemon, so `tools/start-lapy-service.py` is not used.
2. Load the new control payload (PS5_Vulkan `50b16f8`):
   `python3 ../PS5_Vulkan/tools/ps5_console.py deploy-payload`. Then load it once
   with the console's payload loader. `python3 ../PS5_Vulkan/tools/ps5_console.py
   payload` must answer `ping`, `health` (`busy=none` once the worker's `init`
   is done) and `status`.
3. Only once the restart is confirmed: `python3 tools/console_guard.py new-boot`.

## 1. Deploy everything together

The bootstrap protocol is now `0x50573507` (`e429581`), so the title folder and
the runtime must not be mixed with older ones.

1. `bash tools/build-production.sh`
2. `bash ps5/tools/deploy.sh`, then `python3 tools/deploy-wine.py`. The new size
   check (`f9d30ce`) reports files changed on the console since the last
   deploy. Several are expected after this session's manual uploads.

## 2. Nothing regressed

| Run | Command | Pass |
| --- | --- | --- |
| a probe | `python3 tools/run-console.py --exe wine-probe.exe` | PASS, and back to the launcher |
| D3D11 in a helper | `python3 tools/run-console.py --exe d3d11-probe.exe` | PASS |
| OpenGL in a helper | `python3 tools/run-mesa-zink-probe.py --name zink-sprite-default-<date>-02 --exe gl-sprite-probe.exe --env PROTON_ZINK=1` | PASS, 8 of 8 draws, 60 frames |
| a game to its title screen | `python3 tools/run-console.py --profile <game> --seconds 90 --env PROTON_ZINK=1 --env PROTON_GPU_CAPTURE=1 --env PROTON_GPU_CAPTURE_EVERY=600` | captures: splash, intro, title screen. Passes with the new rule (`f9d30ce`) when the game closes on request |

## 3. The new control payload

While a run is going, `python3 ../PS5_Vulkan/tools/ps5_console.py payload`
answers at once. `health` shows `busy=procs for 0` or `busy=none`, never
silence.

## 4. Controller input in a helper (`85204c6`)

A scripted controller run of a game with its saves held aside and checked by hash (the game-specific script, save
tool and command were removed from the tree; in git history). Pass: the captures show the load menu, then the game
world, then the player moving. If not, the same run with `--env SDL_XINPUT_ENABLED=1` tells raw HID from XInput.

## 4b. Pad-script clock and pictures in the launcher layout (PS5_Vulkan `2d414b1`)

The run in step 4 again, with a frame-anchored pad script (with captures) in place of the launcher variant. Pass: the
log says `pad script: first frame N s after the start`, the presses land as in the title layout's run, and
`klog/<run>-NN.png` show the load menu and the in-game date.

## 5–6. Production graphics and audio defaults

Completed in `manual-default-audio-2026-10-06-02`: a .NET (MonoGame) game used no
`PROTON_ZINK` or `WINE_PS5_HELPER_AUDIO` override. The title screen rendered
correctly, non-silent stereo reached AudioOut at 48 kHz, and the person at the
console heard music. The loading display stayed up for 16.2 s until the
presenter opened VideoOut, then released its memory. The game closed on
request, the launcher ended with status 0, and all three payload helpers
were absent afterward.

`manual-default-audio-d3d11-2026-10-06-02` also passed with no overrides;
loading handed off after 3.2 s and the launcher ended with status 0.
Explicit `PROTON_ZINK=0` and `WINE_PS5_HELPER_AUDIO=0` remain diagnostic
opt-outs. No profile or game-specific switch is required for these defaults.
See [WINDOWS_PROCESSES.md](WINDOWS_PROCESSES.md#manual-launch-defaults-2026-10-06).

## 7. If the title hangs in its exit again

The runner now saves `klog/<run>-after.log` (90 s of klog) and
`klog/<run>-processes.txt` (every process and its wait event) when the control
payload stops answering (`3d738e7`). `python3 tools/console-processes.py` gives
the same list at any time, without the control payload.

## 8. Launch another homebrew after Proton closes

The reported "Can't start the game or app" was reproduced after the game returned
through the launcher: ShadowMount's launch hook had reverted. The initial patched
service passes that sequence and Samples rendering; fresh-boot qualification is
still pending. See [LAUNCH_AFTER_EXIT.md](LAUNCH_AFTER_EXIT.md).
Reaching the home screen and seeing an empty application list do not prove that the
next homebrew can start. Validate both PS-menu closure during a game and a game
returning through the launcher, then launch Samples and require its rendering test
and normal exit. Check that the payload helpers are absent and that ShadowMount
released the title's runtime mount.

Add `--launch-diagnostics` to `tools/run-console.py` for these runs. It saves
control replies, the title's mount folder, the sandbox listing and both ShadowMount
debug logs before and after the run, in ignored `klog/`. This also runs when the
runner fails. Preserve these logs before rebooting: the reboot rotates the mounting
service's log, and earlier `0x80940033` failures had no mount-service log captured.
The option only reads console state and does not change the mounting service or
kstuff.
