/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_LOG_SESSIONS_H
#define PW_LOG_SESSIONS_H
/* The logs of the title's last Wine sessions, kept through the next launches.
 *
 * A session is one Wine start by the title (proton_run_wine). Its helpers write <root>/0/wine-child-<pid>.log and the
 * title its flight-recorder.log there. At the next start the title rotates: 0 becomes 1, 1 becomes 2, the oldest goes,
 * and the previous session's title-last.log and wine-last.log are moved into 1 beside its helpers' logs.
 * Why: until this, every Wine start deleted the previous session's helper logs from the install folder
 * (native/proton_wine.cpp), so a run that froze the console lost them at the next launch, and the folder that holds
 * eboot.bin and libc.prx took every log write.
 *
 * pw_log_sessions_rotate(root, keep, removed), in this order:
 *   mkdir root;
 *   remove the regular files directly in <root>/<keep-1>, then rmdir it (it may be missing);
 *   rename <root>/<i> to <root>/<i+1> for i = keep-2 down to 0, a missing one skipped;
 *   mkdir <root>/0.
 * On success <root>/0 exists and is empty and <root>/<i> holds what <root>/<i-1> held. On the first failure nothing
 * later is renamed and <root>/0 is still made if it is missing, so the new session then shares <root>/0 with the
 * previous one: the caller reports the status, and a run whose status is not 0 is failed on it (tools/run-console.py).
 * *removed (may be NULL) counts the files removed. Returns 0 or an errno, never aborts: EINVAL for a NULL root or a
 * keep outside 2..PW_LOG_SESSIONS_MAX_KEEP; ENAMETOOLONG for a root longer than PW_LOG_SESSIONS_PATH - 16; ENOTDIR when
 * <root>/<keep-1> is not a folder and ENOTEMPTY when it holds anything but regular files (nothing in it is removed);
 * otherwise the errno of the call that failed. Symbolic links are never followed. */
#define PW_LOG_SESSIONS_ROOT "/data/ps5proton/logs/sessions"
#define PW_LOG_SESSIONS_KEEP 3u
#define PW_LOG_SESSIONS_MAX_KEEP 9u
#define PW_LOG_SESSIONS_PATH 256u
#ifdef __cplusplus
extern "C" {
#endif
int pw_log_sessions_rotate(const char *root, unsigned keep, unsigned *removed);
#ifdef __cplusplus
}
#endif
#endif
