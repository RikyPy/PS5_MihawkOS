/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * How much pipe memory the console has left, as a new pipe sees it. After about three gates on one boot the kernel
 * logs "kern.ipc.maxpipekva exceeded" and every new pipe gets a small buffer until a restart (dns-check-2026-10-18-05,
 * -07). The title can read neither the kernel's totals (kern.ipc.pipes, pipekva, maxpipekva, pipeallocfail: all
 * refused, dns-check-2026-10-18-07) nor another process's descriptors (kern.proc.filedesc: refused for all 89
 * processes, shell-open-2026-10-18-after), so it cannot name the holder. What it can measure: the bytes a fresh pipe
 * takes before a non-blocking write would wait, 16 KiB or more (64 KiB once a write grows it) while less than half the
 * pipe memory is used, one page past that. The title logs it at each launch's start, a warning before the runs go bad.
 * Host test: tests/test_pw_pipe_memory.c.
 */
#ifndef PW_PIPE_MEMORY_H
#define PW_PIPE_MEMORY_H

#ifdef __cplusplus
extern "C" {
#endif

/* The bytes a fresh pipe takes before a non-blocking write would wait, up to 128 KiB; -1 when no pipe was made. */
long pw_pipe_buffer_probe(void);
/* Whether that many bytes say the pipe got a small buffer: the kernel's pipe memory is more than half used. */
static inline int pw_pipe_memory_low(long taken) { return taken >= 0 && taken < 16384; }

#ifdef __cplusplus
}
#endif
#endif
