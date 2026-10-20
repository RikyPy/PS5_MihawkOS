/* SPDX-License-Identifier: GPL-3.0-or-later
 * Is a unix socket in transit through SCM_RIGHTS safe when its sender closes
 * its own copy before the receiver has taken it?
 *
 * The question comes from mihawk-effects-gate-2026-10-10-03-pe64: a helper's
 * first read of its control socket (the SEQPACKET endpoint the title handed it
 * through the rendezvous connection) returned 0 bytes, readable without hangup,
 * while the title's bootstrap send on the other end succeeded and the helper's
 * reply reached the title. That is sorflush(): receive side shut and emptied,
 * send side intact. FreeBSD 11's unix-domain garbage collector calls sorflush()
 * on a socket whose every reference is in flight and that no scanned socket
 * buffer holds; soreceive() detaches the rights from the buffer before
 * unp_externalize() installs them, and closing any unix socket while rights
 * are in flight schedules the collector.
 *
 * Each worker repeats the title's handoff: a fresh SEQPACKET pair (a,b) and a
 * fresh stream carrier (c,d); b is sent through c and a second thread takes it
 * from d.
 *   early  the sender closes c, then b, at once (the title before this probe)
 *   held   the receiver acknowledges on d; only then the sender closes c and b
 * The received endpoint is healthy if it polls not readable and a non-blocking
 * read says EAGAIN; flushed if that read says 0 (end of file). A byte sent on a
 * afterwards is still expected to arrive. The Linux build is the oracle.
 *
 *   unix-handoff-probe [early|held|both] [n=handoffs per worker] [s=seconds per mode]
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum { WORKERS = 8, MODE_EARLY = 0, MODE_HELD = 1, WAIT_MS = 5000 };

struct worker {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    int mode;
    unsigned limit;
    long long deadline;
    int carrier, received, receive_status, stop; /* mailbox; -1 is empty */
    unsigned handoffs, healthy, flushed, flushed_quiet_hangup, delivered_after_flush, anomalies;
    unsigned errors;
    const char *error_stage;
    int error_number;
    int anomaly_revents, anomaly_bytes, anomaly_errno;
};

static long long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000ll + t.tv_nsec / 1000000;
}
static int send_fd(int carrier, int fd)
{
    char marker = 'P';
    struct iovec vec = {&marker, 1};
    union { struct cmsghdr align; char bytes[CMSG_SPACE(sizeof(int))]; } control;
    struct msghdr message = {0};
    memset(&control, 0, sizeof(control));
    message.msg_iov = &vec;
    message.msg_iovlen = 1;
    message.msg_control = control.bytes;
    message.msg_controllen = sizeof(control.bytes);
    struct cmsghdr *header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(header), &fd, sizeof(int));
    ssize_t sent;
    do sent = sendmsg(carrier, &message, MSG_NOSIGNAL); while (sent < 0 && errno == EINTR);
    return sent == 1 ? 0 : -1;
}
static int recv_fd(int carrier)
{
    char marker = 0;
    struct iovec vec = {&marker, 1};
    union { struct cmsghdr align; char bytes[CMSG_SPACE(sizeof(int))]; } control;
    struct msghdr message = {0};
    memset(&control, 0, sizeof(control));
    message.msg_iov = &vec;
    message.msg_iovlen = 1;
    message.msg_control = control.bytes;
    message.msg_controllen = sizeof(control.bytes);
    ssize_t got;
    do got = recvmsg(carrier, &message, 0); while (got < 0 && errno == EINTR);
    struct cmsghdr *header = got == 1 ? CMSG_FIRSTHDR(&message) : NULL;
    if (!header || header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS ||
        header->cmsg_len != CMSG_LEN(sizeof(int)) || marker != 'P') {
        if (got >= 0) errno = EPROTO;
        return -1;
    }
    int fd;
    memcpy(&fd, CMSG_DATA(header), sizeof(int));
    return fd;
}
static int readable(int fd, int timeout)
{
    struct pollfd p = {fd, POLLIN, 0};
    return poll(&p, 1, timeout) == 1 && (p.revents & POLLIN);
}
static void fail(struct worker *w, const char *stage)
{
    if (!w->errors++) { w->error_stage = stage; w->error_number = errno; }
}

