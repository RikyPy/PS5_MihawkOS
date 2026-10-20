/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_LOG_DRAIN_H
#define PW_LOG_DRAIN_H
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
/* A log's way to its file that never holds up the thread writing a line (graphics patch 0028, the broker's record).
 *
 * The broker wrote its record into a pipe, which a thread of its own drained to the file, so a line written under the
 * broker's lock never waited on the console's disk (graphics-child-gpu-async-18). But the kernel gives a pipe its
 * buffer from the system's pipe memory, and once that is used up ("kern.ipc.maxpipekva exceeded", 11 times in
 * a 2026-10-17 run after an 84-case gate; record removed from the tree) a pipe gets a small one. Then each line waits for the drain thread to
 * reach the disk, under the lock. That run had one ~344 ms gap in every 10 s window, the broker's report period; on a
 * fresh boot the next build had none (2026-10-17, worst gap 77 ms).
 *
 * pw_log_drain_pair makes an AF_UNIX stream socket pair instead. Sockets take no pipe memory, and their buffer is
 * asked for, here up to `bytes` (the kernel caps it, kern.ipc.maxsockbuf). The writing end, ends[1], is
 * non-blocking: a line that does not fit is not written, rather than waited for. The caller fdopen()s ends[1]
 * line-buffered for its writers and drains ends[0] on its own thread, as before. Returns 0, or an errno with nothing
 * left open. Host test: tests/test_pw_log_drain.c, which also measures a small pipe's writer against this one with
 * the drain held 300 ms. */
static inline int pw_log_drain_pair(int ends[2], int bytes)
{
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, ends)) return errno ? errno : EIO;
    const int flags = fcntl(ends[1], F_GETFL);
    if (setsockopt(ends[1], SOL_SOCKET, SO_SNDBUF, &bytes, sizeof(bytes)) ||
        setsockopt(ends[0], SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes)) || flags < 0 ||
        fcntl(ends[1], F_SETFL, flags | O_NONBLOCK)) {
        const int error = errno ? errno : EIO;
        close(ends[0]);
        close(ends[1]);
        return error;
    }
    return 0;
}
#endif
