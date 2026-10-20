/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_PAYLOAD_RENDEZVOUS_H
#define PW_PAYLOAD_RENDEZVOUS_H
/* How the title hands a helper's loader its control endpoint (pw_payload_spawn.h, the title's side;
 * native/proton_payload_child.c, the loader's), and the rule every descriptor handoff here keeps:
 *
 * A unix socket passed through SCM_RIGHTS stays open in its sender until the receiver has shown it holds it.
 *
 * The console's kernel flushes a unix socket whose every reference is in flight: its receive side is shut and
 * emptied (a read returns 0 at once, readable without hangup) while its send side still works. That is FreeBSD's
 * unix-domain garbage collector, which cannot see a socket between soreceive() taking it off the carrier's buffer
 * and the receiver's descriptor table taking it in. unix-handoff-2026-10-10-01 (tests/unix-handoff-probe.c, eight
 * threads of handoffs): a sender that closed its copy at once lost 67 of 160,000 sockets this way; one that held
 * its copy until the receiver acknowledged lost none of 160,000; Linux lost none either way. In
 * mihawk-effects-gate-2026-10-10-03 a helper's control endpoint arrived flushed (its first read returned 0 bytes,
 * so it refused the bootstrap and the title's first Windows client failed), as in
 * mihawk-work-area-gate-2026-10-09-01 and a 2026-10-07 run (record removed from the tree; in git history).
 *
 * Unaffected: a reply socket the receiver only writes to (only the receive side is flushed), and shared memory or
 * any other descriptor that is not a unix socket.
 *
 * The rendezvous: the loader listens on a path named by a token the title gave it; the title connects; the loader
 * says {PW_PAYLOAD_HELLO, its pid}; the title sends PW_PAYLOAD_MARKER with the endpoint; the loader, now holding
 * it, answers {PW_PAYLOAD_HELD, its pid}. Only then does the title close its copy. A loader and a title of
 * different builds refuse each other at the hello. */
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include "pw_wine_process_io.h"

#define PW_PAYLOAD_HELLO 0x50575032u /* "PWP2": the loader acknowledges the endpoint it holds */
#define PW_PAYLOAD_HELD 0x50574844u  /* "PWHD" */
enum { PW_PAYLOAD_MARKER = 0x50, PW_PAYLOAD_WAIT_MS = 10000 };

/* Two words from a stream, within timeout_ms for each piece. 0, or -1 with errno (EPROTO at the other end's close). */
static inline int pw_payload_read_words(int connection, uint32_t words[2], int timeout_ms)
{
    size_t have = 0;
    while (have < 2 * sizeof(uint32_t)) {
        struct pollfd wait = {connection, POLLIN, 0};
        const int polled = poll(&wait, 1, timeout_ms);
        if (polled < 0 && errno == EINTR) continue;
        if (polled != 1) { if (!polled) errno = ETIMEDOUT; return -1; }
        const ssize_t got = recv(connection, (char *)words + have, 2 * sizeof(uint32_t) - have, 0);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) { if (!got) errno = EPROTO; return -1; }
        have += (size_t)got;
    }
    return 0;
}

/* The title's side, on its connection to the loader. *native_pid is the loader's pid as soon as it said hello
 * (0 before), so a failure after that can still close the process. The caller owns control throughout and closes
 * it after a return of 0, when the loader holds its own. 0, or -1 with errno (EPROTO: not this protocol's loader,
 * or it refused the endpoint; ETIMEDOUT). */
static inline int pw_payload_give(int connection, int control, int *native_pid)
{
    uint32_t hello[2] = {0, 0}, held[2] = {0, 0};
    *native_pid = 0;
    if (pw_payload_read_words(connection, hello, PW_PAYLOAD_WAIT_MS)) return -1;
    if (hello[0] != PW_PAYLOAD_HELLO || !hello[1] || hello[1] > (uint32_t)INT_MAX || hello[1] == (uint32_t)getpid()) {
        errno = EPROTO;
        return -1;
    }
    *native_pid = (int)hello[1];
    const unsigned char marker = PW_PAYLOAD_MARKER;
    if (pw_child_send(connection, &marker, 1, &control, 1)) return -1;
    if (pw_payload_read_words(connection, held, PW_PAYLOAD_WAIT_MS)) return -1;
    if (held[0] != PW_PAYLOAD_HELD || held[1] != hello[1]) {
        errno = EPROTO;
        return -1;
    }
    return 0;
}

/* The loader's side, on the connection it accepted: the endpoint (a SOCK_SEQPACKET socket, now in this process's
 * table), after telling the title so. -1 with errno, and the title told nothing, if it is not one. */
static inline int pw_payload_take(int connection)
{
    const uint32_t hello[2] = {PW_PAYLOAD_HELLO, (uint32_t)getpid()};
    if (send(connection, hello, sizeof(hello), MSG_NOSIGNAL) != (ssize_t)sizeof(hello)) return -1;
    struct pollfd wait = {connection, POLLIN, 0};
    int polled;
    do polled = poll(&wait, 1, PW_PAYLOAD_WAIT_MS); while (polled < 0 && errno == EINTR);
    if (polled != 1) { if (!polled) errno = ETIMEDOUT; return -1; }
    unsigned char marker = 0;
    unsigned count = 0;
    int fds[2] = {-1, -1};
    const ssize_t bytes = pw_child_recv(connection, &marker, 1, fds, &count);
    if (bytes != 1 || marker != PW_PAYLOAD_MARKER || count != 1) {
        for (unsigned i = 0; i < count; ++i) close(fds[i]);
        if (bytes >= 0) errno = EPROTO;
        return -1;
    }
    int type = 0;
    socklen_t length = sizeof(type);
    if (getsockopt(fds[0], SOL_SOCKET, SO_TYPE, &type, &length) || type != SOCK_SEQPACKET) {
        close(fds[0]);
        errno = EPROTO;
        return -1;
    }
    const uint32_t held[2] = {PW_PAYLOAD_HELD, (uint32_t)getpid()};
    if (send(connection, held, sizeof(held), MSG_NOSIGNAL) != (ssize_t)sizeof(held)) {
        const int saved = errno;
        close(fds[0]);
        errno = saved;
        return -1;
    }
    return fds[0];
}
#endif
