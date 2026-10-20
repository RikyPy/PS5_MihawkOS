# Pinned DXVK source overlay

DXVK remains on GE-Proton11-7's exact `601930949d111edbbcf9dd463948426d9f8f6ddd`
revision. `pins.json` pins the source and every header/compiler submodule by
revision and archive SHA256. `tools/build-dxvk.py` uses cached inputs only and
builds all five D3D modules for PE32 and PE64 with the existing checksum-pinned
LLVM MinGW toolchain. Missing inputs report their exact public URL and hash.
Sibling repositories remain read-only.

Patch 0001 prepares deferred clears and shader-readable layouts before DXVK's
external blitter accesses ordinary images. The initial symptom was a black
unbound ClearRenderTargetView followed by Present. CopyResource/Map forced
correct pixels, but is diagnostic only. The unmodified pinned release
reproduces black presentation on Linux Wine 11.18 in both architectures.
The normal console gate uses no readback and changes colour after resize and
child exit. No program-specific switch or resource binding is required.

Patch 0002 gives the exported source an explicit patched build identity;
Git discovery must not name the containing PS5_Proton checkout.

The build manifest fingerprints owned inputs and hashes every DLL. Packaging
refuses stale sources or altered DLLs, installs the same modules into both
managed prefix architectures, and includes upstream licence notices.
Console qualification status lives in `docs/PLAN.md` and `evidence/`.
