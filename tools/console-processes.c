/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Every process on the console, with what it waits on, for when the control payload (ps5vkctl) cannot answer:
 * a payload sent through websrv's launcher (POST /elfldr, pipe=1), whose standard output comes back to the sender.
 * It only reads (sysctl kern.proc); tools/console-processes.py builds and sends it.
 *
 *   console-processes                pid, parent, state, threads, wait event, start time, name
 *
 * A process's wait event is its first thread's. Per-thread records (KERN_PROC_INC_THREAD) came back empty on the
 * console, even for websrv's five threads, so there is no per-thread listing.
 * States are FreeBSD's p_stat: 1 new, 2 running, 3 asleep, 4 stopped, 5 zombie, 6 waiting, 7 on a lock. The kernel's
 * records are ki_structsize apart, which on the console is 1096 bytes, not the SDK's 1088: each is copied into the
 * SDK's layout, whose fields read here come first. */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/user.h>
#include <unistd.h>
#include <time.h>

static void line(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    putchar('\n');
    fflush(stdout);
}

static struct kinfo_proc *records(int what, int pid, size_t *count)
{
    int mib[4] = {CTL_KERN, KERN_PROC, what, pid};
    size_t bytes = 0;
    *count = 0;
    if (sysctl(mib, 4, NULL, &bytes, NULL, 0) != 0 || !bytes)
        return NULL;
    bytes += bytes / 4;
    char *raw = malloc(bytes);
    if (!raw || sysctl(mib, 4, raw, &bytes, NULL, 0) != 0) {
        free(raw);
        return NULL;
    }
    struct kinfo_proc *list = calloc(bytes / 64 + 1, sizeof(*list));
    size_t n = 0;
    for (const char *cursor = raw; list && cursor + sizeof(int) <= raw + bytes;) {
        const struct kinfo_proc *record = (const struct kinfo_proc *)cursor;
        if (record->ki_structsize <= 0 || cursor + record->ki_structsize > raw + bytes)
            break;
        const size_t size = (size_t)record->ki_structsize;
        memcpy(&list[n++], record, size < sizeof(*list) ? size : sizeof(*list));
        cursor += size;
    }
    free(raw);
    *count = n;
    return list;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    struct timespec now;
    if (!clock_gettime(CLOCK_MONOTONIC, &now))
        line("console-clock monotonic_ms=%lld", now.tv_sec * 1000ll + now.tv_nsec / 1000000);
    size_t count = 0;
    struct kinfo_proc *list = records(KERN_PROC_PROC, 0, &count);
    if (!list) {
        line("console-processes: kern.proc failed, errno %d", errno);
        return 1;
    }
    for (size_t i = 0; i < count; i++)
        line("pid=%d ppid=%d state=%d threads=%d wait=%s start=%lld name=%s", list[i].ki_pid, list[i].ki_ppid,
             (int)list[i].ki_stat, list[i].ki_numthreads, list[i].ki_wmesg, (long long)list[i].ki_start.tv_sec,
             list[i].ki_comm);
    line("console-processes done: %zu processes", count);
    return 0;
}
