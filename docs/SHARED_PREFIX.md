# Shared Proton prefix baseline

The shared-prefix work starts from local `main` at `8682902`. Runtime-owned prefix
files now come from one bundle, rather than independent launcher font, Mesa and
HID copies. Console qualification is recorded in [PLAN.md](PLAN.md); packaging
alone is not compatibility evidence.

## Ownership and ordering

`tools/stage-wine.py` builds `runtime/ge11-7/lib/wine/prefix/drive_c/windows`.
Patch 0956 calls `pw_wine_prefix_prepare` in the initial Windows client's ntdll,
after `run_wineboot` finishes and before the application imports its DLLs. This
works for first launches and existing prefixes. Descendants use the same prepared
prefix and retain Wine's ordinary inherited or redirected standard handles.

Each leaf directory has a content-version stamp. The portable installer copies
through temporary files and rename, writing the stamp last. A missing file or
interrupted upgrade is repaired on the next initial launch. A current bundle is
left alone. Failure is reported and prevents application loading; a half-installed
baseline is not silently accepted. Registry, application configuration and saves
are not replaced. Only the bundle's named runtime-owned files are synchronized.

## Contents

- The pinned GE-Proton default font collection: 70 font files, including Arial,
  Times New Roman and Courier New compatible fonts, Tahoma and multilingual
  fallbacks. Font licences accompany the runtime.
- ICU 68's three Windows DLLs for both architectures, satisfying Wine's ICU
  forwarders without program-specific DLL copies.
- DXVK, VKD3D-Proton and foundation-built Mesa/Zink for `system32` and `syswow64`,
  plus the i386 HID drivers under `syswow64/drivers`.
- Wine's built-in Windows libraries remain the default implementation. The
  runtime also packages pinned Wine Mono, both Gecko architectures and the
  certificate bundle for offline discovery/setup.

DirectWrite also needs Wine's Unix FreeType backend, `dwrite.prx`, with both
native and WoW64 call tables. Its PE DLL can discover families and map glyphs
without that backend, but rasterization cannot work. The production builder now
links the upstream backend and exports the additional FreeType entry points it
loads. This is distinct from installing font files.

This is a curated compatibility baseline. Installing native versions of every
Windows library would introduce conflicting DLL overrides and would not implement
missing Windows behavior. A program's own runtimes remain its installation's
responsibility. Wider API conformance and application tests qualify those behaviors.

The launcher supplies a usable default stdout/stderr destination for normal
launches, `logs/wine-stdout.txt`. Only the initial client opens it. Children keep
their inherited/redirected pipes; the default file is not reopened for every child.

## Focused checks

`bash tools/host-tests.sh wine_prefix wine_launch` checks versioned installation,
repair, upgrades and preservation of unrelated state.

`prefix-baseline32-probe.exe` and `prefix-baseline64-probe.exe` check complete
standard-output writes, graphics/ICU imports, a UTF-8 converter, font discovery,
GDI glyphs/bitmap pixels and DirectWrite glyphs/raster pixels. The visible text
window provides separate presentation captures. `tools/run-prefix-baseline.py`
runs either probe in a fresh console-created prefix and removes only its marked
throwaway entry after the console is idle. Use `tools/run-console.py --exe ...`
for the existing probe prefix.

The commit gate includes manual-output writes without a runner override,
redirected child writes and fresh-prefix probes for both architectures, alongside
the existing process, graphics, input and memory checks. Raw logs and pictures
remain ignored under `klog/`; verdicts and package hashes go under `evidence/`.

## Compatibility follow-up

The baseline restored complete output writes in normal launches and added the
DirectWrite Unix raster backend. Real application tests then exposed two separate
platform gaps: native SDK threads entering WoW64's signal handlers, and GDI
windows being composed in paint order rather than Windows stacking order.
Patches 0957/0958 and the `native-signals`/`desktop-overlap` gate cases qualify
those boundaries. The overlap check compares actual display pixels while a
background repaints behind a foreground window, then after that window closes.

The tested application's settings text is now readable, and the first timed response was between
0.5 and 1 second. Initial cursor visibility remains affected by window cropping:
the pointer can be outside the view until moved into it. This is separate from
mouse event delivery, which the scripted settings click demonstrated. See the
current [plan](PLAN.md) for qualified results and remaining failures.
