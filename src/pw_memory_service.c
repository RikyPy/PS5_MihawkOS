/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pw_memory_service.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#ifdef __FreeBSD__
#include <sys/filio.h>
#endif

uint32_t pw_memory_pipe_copy(int pipe_fds[2], char *to, const char *from, uint32_t count)
{
    uint32_t done = 0;

    while (done < count) {
        const uint32_t piece = count - done < 16384 ? count - done : 16384;
        /* The source not readable all the way: the kernel takes what it could, or nothing. */
        const ssize_t put = write(pipe_fds[1], from + done, piece);
        ssize_t got;

        if (put <= 0) break;
        got = read(pipe_fds[0], to + done, (size_t)put);
        if (got < put) {
            /* Not writable all the way: what is left in the pipe is not wanted. */
            char scratch[4096];
            for (;;) {
                int left = 0;
                if (ioctl(pipe_fds[0], FIONREAD, &left) || left <= 0) break;
                if (read(pipe_fds[0], scratch, left < (int)sizeof(scratch) ? left : (int)sizeof(scratch)) <= 0) break;
            }
            if (got > 0) done += (uint32_t)got;
            break;
        }
        done += (uint32_t)put;
        /* A short write can mean a full small pipe, not an unreadable source.
         * Drain that piece and continue; a real unreadable boundary stops at
         * the next write without ever touching it in userspace. */
    }
    return done;
}

struct service { int fd; };
static void *serve(void *opaque)
{
    static char buffer[sizeof(struct pw_memory_copy) + PW_MEMORY_COPY_CHUNK];
    const int fd = ((struct service *)opaque)->fd;
    int pipe_fds[2];

    if (pipe(pipe_fds)) {
        close(fd);
        return NULL;
    }
    /* Linux may give this user an 8 KiB pipe. Writing our 16 KiB copy piece
     * before draining would block forever. Keep the owned writer nonblocking
     * so the kernel accepts what fits, then drain it before the next piece. */
    if (fcntl(pipe_fds[1], F_SETFL, O_NONBLOCK)) {
        close(pipe_fds[0]); close(pipe_fds[1]); close(fd);
        return NULL;
    }
    for (;;) {
        struct pw_memory_copy request;
        struct pw_memory_copied reply = {PW_MEMORY_SERVICE_MAGIC, 0, 0, 0};
        ssize_t got = recv(fd, buffer, sizeof(buffer), 0);
        size_t answer = sizeof(reply);

        if (got < 0 && errno == EINTR) continue;
        if (got < (ssize_t)sizeof(request)) break;   /* the title is gone */
        memcpy(&request, buffer, sizeof(request));
        if (request.magic != PW_MEMORY_SERVICE_MAGIC || request.size > PW_MEMORY_COPY_CHUNK ||
            (request.kind != PW_MEMORY_COPY_READ && request.kind != PW_MEMORY_COPY_WRITE) ||
            (request.kind == PW_MEMORY_COPY_WRITE && got != (ssize_t)(sizeof(request) + request.size)))
            reply.status = EINVAL;
        else if (request.kind == PW_MEMORY_COPY_WRITE)
            reply.done = pw_memory_pipe_copy(pipe_fds, (char *)(uintptr_t)request.address, buffer + sizeof(request),
                                             request.size);
        else {
            reply.done = pw_memory_pipe_copy(pipe_fds, buffer + sizeof(reply),
                                             (const char *)(uintptr_t)request.address, request.size);
            answer += reply.done;
        }
        memcpy(buffer, &reply, sizeof(reply));
        if (send(fd, buffer, answer, MSG_NOSIGNAL) < 0) break;
    }
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    close(fd);
    return NULL;
}

static int sized_pair(int pair[2])
{
    const int bytes = (int)(sizeof(struct pw_memory_copy) + PW_MEMORY_COPY_CHUNK) * 2;

    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair)) return -1;
    for (int i = 0; i < 2; ++i) {
        (void)setsockopt(pair[i], SOL_SOCKET, SO_SNDBUF, &bytes, sizeof(bytes));
        (void)setsockopt(pair[i], SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes));
    }
    return 0;
}

int pw_memory_service_start(int *title_end)
{
    static struct service service;
    pthread_attr_t attributes;
    pthread_t thread;
    int pair[2], error;

    if (sized_pair(pair)) return -1;
    service.fd = pair[0];
    pthread_attr_init(&attributes);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attributes, 256u << 10);
    /* The loader main thread's service tier (src/pw_helper_sched.h); a refusal keeps libkernel's default. */
    (void)pthread_attr_setinheritsched(&attributes, PTHREAD_INHERIT_SCHED);
    error = pthread_create(&thread, &attributes, serve, &service);
    pthread_attr_destroy(&attributes);
    if (error) {
        close(pair[0]);
        close(pair[1]);
        errno = error;
        return -1;
    }
    *title_end = pair[1];
    return 0;
}

static int ready(int fd, short events, int timeout_ms)
{
    struct pollfd p = {fd, events, 0};
    int ret;

    do ret = poll(&p, 1, timeout_ms); while (ret < 0 && errno == EINTR);
    return ret == 1 && (p.revents & events);
}

int pw_memory_service_copy(int fd, int writing, uint64_t address, void *buffer, uint32_t size, uint32_t *done,
                           int timeout_ms)
{
    static char chunk[sizeof(struct pw_memory_copy) + PW_MEMORY_COPY_CHUNK];

    *done = 0;
    while (*done < size) {
        const uint32_t piece = size - *done < PW_MEMORY_COPY_CHUNK ? size - *done : PW_MEMORY_COPY_CHUNK;
        const struct pw_memory_copy request = {PW_MEMORY_SERVICE_MAGIC,
                                               writing ? PW_MEMORY_COPY_WRITE : PW_MEMORY_COPY_READ,
                                               address + *done, piece, 0};
        struct pw_memory_copied reply;
        size_t length = sizeof(request);
        ssize_t got;

        memcpy(chunk, &request, sizeof(request));
        if (writing) {
            memcpy(chunk + sizeof(request), (const char *)buffer + *done, piece);
            length += piece;
        }
        if (!ready(fd, POLLOUT, timeout_ms) || send(fd, chunk, length, MSG_NOSIGNAL) != (ssize_t)length ||
            !ready(fd, POLLIN, timeout_ms) || (got = recv(fd, chunk, sizeof(chunk), 0)) < (ssize_t)sizeof(reply))
            return -1;
        memcpy(&reply, chunk, sizeof(reply));
        if (reply.magic != PW_MEMORY_SERVICE_MAGIC || reply.status || reply.done > piece ||
            got != (ssize_t)(sizeof(reply) + (writing ? 0 : reply.done)))
            return -1;
        if (!writing) memcpy((char *)buffer + *done, chunk + sizeof(reply), reply.done);
        *done += reply.done;
        if (reply.done < piece) break;
    }
    return 0;
}
