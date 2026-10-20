> Historical snapshot of `docs/NEXT_CONSOLE_SESSION.md`, archived on 2026-10-07 from `9b07ebb`.
> Defaults, next steps, counts and failure classifications below belong to their original runs.
> Use the [current document](../../../NEXT_CONSOLE_SESSION.md) for present guidance; see the [archive index](../README.md).

# The next console session

There is no per-boot launch limit (removed at my request on 2026-10-04). Launches stay 60 s apart, a recorded drop
refuses further launches until the console is restarted by hand, and the order below is still safest first: stop at
the first anything odd.

Every boot, before the first launch: the console restarted by hand and its services loaded, then
`tools/console_guard.py new-boot`. The title starts the resident Lapy daemon itself on its first launch (without it
the title sees no `/data` and the library is unavailable); do not also run `tools/start-lapy-service.py`. Deploy what is ready with `tools/stage-wine.py` and
`tools/deploy-wine.py --prefix probe` (files only, no launch).

## Done on 2026-10-04 (morning, before the daemon was resident)

* The thread probe passes (`thread-leak-02`): phase 4, 3,000 threads that wait once, which ran out of kqueues
  before patch 0910.
* ntdll and kernel32 batches with patches 0160, 0161, 0912 and 0913: kernel32 `environ` lost its 12 open lines, and
  the free-memory and power-status lines are gone.
* kernel32 `virtual` after patch 0911: the store buffer litmus test reports no reordering; the file still ends in
  `test_stack_commit` (16 KiB pages).
* A .NET (MonoGame) game by its path: 59.9 FPS, back to the launcher.
* Wine's `d3d12` test (`wt-d3d12-batch`): VKD3D-Proton created its device and swapchains, the program ended after
  about 3 s and the title returned to the launcher, but its standard output was not read back, so there is no result.

## Done on 2026-10-04 (afternoon, resident daemon)

* The game lifecycle of a .NET (MonoGame) game: load, sleep and save, close, relaunch and load, and a 30-minute session with an automatic
  save (`docs/GAME_LIFECYCLE.md`).
* kernel32 `loader` after the prefix refresh (34 header lines left of 50), `process` (patch 0914), ntdll `file` (0915),
  `om` alone (no object-directory lines), and `path`/`drive` after patch 0916 (both complete).
* Wine's `d3d12`: complete, 671 tests, 8 failure lines; `d3d10core` in four slices.

## Next, in order

1. **`d3d11` and `d3d9` in slices** (`--slices`, `--file-timeout 600`), then VKD3D-Proton's own suite in slices.
2. **ws2_32 taken apart**: which `protocol` names need DNS (a console limit) and which do not; `sock`'s option and
   error differences; `afd`'s seven lines.
3. **d3d10core's last slice** with `+seh`: which worker thread runs past its stack, and why only here.
4. **Port prospero-win's direct server calls** (its #341 and #342) and measure the loading of a .NET (MonoGame) game and a Java game.
5. **The Java game** again once the PS5 OpenGL driver releases its renderer mod's buffers (its own repository).

Not on the list: `dxgi` (it switches display modes: last, and alone), a signal-based `NtFlushProcessWriteBuffers`
(not written), and anything that kills a title.