/* The receiver: what the helper does with its rendezvous connection. */
static void *receiver(void *argument)
{
    struct worker *w = argument;
    for (;;) {
        pthread_mutex_lock(&w->lock);
        while (w->carrier < 0 && !w->stop) pthread_cond_wait(&w->changed, &w->lock);
        const int carrier = w->carrier, stop = w->stop;
        w->carrier = -1;
        pthread_mutex_unlock(&w->lock);
        if (carrier < 0 && stop) return NULL;
        int fd = -1, status = 0;
        if (!readable(carrier, WAIT_MS)) status = ETIMEDOUT;
        else if ((fd = recv_fd(carrier)) < 0) status = errno ? errno : EPROTO;
        else if (w->mode == MODE_HELD && send(carrier, "A", 1, MSG_NOSIGNAL) != 1) status = errno ? errno : EIO;
        close(carrier);
        pthread_mutex_lock(&w->lock);
        w->received = fd;
        w->receive_status = status;
        pthread_cond_broadcast(&w->changed);
        pthread_mutex_unlock(&w->lock);
    }
}

/* The sender: what the title does in payload_spawn and spawn_native. */
static void *sender(void *argument)
{
    struct worker *w = argument;
    while (w->handoffs < w->limit && now_ms() < w->deadline) {
        int pair[2], carrier[2];
        if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair)) { fail(w, "seqpacket"); break; }
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, carrier)) { fail(w, "carrier"); close(pair[0]); close(pair[1]); break; }
        pthread_mutex_lock(&w->lock);
        w->received = -1;
        w->receive_status = -1;
        w->carrier = carrier[1];
        pthread_cond_broadcast(&w->changed);
        pthread_mutex_unlock(&w->lock);
        const int sent = send_fd(carrier[0], pair[1]);
        if (sent) fail(w, "send");
        if (w->mode == MODE_HELD && !sent) {
            char ack = 0;
            if (!readable(carrier[0], WAIT_MS) || recv(carrier[0], &ack, 1, 0) != 1 || ack != 'A') fail(w, "acknowledge");
        }
        close(carrier[0]);
        close(pair[1]);
        pthread_mutex_lock(&w->lock);
        while (w->receive_status < 0) pthread_cond_wait(&w->changed, &w->lock);
        const int fd = w->received, status = w->receive_status;
        pthread_mutex_unlock(&w->lock);
        if (fd < 0) {
            errno = status;
            fail(w, "receive");
            close(pair[0]);
            break;
        }
        ++w->handoffs;
        struct pollfd p = {fd, POLLIN, 0};
        const int polled = poll(&p, 1, 0);
        char byte = 0;
        errno = 0;
        const ssize_t first = recv(fd, &byte, 1, MSG_DONTWAIT);
        const int first_errno = errno;
        if (polled == 0 && first < 0 && first_errno == EAGAIN) {
            ++w->healthy;
        } else if (first == 0) {
            ++w->flushed;
            if (polled == 1 && (p.revents & POLLIN) && !(p.revents & POLLHUP)) ++w->flushed_quiet_hangup;
            if (send(pair[0], "x", 1, MSG_NOSIGNAL) == 1 && recv(fd, &byte, 1, MSG_DONTWAIT) == 1 && byte == 'x')
                ++w->delivered_after_flush;
        } else {
            if (!w->anomalies++) { w->anomaly_revents = polled == 1 ? p.revents : -1; w->anomaly_bytes = (int)first; w->anomaly_errno = first_errno; }
        }
        close(fd);
        close(pair[0]);
    }
    pthread_mutex_lock(&w->lock);
    w->stop = 1;
    pthread_cond_broadcast(&w->changed);
    pthread_mutex_unlock(&w->lock);
    return NULL;
}

