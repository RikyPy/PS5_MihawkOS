/*
 * MihawkOS - "Check for updates" (docs/SELF_INSTALL.md, phase 8).
 *
 * On request only. A small signed manifest (src/pw_release.h, the owner's Ed25519 key; this build carries the public
 * half) names the release's title pack and runtime manifest by SHA-256. The title pack is downloaded over the title's
 * own TLS (proton_fetch.h) into /data/ps5proton/update/stage, every file checked, and switched in at the next start,
 * before anything else runs, followed by a restart into the new title (src/pw_update.h; rule 2). The previous title's
 * files stay until the new one has drawn its launcher once. The runtime the new title pins is then installed by
 * "Install runtime" (proton_install.h): only the pieces that changed, its stamp last.
 *
 * The address it asks: the title folder's update-source.txt (a PC fixture's, for a rehearsal), else the one the build
 * names (PROTON_UPDATE_URL), which is empty until the owner publishes. No release key, or no address: it says so.
 *
 * Copyright (C) 2026 Mihawk
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PROTON_UPDATE_H
#define PROTON_UPDATE_H

#include <stdint.h>

#include <string>

#include "../../src/pw_pack.h"

struct ProtonUpdate {
	enum Phase { idle, checking, current, available, downloading, ready, failed } phase{ idle };
	std::string version;     /* the release offered, once checked */
	std::string message;     /* for the screen */
	uint64_t download{ 0 };  /* the title pack's bytes to download */
	PwPackProgress progress{};
	double seconds{ 0.0 };   /* since the download started */
};

/* Ask the release address what is newest; returns at once. */
void proton_update_check();
/* Download the release proton_update_check found into the stage; returns at once. */
void proton_update_download();
/* Restart into the staged title now (it switches as that start begins). Returns only when the restart was refused. */
void proton_update_restart();
ProtonUpdate proton_update_state();

/* main.cpp, once /data is granted and before anything reads the title folder: a staged title is switched in, then the
 * title restarts into it. Returns when there was nothing to switch, or the switch failed (the old title goes on, and
 * says why). */
void proton_update_on_start(int argc, char **argv);
/* The launcher drew: the title that runs has started once, so the previous title's files go. */
void proton_update_started();

#endif
