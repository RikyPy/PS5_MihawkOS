> Historical snapshot of `ps5/ui/README.md`, archived on 2026-10-07 from `9b07ebb`.
> Defaults, next steps, counts and failure classifications below belong to their original runs.
> Use the [current document](../../../../../ps5/ui/README.md) for present guidance; see the [archive index](../../README.md).

# PS5 Proton's UI module

The launcher uses PS5_VulkanTemplate's Vulkan UI module at
`ca553412935b6c50500e7dbf1329d133604446d4`. The kit itself is exported by
`setup-kit.sh` at PS5_VKHomebrewUI `2188642afbcc8d84d36149124f3d9e39dc1b1a05`.
BlackBearReloaded created the themes, widgets, fonts integration and sounds;
Mihawk's fork supplies the Vulkan backend and template integration.

The default is **Gloss / Skeuomorphic**. SQUARE opens all thirty themes; the main list contains only games. D-pad previews, CROSS applies, CIRCLE returns. The
choice is saved by stable theme id in `hui/theme.txt` under the resolved title
folder, including after `/data` elevation. Unknown or missing ids use Gloss.
Test runs start in Gloss and never overwrite the saved preference.

The library and picker share one device and audio output. A game selection
queues a restart; `main.cpp` dispatches it after the UI and device are destroyed.
Holding OPTIONS for one second closes the title. No Themes or Close row occupies the game list.

`kit.*` and `hui_platform.cpp` are the template module. `ui.cmake` omits its
sample-gallery screens. `template-adaptations.patch` records the changes to the
imported module against the template pin. The vendor export `.deps/hui` stays
unchanged. The platform additions retain Proton's native HID/touchpad fields
and disconnect handling.

This module, the launcher screen and the UI tests are GPL-3.0-or-later. Fonts
retain OFL-1.1 or Bitstream Vera; sounds retain GPL-3.0-or-later. Assets and
licence texts are recorded in `ps5/assets.json` and packaged with the title.
The Wine runtime's source licence is unchanged.

Build: `ps5/tools/build.sh`. Host picture: `ps5/tools/host-reference.sh ps5proton`.
State and picker checks: `python3 tools/test-launcher-ui.py --render` (after the
host build). Console evidence now covers 300-frame Gloss picker rendering in
sandboxed/elevated modes and Wine-probe return to the launcher. See
`docs/ELEVATION_VALIDATION.md`; manual pad navigation, UI sounds and choosing a
real game remain separate checks.
