# Approved Mihawk OS icons

The fourteen original coloured objects and thirty-four MIT Fluent command
glyphs are the user's approved design assets. Their editable SVGs and original
provenance records are retained unchanged. `build-mihawk-icons.py` verifies all
recorded SHA-256 hashes before generating anything. It uses ImageMagick
7.1.2-32 Q16-HDRI, rasterizes vectors at 256 pixels, then generates transparent
16/20/24/32/48/64-pixel PNGs and multi-size ICOs. Outputs stay in `.deps/`.

Original artwork is repository-owned artwork supplied and explicitly approved
for reuse by Mihawk, under this repository's LGPL-2.1-or-later terms. It does
not contain extracted Windows icons. Fluent SVGs remain MIT, copyright 2020
Microsoft Corporation, pinned to revision
`cd332ec1f90cda2fad16f86754f8da52f1e9ab30`; its LICENSE and NOTICE accompany
the staged resources. Font and wallpaper follow their separate existing policy.

Patch 0967 uses the object icons at Wine's existing shell32 resource IDs. Wine
retains folder/file associations, navigation and actual Recycle Bin state.
Application-provided icons are unchanged. A resource-only `icons.dll` exposes
the objects plus light/dark command glyphs to shell UI; it runs no code and
imports no libraries. The approval preview is not a runtime dependency.

Appearance tokens live in `../appearance.json`: initial target 1920×1080,
96 logical DPI, text scale 1.0. Colours and metrics come from the approved
design. Tokens alone do not qualify styled controls, a taskbar or materials.
