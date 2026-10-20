/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The rendezvous contract (wine/ps5/pw_payload_rendezvous.h): the title's side returns only once the loader says it
 * holds the endpoint, and refuses a loader that does not; the loader's side says so only for a SEQPACKET endpoint.
 * Each loader runs in a forked process, as on the console. Linux never flushes a socket in flight, so the flush itself
 * is the console's to show (tools/run-unix-handoff-probe.py); this pins the order that prevents it. */
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "../wine/ps5/pw_payload_rendezvous.h"

static long long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000ll + t.tv_nsec / 1000000;
}
static void pair_of(int type, int pair[2]) { assert(!socketpair(AF_UNIX, type, 0, pair)); }
static int finished(pid_t pid)
{
    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status));
    return WEXITSTATUS(status);
}

/* The real loader: takes the endpoint, then echoes the title's first packet on it. */
static void loader_echo(int connection)
{
    const int control = pw_payload_take(connection);
    close(connection);
    if (control < 0) _exit(10);
    char packet[64];
    struct pollfd wait = {control, POLLIN, 0};
    if (poll(&wait, 1, 5000) != 1) _exit(11);
    const ssize_t got = recv(control, packet, sizeof(packet), 0);
    if (got <= 0 || send(control, packet, (size_t)got, MSG_NOSIGNAL) != got) _exit(12);
    _exit(0);
}
/* A loader that misbehaves in one way. */
enum { OLD_HELLO, NO_HELD, WRONG_HELD, SLOW_HELD };
static void loader_bad(int connection, int how)
{
    const uint32_t hello[2] = {how == OLD_HELLO ? 0x50575031u : PW_PAYLOAD_HELLO, (uint32_t)getpid()};
    if (send(connection, hello, sizeof(hello), MSG_NOSIGNAL) != sizeof(hello)) _exit(20);
    if (how == OLD_HELLO) { close(connection); _exit(0); }
    unsigned char marker = 0;
    unsigned count = 0;
    int fds[2];
    struct pollfd wait = {connection, POLLIN, 0};
    if (poll(&wait, 1, 5000) != 1 || pw_child_recv(connection, &marker, 1, fds, &count) != 1 || count != 1) _exit(21);
    if (how == SLOW_HELD) usleep(150000);
    if (how != NO_HELD) {
        const uint32_t held[2] = {PW_PAYLOAD_HELD, (uint32_t)getpid() + (how == WRONG_HELD)};
        if (send(connection, held, sizeof(held), MSG_NOSIGNAL) != sizeof(held)) _exit(22);
    }
    close(fds[0]);
    close(connection);
    _exit(0);
}
/* other: the title's descriptors, which a loader process never has. */
static pid_t start_loader(int connection, const int other[3], int how)
{
    const pid_t pid = fork();
    assert(pid >= 0);
    if (!pid) {
        for (unsigned i = 0; i < 3; ++i) close(other[i]);
        if (how < 0) loader_echo(connection);
        loader_bad(connection, how);
    }
    close(connection);
    return pid;
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    {   /* The endpoint reaches a loader that says it holds it; the title's copy can then go. */
        int connection[2], control[2], native = -1;
        pair_of(SOCK_STREAM, connection);
        pair_of(SOCK_SEQPACKET, control);
        const pid_t pid = start_loader(connection[1], (const int[3]){connection[0], control[0], control[1]}, -1);
        assert(!pw_payload_give(connection[0], control[1], &native));
        assert(native == pid);
        close(connection[0]);
        close(control[1]);
        assert(send(control[0], "boot", 4, MSG_NOSIGNAL) == 4);
        char echo[8] = {0};
        struct pollfd wait = {control[0], POLLIN, 0};
        assert(poll(&wait, 1, 5000) == 1 && recv(control[0], echo, sizeof(echo), 0) == 4 && !memcmp(echo, "boot", 4));
        assert(!finished(pid));
        close(control[0]);
    }
    {   /* The title waits for the loader's word, however late. */
        int connection[2], control[2], native = -1;
        pair_of(SOCK_STREAM, connection);
        pair_of(SOCK_SEQPACKET, control);
        const pid_t pid = start_loader(connection[1], (const int[3]){connection[0], control[0], control[1]}, SLOW_HELD);
        const long long begin = now_ms();
        assert(!pw_payload_give(connection[0], control[1], &native));
        assert(now_ms() - begin >= 140 && native == pid);
        assert(!finished(pid));
        close(connection[0]); close(control[0]); close(control[1]);
    }
    {   /* A loader of the old protocol is refused at its hello: nothing is sent to it. */
        int connection[2], control[2], native = -1;
        pair_of(SOCK_STREAM, connection);
        pair_of(SOCK_SEQPACKET, control);
        const pid_t pid = start_loader(connection[1], (const int[3]){connection[0], control[0], control[1]}, OLD_HELLO);
        errno = 0;
        assert(pw_payload_give(connection[0], control[1], &native) == -1 && errno == EPROTO && native == 0);
        assert(!finished(pid));
        close(connection[0]); close(control[0]); close(control[1]);
    }
    {   /* A loader that takes the endpoint but never says it holds it: refused, its pid kept for closing it. */
        int connection[2], control[2], native = -1;
        pair_of(SOCK_STREAM, connection);
        pair_of(SOCK_SEQPACKET, control);
        const pid_t pid = start_loader(connection[1], (const int[3]){connection[0], control[0], control[1]}, NO_HELD);
        errno = 0;
        assert(pw_payload_give(connection[0], control[1], &native) == -1 && errno == EPROTO && native == pid);
        assert(!finished(pid));
        close(connection[0]); close(control[0]); close(control[1]);
    }
    {   /* Another process's word is not this loader's. */
        int connection[2], control[2], native = -1;
        pair_of(SOCK_STREAM, connection);
        pair_of(SOCK_SEQPACKET, control);
        const pid_t pid = start_loader(connection[1], (const int[3]){connection[0], control[0], control[1]}, WRONG_HELD);
        errno = 0;
        assert(pw_payload_give(connection[0], control[1], &native) == -1 && errno == EPROTO && native == pid);
        assert(!finished(pid));
        close(connection[0]); close(control[0]); close(control[1]);
    }
    {   /* The loader takes only a packet socket, and says nothing of any other. */
        int connection[2], stream[2];
        pair_of(SOCK_STREAM, connection);
        pair_of(SOCK_STREAM, stream);
        const pid_t pid = start_loader(connection[1], (const int[3]){connection[0], stream[0], stream[1]}, -1);
        uint32_t hello[2];
        assert(!pw_payload_read_words(connection[0], hello, 5000));
        assert(hello[0] == PW_PAYLOAD_HELLO && hello[1] == (uint32_t)pid);
        const unsigned char marker = PW_PAYLOAD_MARKER;
        assert(!pw_child_send(connection[0], &marker, 1, &stream[1], 1));
        uint32_t held[2];
        errno = 0;
        assert(pw_payload_read_words(connection[0], held, 5000) == -1 && errno == EPROTO);
        assert(finished(pid) == 10);
        close(connection[0]); close(stream[0]); close(stream[1]);
    }
    puts("payload rendezvous: the endpoint is let go only after the loader holds it");
    return 0;
}
