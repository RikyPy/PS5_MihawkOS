# Proton launcher UI

Reviewed 2026-10-07. The UI is imported from PS5_VulkanTemplate
`ca553412935b6c50500e7dbf1329d133604446d4`, using PS5_VKHomebrewUI
`2188642afbcc8d84d36149124f3d9e39dc1b1a05`. BlackBearReloaded created its
themes/widgets/font and sound integration; Mihawk's fork supplies Vulkan/template
integration. Local adaptations are recorded in `template-adaptations.patch`.

## Player behavior

The main menu contains games only. Square opens all thirty themes; D-pad previews,
Cross applies and Circle returns. Gloss / Skeuomorphic is the default. Stable
theme IDs persist in `hui/theme.txt` under the resolved title folder. Missing or
unknown IDs use Gloss. Tests do not overwrite the player's saved preference.
Hold Options for one second to close the launcher.

Triangle opens Settings; its Controls section remaps left/right/middle mouse
clicks. A picker includes Unassigned, the touchpad, face buttons, shoulders,
triggers, stick clicks, D-pad directions and Options. Cross applies, Circle goes
back; Restore defaults is in Controls. Assignments persist by stable button name
in `/data/ps5proton/controls.txt` (the title folder when `/data` is unavailable).
Each button has one mouse assignment and is consumed before XInput/HID reports.
Touchpad movement is independent. Scripted UI previews never save preferences.

A game selection queues a restart after UI/device teardown. A flushed one-shot
request preserves the profile if LoadExec drops arguments. The loading screen
remains until a game presenter opens VideoOut. Normal manual selection of a .NET (MonoGame) game
has user-confirmed loading, menu and music; earlier "choosing a real game is
untested" notes are superseded. Manual checks of every theme and UI sound remain
distinct from that game-launch result.

## Development and licences

`kit.*` and `hui_platform.cpp` come from the template module; `ui.cmake` omits
its sample galleries. The ignored UI export stays unchanged. Proton retains its
native HID/touchpad and disconnect integration.

Build from the repository root with `ps5/tools/build.sh`. Host references use
`ps5/tools/host-reference.sh ps5proton`; UI checks use
`python3 tools/test-launcher-ui.py --render` after the host build. Full production
build and console gate are required for a code commit.

The module, launcher screens and UI tests are GPL-3.0-or-later. Fonts and sounds
retain their component licences. See [ASSETS.md](../ASSETS.md),
[NOTICE.md](../../NOTICE.md) and
[the UI history](../../docs/history/2026-10-07/ps5/ui/README.md).
