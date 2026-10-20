/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_DETAILED_LOGS_H
#define PW_DETAILED_LOGS_H
/* Settings > Logging: one folder of every log a session leaves, for a player to zip and post.
 *
 * Why: asking a player for klog needs a PC running a klog client before the problem happens (the owner, 2026-10-20).
 * With Logging on (default off, saved as logging.txt beside game-mode.txt: "logging on" or "logging off", atomic, a
 * missing or unreadable file means off), each Wine session gets <root>/<YYYY-MM-DD_HH-MM-SS> (UTC: the console gives
 * a title no time zone), filled in two steps:
 *   pw_detailed_logs_begin, at the session's start (native/proton_wine.cpp): prune the oldest folders so that keep - 1
 *     are left, make the new one (a "-2".."-9" suffix when the name is taken), write README.txt (what to zip and post,
 *     what to mask, that program folder names show), and name the folder in <root>/pending;
 *   pw_detailed_logs_pending + pw_detailed_logs_fill, at the next title start after /data is granted (ps5/src/main.cpp),
 *     before any session rotates the logs: the session's own logs are then complete, also after a crash, a freeze or a
 *     power loss, since nothing moves them until the next session starts.
 * Every copy goes through pw_detailed_logs_mask: dotted IPv4 addresses (four groups of 1-3 digits) have their digits
 * replaced by 'x', so the console's address never reaches the folder (AGENTS.md rule 5); README.txt still asks the
 * player to look.
 * Bounds, so /data never fills: at most keep folders (PW_DETAILED_LOGS_KEEP), each at most PW_DETAILED_LOGS_FOLDER_BYTES
 * (fill copies only what the folder has room for, and says how many files it left out), one copied file at most
 * PW_DETAILED_LOGS_FILE_BYTES (its end: a longer file starts at the first line break after the cut, below a line that
 * says so), the kernel log at most PW_DETAILED_LOGS_KLOG_BYTES. All of them: at most keep * FOLDER_BYTES.
 * Functions return 0 or an errno and never abort; symbolic links are never followed. */
#include <stddef.h>
#include <time.h>

#define PW_DETAILED_LOGS_ROOT "/data/ps5proton/logs/detailed"
#define PW_DETAILED_LOGS_KEEP 5u
#define PW_DETAILED_LOGS_FOLDER_BYTES (96ull << 20)
#define PW_DETAILED_LOGS_FILE_BYTES (32ull << 20)
#define PW_DETAILED_LOGS_KLOG_BYTES (16ull << 20)
#define PW_DETAILED_LOGS_PATH 256u
#define PW_DETAILED_LOGS_CARRY 32u
#ifdef __cplusplus
extern "C" {
#endif
/* *enabled: 1 or 0; 0 when the file is missing (ENOENT) or not understood (EINVAL). */
int pw_detailed_logs_load(int *enabled, const char *path);
int pw_detailed_logs_save(int enabled, const char *path);
/* The session's folder into folder (size bytes). *removed (may be NULL) counts the files of the pruned folders. A folder
 * that cannot be pruned (it holds a folder of its own) is left and the next oldest goes. EINVAL for keep 0, a NULL
 * argument or a root longer than PW_DETAILED_LOGS_PATH - 40; EEXIST when ten folders have this second's name. */
int pw_detailed_logs_begin(const char *root, unsigned keep, time_t now, char *folder, size_t size, unsigned *removed);
/* The folder a session left to fill, consumed: 0 and its path; ENOENT when there is none; EINVAL (consumed too) when
 * <root>/pending does not name a folder directly under root. */
int pw_detailed_logs_pending(const char *root, char *folder, size_t size);
/* Copy the sources into folder: a regular file under its own name, a folder's regular *.log and *.txt files (not its
 * subfolders). A name already in folder, or a missing source, is skipped. *copied and *left_out (may be NULL) count the
 * files copied and those the folder had no room for. Returns the first failure, after trying every source. */
int pw_detailed_logs_fill(const char *folder, const char *const *sources, size_t count, unsigned *copied,
                          unsigned *left_out);
/* Mask the IPv4 addresses in text[0, length) in place. Returns how many bytes at its end are a run of digits and dots
 * that may go on in the next chunk (0 when final; at most PW_DETAILED_LOGS_CARRY, from the run's first digit): the
 * caller passes them again, first, with the next chunk. A longer run is settled where the chunk ends and *continued
 * (may be NULL) is set, so the next call leaves the rest of that run alone. So chunks mask exactly what the whole text
 * would, except that an address followed by more than PW_DETAILED_LOGS_CARRY - 15 dots and then a digit is masked
 * too: never less than the whole text (tests/test_pw_detailed_logs.c checks both on random text). */
size_t pw_detailed_logs_mask(char *text, size_t length, int final, int *continued);
/* Read from until its end, masked, writing at most cap bytes to to (the kernel log from klogsrv's socket, a copied
 * file). *written (may be NULL) is what was written. 0 at the end of input or at the cap, else the errno. */
int pw_detailed_logs_stream(int from, int to, unsigned long long cap, unsigned long long *written);
#ifdef __cplusplus
}
#endif
#endif
