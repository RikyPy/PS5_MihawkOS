> Historical snapshot of `wine/patches/README.md`, archived on 2026-10-07 from `9b07ebb`.
> Defaults, next steps, counts and failure classifications below belong to their original runs.
> Use the [current document](../../../../../wine/patches/README.md) for present guidance; see the [archive index](../../README.md).

# The PS5 patch series

Patch 0936 fixes sparse Windows decommit/recommit semantics on larger host pages:
partial host pages retain neighboring contents but newly recommitted Windows
pages must be zero. It passed the synthetic VM probe with 4 KiB and simulated
16 KiB host bookkeeping; console qualification is still pending. The payload's
separate anonymous-reservation adapter is documented in
[`../../docs/WINDOWS_PROCESSES.md`](../../docs/WINDOWS_PROCESSES.md).

Wine's Unix side does not run on this console as it ships. These patches are what
makes it run: the in-process wineserver, the `win32u` user driver that presents
through VideoOut, the winevulkan display plane DXVK and VKD3D-Proton draw into, the
AudioOut driver, XInput on the DualSense, `/data`, direct memory and the console's
16 KiB pages.

**Provenance.** The series is derived from
[prospero-win](https://github.com/mpereiraesaa/prospero-win) (mpereiraesaa,
LGPL-2.1-or-later), whose patches apply to Wine `490f6d5dcbb2a5047345b8af88d114bbcaad69a8`
(wine-mirror/wine). The series started as byte-identical copies; completed rebase
patches now target Proton's Wine while retaining authorship, dates and SPDX headers.
The derivation, rebase and licence are recorded in
[`../../NOTICE.md`](../../../../../NOTICE.md) and [`../../LICENSING.md`](../../../../../LICENSING.md).

**Not imported** — three patches exist only for prospero-win's i386 translator, and
this project runs 64-bit executables only (`docs/PLAN.md`, D1):

| Patch | Why not |
| --- | --- |
| `0601-ntdll-ps5-keep-i386-images-below-4gib.patch` | 32-bit address-space layout |
| `0610-ntdll-ps5-segv-hook-for-the-wow64-cpu.patch` | the translator's fault hook |
| `0710-ntdll-ps5-guest-access-violations-from-the-wow64-cpu.patch` | the translator's fault reporting |

**Where the rebase stands (2026-10-02).** 54 active patches through `0870`.
The original seven OpenGL patches remain in `../deferred-opengl/` as reference;
`0820` adapts their EGL integration to Proton, and `0830` captures presented GL
frames on request. `0690` remains in `../deferred-diagnostics/`. The installed
.NET (MonoGame) game is DesktopGL. D3D11/DXVK remains separately proven by the console probe.
`0840` uses the GPU quad for GL presentation, `0850` supplies the PE audio
loader, and `0860` fixes extensionless Unix-library lookup for XInput.
`0870` runs the Wine HID/PnP stack in-process and exposes native DualSense reports.
`0780`–`0810` add Vulkan capture, fixed-reservation backing, PS5 signal FP state
and presentation scaling. `0750`–`0770` address non-Linux build differences.

Rebasing one, with the tools here:

1. `tools/rebase-next.sh` applies the next patch to `.deps/wine-proton`; if it does not
   apply it leaves the rejections and says so. It refuses a dirty tree rather than
   deleting unfinished work.
2. Place each rejected hunk **by intent**, not by context: Proton renamed things
   (`wine_server_ptr_handle( reply->top_window )` became `reply->top_window`,
   `init_thread_pipe( data )` lost its argument, `make_vulkan`'s stub decision moved out
   of `Function.spec()` into its two generation loops), so the lines around an insertion
   point are often not the lines the patch expects.
3. `tools/rebase-next.sh --commit` commits the resolution with the patch's authorship,
   subject and body, adds the rebase note, and regenerates the patch file in place.
   Rejection files and missing hunks stop it before HEAD advances. A hunk-count
   match is only a guard, not proof that a resolution is correct.

The state is the branch: the number of commits on `ps5-rebase` above the pin is the
number of patches done, and the next patch is that many files into this directory. GNU
`patch -F3` is not a shortcut here: it answers "previously applied" for hunks whose
*surrounding* files are already changed, which is misleading.

**Target.** These patches must apply to the Wine revision the Proton-GE pin carries
(`ValveSoftware/wine`, the `wine` submodule of
[`mihawk-99/proton-ge-custom`](https://github.com/mihawk-99/proton-ge-custom)
`GE-Proton11-7`). `tools/check-patches.sh --write` replays them sequentially in a
temporary tree, stopping at the first conflict (exit 1) and marking later patches
untested. It never modifies the source checkout:
the measured state lives in [`../../docs/PATCH_REBASE.md`](../../docs/PATCH_REBASE.md),
and the areas that carry the most patches (`dlls/win32u`, `dlls/ntdll`, `server/`) are
exactly where Proton's own changes sit.

**Order matters.** The series is applied in file-name order: `01xx` is the server and
ntdll foundation, `04xx` the `win32u` and winevulkan drivers, `05xx`–`07xx` memory,
signals, printing, audio and the OpenGL paths.

**Repeatable host checks.** `python3 tools/test-rebase.py` checks the helpers in
throwaway repositories. `tools/check-wine-host.sh` builds an exported copy of the
committed Wine tree with the in-process server and PS5 user driver enabled. It needs
the system C/autotools toolchain and the exact Vulkan registry 1.4.357 files checked
by SHA-256 in the script (`VULKAN_REGISTRY` can name a local directory). It downloads
nothing and keeps build logs under `.deps/wine-host-check-<revision>/`.

`0928-win32u-ps5-broker-variable-present-extent.patch` disables the ordinary
title's fixed-VideoOut fullscreen blit only for `WINE_PS5_GPU_BROKER=1` in
the native environment. The owner broker scales the guest image instead.
The focused four-renderer/graphics-descendant console evidence and remaining
memory/API boundaries are in `docs/GRAPHICS_PROCESS_BROKER.md`.