static unsigned run(int mode, unsigned limit, unsigned seconds, unsigned *handoffs_out, unsigned *errors_out)
{
    static struct worker workers[WORKERS];
    pthread_t senders[WORKERS], receivers[WORKERS];
    unsigned started = 0;
    const long long begin = now_ms();
    for (unsigned i = 0; i < WORKERS; ++i) {
        struct worker *w = &workers[i];
        memset(w, 0, sizeof(*w));
        pthread_mutex_init(&w->lock, NULL);
        pthread_cond_init(&w->changed, NULL);
        w->mode = mode;
        w->limit = limit;
        w->deadline = begin + seconds * 1000ll;
        w->carrier = w->received = -1;
        if (pthread_create(&receivers[i], NULL, receiver, w)) break;
        if (pthread_create(&senders[i], NULL, sender, w)) {
            pthread_mutex_lock(&w->lock); w->stop = 1; pthread_cond_broadcast(&w->changed); pthread_mutex_unlock(&w->lock);
            pthread_join(receivers[i], NULL);
            break;
        }
        ++started;
    }
    unsigned handoffs = 0, healthy = 0, flushed = 0, quiet = 0, delivered = 0, anomalies = 0, errors = 0;
    const char *stage = "none";
    int error_number = 0, anomaly_revents = 0, anomaly_bytes = 0, anomaly_errno = 0;
    for (unsigned i = 0; i < started; ++i) {
        pthread_join(senders[i], NULL);
        pthread_join(receivers[i], NULL);
        const struct worker *w = &workers[i];
        handoffs += w->handoffs; healthy += w->healthy; flushed += w->flushed;
        quiet += w->flushed_quiet_hangup; delivered += w->delivered_after_flush;
        if (w->anomalies && !anomalies) { anomaly_revents = w->anomaly_revents; anomaly_bytes = w->anomaly_bytes; anomaly_errno = w->anomaly_errno; }
        anomalies += w->anomalies;
        if (w->errors && !errors) { stage = w->error_stage; error_number = w->error_number; }
        errors += w->errors;
        pthread_mutex_destroy(&workers[i].lock);
        pthread_cond_destroy(&workers[i].changed);
    }
    if (started < WORKERS) { ++errors; if (!strcmp(stage, "none")) { stage = "thread"; error_number = EAGAIN; } }
    printf("unix-handoff mode=%s workers=%u handoffs=%u healthy=%u flushed=%u flushed_readable_without_hangup=%u "
           "delivered_after_flush=%u anomalies=%u anomaly_revents=%d anomaly_bytes=%d anomaly_errno=%d "
           "errors=%u first_error=%s errno=%d ms=%lld\n",
           mode == MODE_HELD ? "held" : "early", started, handoffs, healthy, flushed, quiet, delivered, anomalies,
           anomaly_revents, anomaly_bytes, anomaly_errno, errors, stage, error_number, now_ms() - begin);
    fflush(stdout);
    *handoffs_out = handoffs;
    *errors_out = errors + anomalies;
    return flushed;
}

int main(int argc, char **argv)
{
    const char *which = "both";
    unsigned limit = 20000, seconds = 12;
    /* websrv's payload launcher passes `args` as argv; other words are ignored. */
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "early") || !strcmp(argv[i], "held") || !strcmp(argv[i], "both")) which = argv[i];
        else if (!strncmp(argv[i], "n=", 2)) limit = (unsigned)strtoul(argv[i] + 2, NULL, 10);
        else if (!strncmp(argv[i], "s=", 2)) seconds = (unsigned)strtoul(argv[i] + 2, NULL, 10);
    }
    if (!limit) limit = 20000;
    if (limit > 200000) limit = 200000;
    if (seconds < 1 || seconds > 30) seconds = 12;
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("unix-handoff pid=%d which=%s limit=%u seconds=%u workers=%u\n", (int)getpid(), which, limit, seconds, WORKERS);
    unsigned early = 0, held = 0, early_handoffs = 0, held_handoffs = 0, early_errors = 0, held_errors = 0;
    if (strcmp(which, "held")) early = run(MODE_EARLY, limit, seconds, &early_handoffs, &early_errors);
    if (strcmp(which, "early")) held = run(MODE_HELD, limit, seconds, &held_handoffs, &held_errors);
    printf("UNIX_HANDOFF early_flushed=%u early_handoffs=%u early_errors=%u held_flushed=%u held_handoffs=%u held_errors=%u\n",
           early, early_handoffs, early_errors, held, held_handoffs, held_errors);
    return 0;
}
