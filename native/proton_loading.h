// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef PROTON_LOADING_H
#define PROTON_LOADING_H
#include <string>
// A loading screen for the seconds before a game shows its first frame. The title draws it on the
// console's display itself, straight through VideoOut (a Java game, a .NET one or a DXVK one shows
// nothing of its own for 8 to 22 s), and closes it when the game's own presenter (Vulkan or OpenGL)
// asks for the display (Wine's release_display callback or the native VideoOut wrapper),
// or when the game ends without one: VideoOut takes
// one owner, and the presenter opens it next. PROTON_LOADING_SCREEN=0 leaves it out.
//
// name: what the screen says is starting; fonts: the folder of the launcher's fonts (Inter).
void proton_loading_start(const char *name, const char *fonts);
// Idempotent and safe from any thread; returns once the display is closed and the screen's memory
// released, with the line it said (empty when nothing was shown). why: for the log.
std::string proton_loading_stop(const char *why);
// The guided setup's first session (src/pw_setup.h, "The first session's cover"): instead of the empty desktop, the
// picture the launcher kept when Start was pressed, with each stage's mark and the time since the start drawn over it
// ten times a second, handed over by show (the GPU broker's set_cover). False, and nothing shown, when the picture is
// missing or not 1920x1080.
bool proton_cover_start(const char *picture, bool (*show)(const void *));
// A Windows process started (its command line, or NULL): the stage it marks (pw_setup_build_next).
void proton_cover_process(const char *command_line);
// The prefix is made: every stage is done and the time stops.
void proton_cover_ready();
// Idempotent; the broker shows no cover once it returns, and the picture is removed. why: for the log.
std::string proton_cover_stop(const char *why);
#endif
