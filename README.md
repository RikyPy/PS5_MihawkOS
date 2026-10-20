<p align="center">
  <img src="docs/media/mihawkos-banner.jpg" width="100%" alt="The MihawkOS mark over Aero Meadow, the desktop's own wallpaper: a blue sky with soap bubbles above green hills">
</p>

<h1 align="center">MihawkOS</h1>

<p align="center">
  <strong>Windows programs on a PlayStation 5, started from a desktop of their own.</strong><br>
  A homebrew title for consoles that already run homebrew: a Wine runtime underneath, and Mihawk OS, a<br>
  Windows&#8209;style desktop with a taskbar, Start, File Explorer and a web browser, on top.
</p>

<p align="center">
  <a href="https://github.com/mihawk-99/PS5_MihawkOS/actions"><img alt="CI" src="https://img.shields.io/github/actions/workflow/status/mihawk-99/PS5_MihawkOS/ci.yml?branch=main&label=CI&logo=github"></a>
  <img alt="platform: PlayStation 5" src="https://img.shields.io/badge/platform-PlayStation%205-0070D1">
  <img alt="status: experimental" src="https://img.shields.io/badge/status-experimental-orange">
  <a href="LICENSE"><img alt="license: GPL-3.0-or-later" src="https://img.shields.io/badge/license-GPL--3.0--or--later-blue"></a>
</p>

<p align="center">
  <a href="#where-it-stands">Status</a> ·
  <a href="#what-you-need">Requirements</a> ·
  <a href="#set-up-the-console">Set up</a> ·
  <a href="#adding-programs">Add programs</a> ·
  <a href="#controls">Controls</a> ·
  <a href="#troubleshooting">Troubleshooting</a> ·
  <a href="#build-from-source">Build</a> ·
  <a href="#credits-and-licences">Credits</a>
</p>

<p align="center"><sub><code>PPSA99170</code> · firmware 10.01 and 13.40 · 64-bit and 32-bit Windows programs · in development · status of 2026-10-20</sub></p>

---

## What it is

MihawkOS is two things that ship together:

- **A runtime.** Wine with Proton's components (DXVK and VKD3D-Proton for Direct3D, Mesa's Zink for OpenGL), pinned
  from GE-Proton 11-7 and patched so its Unix side runs on the console. Each Windows program runs in a process of its
  own; the title owns the shared Wine server, the GPU broker that draws for every program, the display, the
  controller and the sound mixer.
