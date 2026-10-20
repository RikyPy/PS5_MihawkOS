/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_UPDATE_H
#define PW_UPDATE_H

/* The switch to a new MihawkOS title (docs/SELF_INSTALL.md, phase 8). The running eboot.bin is never written: the new
 * title is installed whole into a staging folder first, every file checked by its SHA-256 (src/pw_pack.h, a title
 * pack), and moved into the title folder at the next start, before anything else runs, followed by a restart into it
 * through sceSystemServiceLoadExec (rule 2). Under the update folder (/data/ps5proton/update):
 *
 *   stage/       the new title, with its record title-pack.txt (the manifest it was installed from)
 *   ready        written once the stage is whole: the new version; the next start switches
 *   switching    `ready`, renamed as the switch begins: a switch cut short resumes from where it stopped
 *   previous/    every file the switch replaced or removed, kept until the new title has started once
 *   switched     `switching`, renamed once every file is in place: the new version, until pw_update_confirm
 *
 * The switch moves (rename(2), so each file is either the old one or the new one) the new title's files in path
 * order, then moves aside the files the old record lists and the new one does not, then the record, then eboot.bin
 * and sce_sys/param.json last, as the install writes them: ShadowMountPlus never sees half a title. Every file it
 * replaces goes to previous/ first. Only files a record lists are touched: the title folder's own state (markers,
 * settings written beside the eboot) stays. The stage and the title folder are on one file system (/data).
 * shortcut: no automatic rollback; a new title that never draws its launcher is undone by hand (previous/ copied back
 * over the title folder, or an older release's folder copied in): add a start counter if one ever ships broken.
 * tests/test_pw_update.c checks the switch, a switch cut at each step and resumed, and the confirm. */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The record a title pack's install leaves in its root: the manifest, byte for byte (src/pw_pack.h). */
#define PW_UPDATE_RECORD "title-pack.txt"

/* Write `ready` (through a part file, fsync'd, renamed): 0 or -errno. */
int pw_update_mark_ready(const char *update, const char *version);
/* 1 when a switch is due (`ready`) or was cut short (`switching`), with its version; 0 when none is. */
int pw_update_pending(const char *update, char *version, size_t size);
/* The switch, when one is due: 1 when it ended (the caller restarts into the new title), 0 when none was due, or
 * -errno with the path that failed in `failed`. Run again after a failure or a cut, it goes on from where it stopped. */
int pw_update_switch(const char *update, const char *title, char *failed, size_t size);
/* The new title has started once: previous/ and `switched` are removed. 1 when there was a switch to confirm, 0 when
 * not, -errno when previous/ could not be removed. */
int pw_update_confirm(const char *update);

#ifdef __cplusplus
}
#endif

#endif
