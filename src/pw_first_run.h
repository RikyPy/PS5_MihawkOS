/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_FIRST_RUN_H
#define PW_FIRST_RUN_H

/* The title sets itself up (docs/SELF_INSTALL.md, phase 7). Installing MihawkOS means copying its folder to
 * /data/homebrew; nobody else prepares the console for it. So, on every start and before its first root request, the
 * title writes what that request needs, as the title's own user (uid 1: with ShadowMountPlus mounting /data into the
 * sandbox it can, probe 3, evidence/probe-install-2026-10-18-01):
 * - /data/homebrew/dev.conf, by tools/console-setup.py S6's rule: written only when it is missing, since without it the
 *   Lapy daemon the title starts exits 2 and /data is never granted (a format removes it). One that holds anything
 *   else is the owner's (it may point at tools/ps5log-sink.py on purpose) and is kept as it is;
 * - /data/ps5proton, whose presence decides how the title asks for root (ps5/src/proton_launch.cpp request_root), and
 *   its logs/, where the daemon opens its log before anything else (it exits 2 when it cannot), by mkdir in
 *   ps5/src/main.cpp. The other folders inside it are made after the grant, as root (phase 1).
 * Nothing stays open: the daemon refuses a process with an extra reference to its credential.
 * tests/test_pw_first_run.c checks the rule. */

#ifdef __cplusplus
extern "C" {
#endif

/* The setting S6 writes: the daemon's log endpoint on the loopback (35 bytes). */
#define PW_FIRST_RUN_DEV_CONF "DEV_SERVER=127.0.0.1\nDEV_PORT=8080\n"

enum {
    PW_FIRST_RUN_PRESENT = 0,   /* dev.conf already holds the setting */
    PW_FIRST_RUN_WRITTEN = 1,   /* it was missing and is written (through a part file, fsync'd, renamed) */
    PW_FIRST_RUN_OTHER = 2,     /* it holds something else, kept as it is */
};

/* dev.conf at `path` by S6's rule: one of the values above, or -errno when it could not be read or written. */
int pw_first_run_dev_conf(const char *path);

#ifdef __cplusplus
}
#endif

#endif