- **A desktop.** Mihawk OS is a Windows 11-style desktop, built from Microsoft's published design numbers, with no
  proprietary Microsoft code, fonts, icons or artwork; the open-source Microsoft assets it uses are listed under
  [Credits](#credits-and-licences). Its wallpaper, Aero Meadow, its music and its mark are the project's own.

You open MihawkOS from the PS5 home screen (the first time, a short guided setup comes first), choose **Mihawk OS**,
and start a program from its desktop shortcut, as on a PC. A full-screen game takes the screen directly (Game Mode) and the desktop comes back when it ends.

## Where it stands

MihawkOS runs on the console today, on a PS5 Pro (firmware 10.01) and a standard PS5 (firmware 13.40). Prebuilt
releases are on the [Releases](https://github.com/mihawk-99/PS5_MihawkOS/releases) page; you can also build it from a
Linux PC. Every claim below is something the console showed; it is not a compatibility list, and each program needs its own evidence. The proof behind each line, and its
limits, are in [docs/FEATURES.md](docs/FEATURES.md) and [docs/PLAN.md](docs/PLAN.md).

**What the console has shown**

| Area | What runs |
| --- | --- |
| Windows programs | 64-bit programs, and 32-bit ones natively through WoW64; up to eight at once, with their child processes |
| Graphics | Direct3D 11 through DXVK, Direct3D 12 through VKD3D-Proton, OpenGL through Mesa's Zink; Game Mode hands a full-screen game the screen directly |
| Input and sound | The DualSense through XInput, the touchpad as a pointer, USB keyboard and mouse; sound through the title's mixer |
| Network | Name lookups, HTTP and strict HTTPS from Windows programs |
| Desktop | Taskbar, Start with search and pins, File Explorer, a desktop folder view, and a shortcut for each folder in `C:\Games` with one program at its top |
| MihawkBrowser | The desktop's own web browser, on the Chromium Embedded Framework |
| When things go wrong | PS5 system notifications during a session, repeated in the launcher afterwards; a memory guard that closes a runaway program before the console stalls |

**Built and host-tested, not yet fully shown on the console:** the guided first launch (its screens were walked once on
the console, in Japanese, up to Preparing with a simulated install; the nine languages and the music are host-tested
only), installing the runtime from a USB stick or over HTTPS with no PC, the title setting itself up on its first
start, and signed self-updates.

**Not yet, or out of scope:** kernel drivers and anti-cheat, rumble, moving the desktop focus with the D&#8209;pad, and
consoles other than the two tested ones (a PS5 Pro on 10.01, a standard PS5 on 13.40), which are untested.

The last full console battery passed 96 of 96 on both tested consoles on one build (fingerprint `4fd9c7f4f502f44a`,
2026-10-20): `alpha3-base-01` on the standard PS5 (13.40) and `alpha3-pro-01` on the PS5 Pro (10.01). The records are
in `evidence/`.

## What you need

- **A PS5 on a firmware MihawkOS's payloads accept (1.00 to 13.60).** Tested: a PS5 Pro on 10.01 and a standard PS5
  on 13.40; a Slim and other firmwares are untested. The PC setup tool (`tools/console-setup.py`) still stops on a
  firmware other than 10.01; the release's install without a PC does not.
- **A console that already runs homebrew**, with the payloads under [Payloads](#payloads) loaded on every boot, and
  your own copy of **ShadowMountPlus** to mount the title. You set these up yourself, with tools from their own
  authors. This project does not provide, link to or explain a jailbreak, and whether you may modify your console is
  yours to check where you live ([LEGAL.md](LEGAL.md)).
- **An x86_64 Linux PC on the same network** with the [host tools](#host-tools). It builds MihawkOS and installs it
  over FTP.
- **Your own programs and games**, as Windows folders you own. This repository never contains, fetches or shares game
  data, keys or firmware.
- **Space on `/data`.** The runtime and the tools' check prefix take about 2.2 GiB (the runtime about 1.45 GiB, the
  check prefix about 0.75 GiB); the Mihawk OS prefix adds about 1.3 GiB once used, before any game.
- **Time and space on the PC.** A first build downloads well over 1 GB and builds Wine for hours.

> [!IMPORTANT]
> Prebuilt packages are on the [Releases](https://github.com/mihawk-99/PS5_MihawkOS/releases) page. To build MihawkOS
> yourself on a PC, read [Known gaps](#known-gaps) first.

### Host tools

The owner builds on CachyOS (Arch Linux). Other distributions are untested; the names below are the upstream
projects, so look up your distribution's package for each.

- **clang, clang++, lld and LLVM 23.1.3** (`llvm-ar`, `llvm-ranlib`, `llvm-objcopy`, `llvm-config`). The version
  matters: the Lapy daemon the title ships is pinned by SHA-256, and another compiler version gives another file. The
  payload SDK finds LLVM through `llvm-config`: `LLVM_CONFIG` if set, else `llvm-config-21` down to `-15`, else plain
  `llvm-config`. If your system has one of those numbered ones, set `LLVM_CONFIG` to the 23.1.3 one.
- **Build tools:** git, curl, wget, unzip, tar, xz, zstd, patch, make, cmake, ninja, meson, pkg-config, autoconf,
  bison, flex, perl, glslang (`glslangValidator`), a C compiler named `cc` (or one named in `CC`), the zlib headers and
  the FreeType development files.
- **Python 3.12 or later** with Pillow, pefile, fontTools, numpy, mako, MarkupSafe, PyYAML, packaging and
  cryptography.
- **ImageMagick 7.1.2-32 Q16-HDRI with librsvg**, for the Mihawk OS icons. `tools/fetch-all.sh` unpacks Arch Linux's
  package of that release into `.deps/` on Arch-based hosts; elsewhere, put a build of that exact release with
  librsvg on `PATH`.
- **Not needed on the system:** the Windows cross compiler and Wine; `tools/fetch-all.sh` puts the pinned ones in
  `.deps/`.

## Payloads

What the tested console runs alongside MihawkOS. This repository ships none of them; each comes from its own project.

| Payload | Version tested | Port | Source |
| --- | --- | --- | --- |
| elfldr | v0.26 (`bb1e117`) | through websrv | [ps5-payload-dev/elfldr](https://github.com/ps5-payload-dev/elfldr) |
| websrv | not recorded | 8080 | [ps5-payload-dev/websrv](https://github.com/ps5-payload-dev/websrv) |
| ftpsrv | v0.21.1, with its `SELF` command | 2121 | [ps5-payload-dev/ftpsrv](https://github.com/ps5-payload-dev/ftpsrv) |
| klogsrv | not recorded | 3232 | [ps5-payload-dev/klogsrv](https://github.com/ps5-payload-dev/klogsrv) |
| ShadowMountPlus | stock upstream 1.7beta4 (`d7e35e6`) | - | [drakmor/ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus) |

**On every boot, in this order:**

1. Your console's homebrew enabler. The tested console uses kstuff alone; etaHEN is not needed.
2. **elfldr**, then **websrv** (port 8080). Keep elfldr resident while you play: the title starts its `/data` access
   service and every Windows helper through it.
3. **ftpsrv** (port 2121). The PC tools copy the title, the runtime and your programs over FTP.
4. **klogsrv** (port 3232). The setup tool watches the kernel log for storage errors, and stops if the log is gone.
5. Check that **ShadowMountPlus** is running. It mounts `/data/homebrew/PPSA99170` so the title starts from the home
   screen.

> [!WARNING]
> **Never load OnionHEN.** The setup tool stops if the kernel log mentions it. Restart without it.

**Started for you; never start them yourself:** **ps5vkctl** (port 9111), the control service the PC tools talk to,
which the setup tool sends when nothing answers; and **the Lapy daemon**, which the title starts to reach `/data`. Two
copies of either must never run.

MihawkOS does not ship, patch or install ShadowMountPlus. Known limitation: with stock 1.7beta4 on 10.01, other
homebrew may not start after MihawkOS closes ([docs/LAUNCH_AFTER_EXIT.md](docs/LAUNCH_AFTER_EXIT.md)); restart the
console to recover.

## Known gaps

What still stands between a fresh clone and a working console, as of 2026-10-20:

- **The Lapy daemon rebuilds byte-identically only with clang and LLD 23.1.3.** It is pinned at
  `5651f2ade61731a302e2a630344e6bd893a33f6503c90237d0abed699364bd7f` (payload SDK v0.42), and the clean room of
  2026-10-20 rebuilt it from source to exactly that file; another compiler version gives another file, which
  `tools/build-production.sh` refuses.
- **ImageMagick outside Arch-based hosts**: no pinned build with librsvg runs there yet.
- **The setup tool's own Mihawk OS prefix session (S9) has never run**, and no apply run has yet ended *ready*.

## Set up the console

Steps 0 to 6 are done once. After that, see [Every time you play](#every-time-you-play).

### 0. Get the sources

Clone these next to each other. The build reads PS5_Mesa, PS5_PayloadSDK and PS5-Lapy-JB-Daemon at their pinned
revisions with `git archive`, so a plain clone of each default branch is enough for them. PS5_Vulkan is the
exception: its working tree must stay at its pin, because the PC tools import its `tools/`.

```text
<any folder>/
├── PS5_MihawkOS/         this repository
├── PS5_Vulkan/           RADV's link recipe and the console tools; holds .env (step 4)   pin f410e67
├── PS5_Mesa/             Mesa, for RADV and Zink                                       pins 1ba9874, b3588f7
├── PS5_PayloadSDK/       the payload SDK fork                                          pins 611893f, b83202b
└── PS5-Lapy-JB-Daemon/   the /data access daemon                                       pin 5b8397b
```

```sh
git clone https://github.com/mihawk-99/PS5_MihawkOS.git
git clone https://github.com/mihawk-99/PS5_Vulkan.git
git clone https://github.com/mihawk-99/PS5_Mesa.git
git clone https://github.com/mihawk-99/PS5_PayloadSDK.git
git clone https://github.com/mpereiraesaa/PS5-Lapy-JB-Daemon.git
```

`PS5_VKHomebrewUI` (the launcher's UI kit) is read from beside them too, or fetched from GitHub when it is missing.

**PS5_Vulkan's own setup.** The build copies two files PS5_Vulkan builds, `runtime/libc.prx` and
`build/host/ps5-native-tool`, so build them at the pin:

```sh
cd PS5_Vulkan
git checkout f410e67a0241b3e8edcdbcd2a9b285dfc4a412ed
bash tools/setup-native-dependencies.sh
make                                      # stops at its own demo app (ps5_fp_ieee); MihawkOS does not use it
ls runtime/libc.prx build/host/ps5-native-tool
cd ../PS5_MihawkOS
```

**The pinned one-time inputs:**

```sh
bash tools/fetch-all.sh            # downloads, the PRX tool, the Lapy daemon, the host Wine build (hours)
bash tools/fetch-all.sh --check    # changes nothing: each pin's SHA-256 and a HEAD request to each URL
```

It fetches llvm-mingw, the DXVK sources, GE-Proton11-7, the OpenGL SDK, the payload SDK v0.42 and ImageMagick
7.1.2-32, each checked by SHA-256, then builds the PRX converter, the Lapy daemon, the Wine sources at their pins and
the host Wine build. Running it again skips what is there. `PS5_PROTON_CACHE=/path/to/folder` reuses files you
downloaded before when their name and SHA-256 match. `tools/build-production.sh` fetches the remaining pins itself,
so the build needs the network too.

### 1. Prepare the console (once, and after every factory reset)

- **Power Saving:** never enter rest mode. Rest mode unloads the payloads, and the tools then need a real restart.
- **Automatic system updates:** off.
- **Clock:** correct. Secure HTTPS from Windows programs checks certificates against it.

### 2. Load the payloads

In the order under [Payloads](#payloads), then check that ShadowMountPlus is running.

### 3. Build on the PC

```sh
bash tools/build-production.sh --jobs 8
```

This builds the title into `dist/PPSA99170/` and stages the Wine runtime in `.deps/staged/`.

### 4. Tell the tools where the console is

Copy `../PS5_Vulkan/.env.example` to `../PS5_Vulkan/.env` and set `PS5_HOST` to the console's address:

```sh
PS5_HOST=<the console's address>
FTP_PORT=2121
```

The file is git-ignored, and the tools mask the address in what they print. Mask it yourself before pasting
anything.

### 5. Run the setup tool

Close MihawkOS first: the tool never force-closes a title.

```sh
python3 tools/console-setup.py --check   # read only: 0 ready, 2 something to do, 1 blocked
python3 tools/console-setup.py           # apply: 0 ready, 1 stopped
```

It reads the console's state first and does only what is missing, so running it again resumes where it stopped. It
never deletes anything on the console and never launches a game. When the Mihawk OS prefix is missing, it opens one
Mihawk OS session of its own (a 150 s budget; [below](#the-mihawk-os-prefix)).

<details>
<summary>What the setup tool does, step by step</summary>

| Step | What it checks or does |
| --- | --- |
| S0 host | The built title and staged runtime. |
| S1 reach | FTP and websrv answer; a kernel-log stream stays open for the whole run. |
| S2 guard | No recorded console drop and no storage block. |
| S3 control | ps5vkctl answers; sends the pinned build only when nothing listens on its port. No title runs. A firmware other than 10.01 in the Lapy daemon's log stops it here, before the first write. |
| S4 klog | klogsrv's replay: no storage damage, no OnionHEN. |
| S5 salvage | Once per boot, copies the console's logs to `klog/salvage-<boot>/`. |
| S6 dev.conf | Writes `/data/homebrew/dev.conf` if missing; stops if it holds anything else. |
| S7 title | Reads the title back by SHA-256 and sends only the files that differ. |
| S8 runtime | Runs `tools/deploy-wine.py` when the runtime or the tools' check prefix is not current. |
| S9 prefix | Builds the Mihawk OS prefix with one session of its own. |
| S10 read-back | Reads every deployed runtime file back and compares it by SHA-256. |

`--check --deep` also does the S10 read-back without writing; `--until S7` stops after a step. By hand, the title
alone is `bash ps5/tools/deploy.sh` and the runtime alone `python3 tools/deploy-wine.py`; both read back what they send.

</details>

### 6. Open MihawkOS

Choose **MihawkOS** on the home screen. The first open shows the guided setup: choose a language; with the runtime
already installed it says so, then Continue and Done (or **Skip setup** on Welcome). After that, choose **Mihawk OS**
in the launcher. A title folder that did not exist before
needs up to about 45 s before ShadowMountPlus lets it start; a refusal in that window is not a crash.

### Every time you play

1. Cold-boot the console. Never use rest mode.
2. Load the payloads, in the order under [Payloads](#payloads).
3. Only after a real restart, and only if you will use the PC tools: run `python3 tools/console_guard.py new-boot` on
   the PC once the payloads are loaded. It saves the boot's kernel log and checks it for storage damage; until it has
   run, the PC tools refuse to write or launch. Opening MihawkOS from the home screen does not need it.
4. Open MihawkOS.

### After a factory reset

Reinstall the same 10.01 image and redo [step 1](#1-prepare-the-console-once-and-after-every-factory-reset), then
the every-boot routine. If the console's address changed, fix `PS5_HOST` in `../PS5_Vulkan/.env`. The setup tool then
rebuilds the title, the runtime and both prefixes. Your Windows programs, saves and settings were on `/data` and are
gone: copy them again from your own copies.

## What goes in `/data`

- **You put:** your program folders in `ps5proton/prefixes/mihawk/drive_c/Games/`, an optional `games/<id>/game.json`
  per program, and optionally an empty `library-show-all`.
- **The setup tool puts:** the title, `dev.conf`, the runtime, `run/` and the tools' check prefix.
- **MihawkOS writes:** the Mihawk OS prefix, logs, caches and your settings.

```text
/data
├── homebrew/
│   ├── dev.conf                        two lines for the Lapy daemon
│   └── PPSA99170/                      the MihawkOS title
└── ps5proton/
    ├── runtime/ge11-7/                 the Wine runtime and its deploy-manifest.json
    ├── run/proton-payload-child.elf    the loader every Windows helper starts from
    ├── prefixes/
    │   ├── mihawk/                     Mihawk OS: the desktop and every program
    │   │   └── drive_c/Games/<Name>/   your Windows programs, C:\Games\<Name>
    │   └── probe/                      a separate prefix the tools use for checks
    ├── games/<id>/game.json            the library, one entry per program
    ├── logs/                           every log the title and Wine write
    ├── caches/                         shader caches
    ├── controls.txt                    mouse-button choices
    └── game-mode.txt                   Game Mode on or off
```

<details>
<summary>Every path, and who writes it</summary>

| Path | What it holds | Written by |
| --- | --- | --- |
| `homebrew/PPSA99170/` | `eboot.bin`, `sce_sys/` (icon, `param.json`), `sce_module/`, assets, shaders, the pinned Lapy daemon and `runtime-stamp`. Your launcher theme is kept in `hui/theme.txt`; `session-running` is there while a session runs. | Setup (S7); the title saves the theme and the marker |
| `homebrew/dev.conf` | Exactly `DEV_SERVER=127.0.0.1` and `DEV_PORT=8080`, the loopback setting the Lapy daemon needs. | Setup (S6), only if missing |
| `ps5proton/runtime/ge11-7/` | Wine for both architectures, DXVK, VKD3D-Proton, Mesa/Zink, MihawkBrowser, fonts, Wine Mono and Gecko, and its `runtime-stamp`. | Setup (S8) |
| `ps5proton/run/` | `proton-payload-child.elf`, the helper loader. | Setup (S8) |
| `ps5proton/prefixes/mihawk/` | Mihawk OS's Windows: registry, user folders, saves, `C:\Games` and anything installed into `C:\Program Files`. | The console, on the first Mihawk OS session |
| `ps5proton/prefixes/probe/` | The tools' check prefix, where diagnostics and the automated console tests run. | Setup (S8) |
| `ps5proton/games/<id>/game.json` | A library entry; `<id>` is the folder name. | You |
| `ps5proton/logs/` | `title-last.log`, `wine-last.log`, `wine-stdout.txt`, `lapy-service.log`, DXVK logs, and `sessions/0..2`, the last three sessions. | The title |
| `ps5proton/caches/` | Shader caches per launched entry and runtime version. A driver or DXVK change starts a fresh set. | The title |
| `ps5proton/controls.txt`, `game-mode.txt` | Choices made in the launcher's Settings. | The title |
| `ps5proton/library-show-all` | Optional empty file: the launcher lists every entry, not only Mihawk OS. | You |

</details>

`C:` in Mihawk OS is `prefixes/mihawk/drive_c`, and `Z:` is the console's root, so `/data/...` is also `Z:\data\...`.

## The Mihawk OS prefix

A *prefix* is one Windows installation: a registry, a `C:` drive and its user folders. Mihawk OS and every program
share one, `prefixes/mihawk`. The PC never sends it; the console builds it the first time Mihawk OS opens, or the
setup tool's S9 step does it for you.

**By hand:** open MihawkOS (finish or skip the guided setup the first time; when the prefix is missing, its Preparing
step runs this first session itself and ends it), choose **Mihawk OS**, give the first session time (the tools' own fresh-prefix check takes
about 150 s), then **Start → End session**. Do not open a game in this first session: a game started in a brand-new
prefix's first session never got the DualSense; in later sessions it did. The cause is still open.

**Check it:** `python3 tools/console-setup.py --check` shows `S9 prefix: done`. Later launches keep the prefix and
repair any missing or outdated baseline file; your registry, settings and saves are never replaced.

> [!WARNING]
> `prefixes/mihawk` holds every program in `C:\Games`, everything installed, and your saves. Deleting it deletes all of
> them. There is no tested reset procedure yet.

## Adding programs

The launcher shows **Mihawk OS**. Your programs open from shortcuts on its desktop, which the title writes at the start
of each session for every folder in `C:\Games` with one program at its top, and every library entry in its prefix. Your own shortcuts are never
touched.

**The quick way: copy the folder.** Put the program's folder in `/data/ps5proton/prefixes/mihawk/drive_c/Games/<Name>`
(`C:\Games\<Name>`) over FTP on port 2121. If the folder holds one x86-64 or 32-bit `.exe` at its top (uninstallers
named `unins*` and Unity's crash handler are not counted), the next Mihawk OS session puts a shortcut named after the folder
on the desktop.

**With an entry.** A folder whose program is in a subfolder, one with several programs at its top, or a program that
needs arguments or settings needs an entry, `game.json`. Its line in `logs/title-last.log` (`games folder <Name>: ...`)
says which.

```json
{
  "name": "My Program",
  "detail": "Second line in the library",
  "executable": "C:\\Games\\MyProgram\\MyProgram.exe",
  "directory": "C:\\Games\\MyProgram",
  "prefix": "mihawk"
}
```

<details>
<summary>Every key in game.json</summary>

| Key | Meaning |
| --- | --- |
| `name`, `detail` | The two lines shown for the entry. Without `name`, the folder name is used. |
| `executable` | Absolute Windows path, `C:\...` or `Z:\...`. The launcher checks the file is there. |
| `arguments` | One string, split at spaces; double quotes keep words together. |
| `directory` | The *Start in* folder, an absolute Windows path. |
| `prefix` | Prefix folder id, up to 64 letters, digits, `_` or `-`. Defaults to `mihawk`. |
| `env` | A list of `"NAME=VALUE"` strings, such as `"WINE_PS5_OPENGL=1"` for an OpenGL program. Words starting `PROTON_` or `MIHAWK_`, and `WINE_PS5_NTSYNC`, belong to the title and are not applied when a program opens from the desktop. |
| `desktop` | `false` leaves the program off the desktop. |

</details>

Send the folder and its entry together, resumably and with every file read back:

```sh
python3 tools/deploy-folder.py --source /path/to/MyProgram --folder MyProgram \
    --id myprogram --game-json myprogram.json
```

`--dry-run` previews; `--folder` and `--id` take only letters, digits, `-` and `_`. An entry with a problem gets no
desktop shortcut, and its reason is in `logs/title-last.log` (`game <id> (<name>): <reason>`). Only x86 programs are
accepted, 64-bit or 32-bit; a 32-bit one runs through WoW64.

**Starting a program from the launcher, without the desktop.** The launcher lists Mihawk OS, and any entry with a
prefix of its own. To list the entries that share Mihawk OS's prefix as well, create an empty file named
`library-show-all` in `/data/ps5proton/`. An entry is a folder `/data/ps5proton/games/<id>/` holding a `game.json`
like the one above, with the program's folder in `C:\Games` as usual. Choose the entry in the launcher, and the program
starts full screen, without the desktop, and returns to the launcher when it ends. The entry's line in
`logs/title-last.log` (`game <id> (<name>): <reason>`) says why one is not listed or cannot start.

## Controls

### The launcher

| Button | Does |
| --- | --- |
| D-pad, Cross | Choose, then open |
| Square | Themes: thirty of them. D-pad previews, Cross applies, Circle returns. Gloss / Skeuomorphic is the default. |
| Triangle | Settings: **Controls** and **Game Mode** |
| R1 | Next piece of MihawkOS's own music |
| Circle | Back |
| Options, held 1 s | Close the launcher |

**Controls** assigns the left, right and middle mouse clicks to a DualSense button: the touchpad, Cross, Circle,
Square, Triangle, L1, R1, L2, R2, L3, R3, a D-pad direction or Options. The default is left click on the touchpad
click; *Restore defaults* brings it back. Choices apply to every program.

**Game Mode** is on by default and applies from the next launch. A full-screen game's frames then go straight to the
screen, and the desktop unloads while it plays and returns when it ends. If a game's screen stays black, turn Game
Mode off: every program is then shown inside the desktop.

### The Mihawk OS desktop

- **Touchpad** is a trackpad: move to point, click to left-click, click with two fingers to right-click, hold the
  click and move to drag.
- **USB keyboard and mouse** work, through a USB-C hub in the tests; one plugged in later is found within 3 s.
- **Start → End session** returns to the launcher. Use it rather than the PS button: the registry is saved every
  30 s, so a session ended from outside can lose up to 30 s of settings.
- **While a game runs**, Options goes to the game; there is no hold-to-quit inside a session.

## Updating

1. **Close MihawkOS** on the console: *End session*, then hold Options in the launcher. The tools refuse to write
   while a title runs.
2. **Rebuild:** `bash tools/build-production.sh`.
3. **Re-run** `python3 tools/console-setup.py`. It sends only the title files that differ and redeploys the runtime
   only when its manifest changed. Existing prefixes are kept.

## Troubleshooting

| You see | Do this |
| --- | --- |
| *No access to /data: …* under Mihawk OS | The line says why (websrv not answering, `dev.conf` missing, the daemon not answering). Check elfldr and websrv are loaded and `dev.conf` holds exactly its two lines, then reopen. After rest mode, restart the PS5 and load the payloads again. Never start a second Lapy daemon. |
| *The Wine runtime is not installed …* or *… is not the one this MihawkOS build was made with* | Re-run the setup tool: the launcher compares the runtime's `runtime-stamp` with the title folder's copy. |
| *Last session ended: …* | The session stopped for the reason shown; `logs/title-last.log` has the details. |
| A PS5 notification, *MihawkOS: A program could not start …* | With *Restart the PS5*: errno 5, or the boot's pipe memory used up, which only a restart frees. Otherwise `logs/title-last.log` has the helper's failure. |
| A PS5 notification, *MihawkOS: The PS5's memory is nearly full …* | The memory guard asked the program using the most to close, or ended it, before the console swapped itself to a stop. |
| A PS5 notification, *MihawkOS: A program stopped: the GPU did not answer it* | The program's helper gave the GPU up after 15 s without an answer and was ended. `logs/sessions/0/wine-child-*.log` has which. |
| A PS5 notification, *… holds several programs, so it has no shortcut …* | The folder has more than one `.exe` at its top. Write a `game.json` naming the program ([Adding programs](#adding-programs)). |
| *The last session … did not end normally* | It crashed, froze or lost power. Copy `logs/title-last.log` and `logs/wine-last.log` off before the next session moves them to `logs/sessions/1`. |
| The home-screen tile refuses to start right after setup | ShadowMountPlus needs up to about 45 s to register a new title folder. Wait and try again. |
| Launch refused with `0x80940033`, or another homebrew will not start after MihawkOS closes | A known issue with stock ShadowMountPlus 1.7beta4 ([docs/LAUNCH_AFTER_EXIT.md](docs/LAUNCH_AFTER_EXIT.md)). Restart the PS5. |
| A program's shortcut is missing from the desktop | Read its `games folder <Name>` or `game <id>` line in `logs/title-last.log`. |
| No controller in a game, in a brand-new prefix | The known first-session problem. End the session and start the game again. |
| A game's screen stays black | Turn Game Mode off in Settings and relaunch. |
| The setup tool stops | Its message names the step and why; it resumes where it stopped. A storage-damage line blocks every write and launch until a person has checked the console and run `python3 tools/console_guard.py clear-storage "what was checked"`. |
| The console hangs | Wait at least 3 minutes before cutting power. On the next boot, run `new-boot`, then the setup tool, which salvages that session's logs. Leave the console off MihawkOS for the rest of the day. |

**Logs** are in `/data/ps5proton/logs/`: `title-last.log` (the title, written every 2 s), `wine-last.log` and
`wine-stdout.txt` (Wine and the first program's output), `sessions/0`, `1`, `2` (the last three sessions, with each
helper's `wine-child-<pid>.log`) and `lapy-service.log` (the `/data` access service). Mask the console's address
before you paste a log anywhere.

## Build from source

For developers. Read [AGENTS.md](AGENTS.md) first: the rules, the commit gate and the sibling repositories the build
expects ([step 0](#0-get-the-sources)).

```sh
bash tools/fetch-all.sh                         # once: the pinned one-time inputs (hours: host Wine)
bash tools/build-production.sh --jobs 8         # runtimes, title, Mesa, DXVK; staged runtime
bash tools/host-tests.sh                        # host tests (some read the build's .deps/ outputs)
python3 tools/console-setup.py                  # deploy and read back
python3 tools/gate.py run --name <unique-run>   # the console test battery, before a code commit
python3 tools/publication-check.py all          # safe to publish: no addresses, secrets, game data or game names
```

The hooks (`git config core.hooksPath tools/git-hooks`) refuse a code commit that the console battery did not pass on
exactly that build, and run the privacy guard ([tools/privacy_check.py](tools/privacy_check.py)) on every commit.

Where to read next: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) (who owns what, how a program starts),
[docs/PLAN.md](docs/PLAN.md) (what is being built and what is proven),
[docs/WINDOWS_PROCESSES.md](docs/WINDOWS_PROCESSES.md) (Windows programs as payload processes),
[docs/MIHAWK_OS.md](docs/MIHAWK_OS.md) (the desktop session), [docs/MIHAWK_BROWSER.md](docs/MIHAWK_BROWSER.md) (the
browser), [docs/SELF_INSTALL.md](docs/SELF_INSTALL.md) (installing without a PC) and
[docs/FEATURES.md](docs/FEATURES.md) (accomplishments and their proof limits).

## Credits and licences

MihawkOS stands on other people's work. Every upstream is pinned by revision or hash, keeps its own licence, and ships
its licence text beside the binaries (the runtime's `share/licenses/`, the title's `licenses/`). The full list, with
every pin, is [NOTICE.md](NOTICE.md); where the platform knowledge came from is
[docs/PROVENANCE.md](docs/PROVENANCE.md).

| Part | Project | Licence |
| --- | --- | --- |
| PS5 Wine patch series and platform knowledge | [prospero-win](https://github.com/mpereiraesaa/prospero-win) by mpereiraesaa | LGPL-2.1-or-later |
| Wine | [ValveSoftware/wine](https://github.com/ValveSoftware/wine) and wine-staging, pinned from GE-Proton11-7 through [mihawk-99/proton-ge-custom](https://github.com/mihawk-99/proton-ge-custom), a fork of [GloriousEggroll/proton-ge-custom](https://github.com/GloriousEggroll/proton-ge-custom) | LGPL-2.1-or-later |
| Unicode data and default fonts | The GE-Proton11-7 archive's ICU 68 Windows DLLs and font collection (Liberation, Source Han Sans, Noto, Ume and Wine's own) | Unicode/ICU licence; OFL-1.1; GPL-2.0 with font exception; Ume's licence; Apache-2.0 (see [NOTICE.md](NOTICE.md)) |
| Direct3D 8 to 11 | [DXVK](https://github.com/doitsujin/dxvk) by Philip Rebohle and contributors | zlib |
| Direct3D 12 | [VKD3D-Proton](https://github.com/HansKristian-Work/vkd3d-proton) | LGPL-2.1-or-later |
| Title-side OpenGL | [PS5 OpenGL SDK](https://github.com/blackbearreloaded/ps5-opengl) 1.0.0 by BlackBearReloaded, statically linked into the diagnostic title-layout runtime | GPL-3.0-or-later |
| Vulkan driver and OpenGL | Mesa's RADV and Zink, through [PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan) and [PS5_Mesa](https://github.com/mihawk-99/PS5_Mesa) | MIT |
| Media playback | [FFmpeg](https://ffmpeg.org/) 8.1.3, built without GPL or non-free parts, and GE-Proton's winedmo rework | LGPL-2.1-or-later |
| TLS | [GnuTLS](https://gnutls.org/) with [Nettle](https://www.lysator.liu.se/~nisse/nettle/) for Windows programs; [Mbed TLS](https://github.com/Mbed-TLS/mbedtls) for the title's own downloads | LGPL-2.1-or-later; LGPL-3.0-or-later or GPL-2.0-or-later; Apache-2.0 |
| MihawkBrowser's engine | [Chromium Embedded Framework](https://github.com/chromiumembedded/cef) 150, unmodified | BSD-3-Clause, third-party notices in its `CREDITS.html` |
| .NET and HTML for Windows programs | [Wine Mono](https://gitlab.winehq.org/mono/wine-mono) and [Wine Gecko](https://gitlab.winehq.org/wine/wine-gecko) | MIT with component notices; MPL-2.0 |
| Title foundation | [PS5_VulkanTemplate](https://github.com/mihawk-99/PS5_VulkanTemplate), from [SaschaWillems/Vulkan](https://github.com/SaschaWillems/Vulkan) | MIT |
| Platform layer and PRX tools | [PS5_PayloadSDK](https://github.com/mihawk-99/PS5_PayloadSDK) and [ps5-native-app-boilerplate](https://github.com/BlackBearReloaded/ps5-native-app-boilerplate) by BlackBearReloaded | GPL-3.0-or-later |
| `/data` access | [PS5-Lapy-JB-Daemon](https://github.com/mpereiraesaa/PS5-Lapy-JB-Daemon) by Arksama / Team PHU and contributors | MIT |
| Updates and runtime install | [Monocypher](https://monocypher.org/) (signature checks) and [Zstandard](https://github.com/facebook/zstd) (decompression) | BSD-2-Clause; BSD-3-Clause |
| Desktop type and glyphs | Proton UI, derived from [Selawik](https://github.com/microsoft/Selawik); [Fluent System Icons](https://github.com/microsoft/fluentui-system-icons) | OFL-1.1; MIT |

**MihawkOS itself** is free software under the **GNU General Public License v3.0 or later**
([LICENSE](LICENSE), [LICENSING.md](LICENSING.md)); the few files that keep another licence are named there. Patches to
an upstream project keep that project's licence: the Wine patch series is LGPL-2.1-or-later, like Wine. A binary
release comes with its corresponding source: this repository at the same revision, its patch series and the pins that
rebuild every input.

## Legal

MihawkOS is an independent hobby project. It is not affiliated with, sponsored by or endorsed by Sony Interactive
Entertainment, Microsoft, Valve or any game publisher. PlayStation, PS5 and DualSense are trademarks of Sony
Interactive Entertainment Inc.; Windows is a trademark of Microsoft Corporation; Proton is a trademark of Valve
Corporation. Other names belong to their owners and are used only to say what the project runs on and is compatible
with.

Mihawk OS is not Windows: it contains no proprietary Microsoft code, fonts, icons, wallpapers or sounds, and no Windows
logo; the open-source Microsoft assets it uses are credited above. Its
look follows Windows 11 from public references. This repository and every build of it contain no game files, saves,
keys, firmware or exploits. You are responsible for having the right to the programs you run, and for whether
modifying your console is lawful where you live. The full notes are in [LEGAL.md](LEGAL.md).
