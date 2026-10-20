/* SPDX-License-Identifier: GPL-3.0-or-later */
/* A log written through src/pw_log_drain.h against the broker's old pipe, with the disk (here: a sink pipe nobody
 * reads) held for 300 ms while 4,000 lines of 128 bytes are written, as the broker's lines are, line-buffered from
 * a FILE. With the old pipe, shrunk to 4 KiB as the kernel's small pipes are once its pipe memory runs out, a line
 * waits for the sink; with the socket pair no line waits, and lines that do not fit are lost whole: every line that
 * reaches the file is one the writer wrote, intact and in order. Prints both writers' worst line. */
#define _GNU_SOURCE
#include "../src/pw_log_drain.h"
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { LINES = 4000, HOLD_MS = 300 };

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

struct copier { int from, to; };
static void *copy_main(void *argument)   /* the broker's drain thread: whatever arrives, to the file */
{
    const struct copier *c = argument;
    char buffer[4096];
    for (;;) {
        const ssize_t got = read(c->from, buffer, sizeof(buffer));
        if (got <= 0) return NULL;
        for (ssize_t done = 0; done < got;) {
            const ssize_t put = write(c->to, buffer + done, (size_t)(got - done));
            if (put <= 0) return NULL;
            done += put;
        }
    }
}

struct sink { int fd; char *data; size_t size; };
static void *sink_main(void *argument)   /* the disk: held HOLD_MS, then read to the end */
{
    struct sink *s = argument;
    struct timespec hold = {0, HOLD_MS * 1000000L};
    nanosleep(&hold, NULL);
    size_t capacity = 1 << 20;
    s->data = malloc(capacity);
    for (;;) {
        if (s->size + 4096 > capacity) s->data = realloc(s->data, capacity *= 2);
        const ssize_t got = read(s->fd, s->data + s->size, 4096);
        if (got <= 0) return NULL;
        s->size += (size_t)got;
    }
}

/* Writes LINES lines through a FILE on `front`, drained from `back` to a sink held HOLD_MS; returns the worst line's
 * nanoseconds and leaves what reached the sink in *out. */
static uint64_t run(int front, int back, struct sink *out)
{
    int sink_pipe[2];
    assert(!pipe(sink_pipe));
    fcntl(sink_pipe[1], F_SETPIPE_SZ, 4096);   /* a small disk queue, so the drain thread waits too */
    struct copier copier = {back, sink_pipe[1]};
    *out = (struct sink){sink_pipe[0], NULL, 0};
    pthread_t copy, sink;
    assert(!pthread_create(&copy, NULL, copy_main, &copier) && !pthread_create(&sink, NULL, sink_main, out));
    FILE *log = fdopen(front, "w");
    assert(log);
    setvbuf(log, NULL, _IOLBF, 0);
    uint64_t worst = 0;
    for (unsigned i = 0; i < LINES; ++i) {
        const uint64_t start = now_ns();
        fprintf(log, "gpu-pacing line=%05u %0*u\n", i, 128 - 23, i);
        const uint64_t spent = now_ns() - start;
        if (spent > worst) worst = spent;
    }
    fclose(log);   /* the drain thread reads to the end, then the sink */
    pthread_join(copy, NULL);
    close(sink_pipe[1]);
    pthread_join(sink, NULL);
    close(sink_pipe[0]);
    close(back);
    return worst;
}

int main(void)
{
    /* The old way: a pipe, small as the kernel's are once its pipe memory is used up. */
    int old[2];
    assert(!pipe(old));
    const int small = fcntl(old[1], F_SETPIPE_SZ, 4096);
    struct sink old_sink;
    const uint64_t old_worst = run(old[1], old[0], &old_sink);

    /* The socket pair. */
    int ends[2];
    assert(pw_log_drain_pair(ends, 1 << 20) == 0);
    assert(fcntl(ends[1], F_GETFL) & O_NONBLOCK);
    struct sink new_sink;
    const uint64_t new_worst = run(ends[1], ends[0], &new_sink);

    /* What reached the file: whole lines, each one written, in order; the old pipe loses none. */
    unsigned old_lines = 0, new_lines = 0;
    int last = -1;
    for (char *line = new_sink.data, *end; line < new_sink.data + new_sink.size; line = end + 1) {
        end = memchr(line, '\n', (size_t)(new_sink.data + new_sink.size - line));
        assert(end && end - line == 127);
        unsigned number = 0, again = 0;
        assert(sscanf(line, "gpu-pacing line=%05u %u", &number, &again) == 2 && number == again && (int)number > last);
        last = (int)number;
        ++new_lines;
    }
    for (size_t at = 0; at < old_sink.size; ++at) old_lines += old_sink.data[at] == '\n';
    assert(old_lines == LINES);
    assert(new_lines > 0 && new_lines <= LINES);
    printf("test_pw_log_drain: small pipe (%d bytes): worst line %.1f ms, %u of %u lines; socket pair: worst line %.3f ms, "
           "%u of %u lines, the rest dropped whole\n", small, old_worst / 1e6, old_lines, LINES, new_worst / 1e6,
           new_lines, LINES);
    assert(old_worst >= HOLD_MS * 1000000ull / 2);   /* the hazard is real: the writer waited on the disk */
    assert(new_worst < 20 * 1000000ull);           /* and is gone */
    free(old_sink.data);
    free(new_sink.data);
    return 0;
}
