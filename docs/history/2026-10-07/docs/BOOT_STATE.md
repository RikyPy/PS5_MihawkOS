> Historical snapshot of `docs/BOOT_STATE.md`, archived on 2026-10-07 from `9b07ebb`.
> Defaults, next steps, counts and failure classifications below belong to their original runs.
> Use the [current document](../../../BOOT_STATE.md) for present guidance; see the [archive index](../README.md).

# What Windows starts at boot, and what the console does without it

On Windows, and on Linux under Wine, a program starts into a system that other processes have already set up:
`wineboot` creates volatile registry keys and the known DLL sections at every start, `services.exe` starts
`winedevice.exe`, which hosts the kernel drivers, and the mount manager (`mountmgr.sys`) is one of them. The
console allows no child processes (patch 0550), so none of this runs. A prefix keeps what `wineboot` stored the first
time (it is made on the host and shipped), and the rest is missing.

## What the missing pieces cost (measured by Wine's own tests)

| Missing | Symptom | Where it shows |
| --- | --- | --- |
| `ActiveComputerName` (volatile, made by `wineboot`) | `GetComputerName` fails, then `LookupAccountSid` and `GetUserProfileDirectory` | kernel32 `environ` |
| DOS device links as server objects (made by the mount manager) | `GetLogicalDrives` returns 0, `QueryDosDevice` fails, `GetDriveType` calls every letter a fixed drive | kernel32 `drive`, `volume`, `file`; ntdll `file` |
| The mount manager's device and its ioctls | `GetVolumeNameForVolumeMountPoint`, `FindFirstVolume`, `\\.\C:` and `\\?\C:` opens fail | kernel32 `volume`, `file` |
| `\KnownDlls` sections (made by `wineboot`) | the headers of a loaded builtin DLL differ from its file | kernel32 `loader` |
| `HARDWARE\DESCRIPTION` and the other hardware keys | a program that reads the CPU's name from the registry finds none | not yet measured |

## How they are given back, smallest first

Each step is a small, general change that does nothing where the missing piece exists, and is checked in the
boot-less host mode: the pinned Wine on Linux, a prefix copied from the console's, its server started without
`wineboot` and no child processes (`libnoproc.so`, tools/noproc.c). That mode reproduces the console's failures
(`environ.c:74`, `drive.c:38`) on the host, which the console cannot do cheaply.

* **Done, patch 0160**: kernelbase reads the stored computer name when the active one is missing. kernel32 `environ`: no
  failures.
* **Done, patch 0161**: `GetLogicalDrives` takes the letters whose root opens when `\DosDevices` lists none. kernel32
  `drive`: 16 failures down to 4, the 4 that need volume names.
* **Next, in ntdll's Unix side**: create the DOS device links as server objects at start (`\??\C:` and `\??\Z:` pointing at
  `\??\unix\...`), from the prefix's `dosdevices` folder, with Nt calls, and the hardware keys from `cpuid`. This gives
  `QueryDosDevice` and the object directory, and makes the probe of patch 0161 unnecessary.
* **Last, and large**: run `mountmgr.sys` in the title: a thread of the process that does what `winedevice.exe` does
  (`ZwLoadDriver` for `MountMgr`, then `wine_ntoskrnl_main_loop`), started at the end of the PE loader's process
  initialization, with `mountmgr.so` built as a PRX. It would give every volume API at once. It touches process
  start-up, so it needs the console and its own round of tests, and it is worth doing only if programs are found that need
  volume names.

A program that needs a child process for real (a launcher that starts the game, an updater) cannot be helped by any of
this: the console has none.

## Findings of 2026-10-04

* **`\KnownDlls` also explains kernel32 `module.c:2052-2078`** (7 lines, filed as "the file name of a loaded DLL
  is not the one the test expects"): the test loads `psapi.dll` with `LOAD_LIBRARY_SEARCH_USER_DIRS` and from the
  application folder and expects the system copy, which a known DLL always is. With no `\KnownDlls` directory
  (ntdll's `open_known_dll_ntdir` finds none) the search finds nothing, or `C:\psapi.dll` beside the test program.
  The way back is wineboot's `create_known_dlls` done by the title's first process at each start (the in-process
  server keeps no permanent objects between launches): the registry list, each DLL's imports, one image section
  each. It costs those sections at every game start and matters to a program that carries its own copy of a
  system DLL; it is not started.
* **The mount manager in the title** has a precedent: patch 0870 already runs `wine_ntoskrnl_main_loop` on a thread
  of the game process and lets named drivers past the service manager. Loading `MountMgr` there also needs
  `mountmgr.so` built as a PRX (it is not one of the console's Unix modules today) and its FreeBSD side to read the
  prefix's `dosdevices`, which the console keeps in `.pw-symlinks` rather than as links. 67 of the 69 DOS-device
  test lines need it (volume GUIDs, `\Device\HarddiskVolumeN`); links alone would fix almost none.
* **Bad pipe and mailslot names** (`pipe.c:193`, `mailslot.c:59`: a relative name is accepted on the console) are
  refused on the pinned Wine on Linux even with `fork()` failing (host `--noproc`, 2026-10-04), so the difference
  is the console's, not only the missing mount manager; finding it needs a server trace on the console.
