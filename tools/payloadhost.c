/*
 * payloadhost - capacity questions about a process the jailbreak's payload
 * loader starts, which is outside the title's seven helpers.
 *
 * Sent with websrv's payload launcher (POST /elfldr, pipe=1), which streams
 * stdout back and passes `args` as argv. One step per send; every step prints
 * what it is about to do before it does it, on stdout *and* stderr, so a
 * console that goes down leaves the step that took it down on the wire.
 *
 * Steps (argv, since websrv may pass args as the whole argv with no program
 * name first, an argument is matched anywhere):
 *
 *   linger      stay alive printing a heartbeat every couple of seconds, and
 *               exit when stdout can no longer be written. That is the
 *               lifetime mechanism a payload-born helper needs: the starter
 *               holds the stream, so the payload dies with it instead of
 *               outliving the title. Also the step the count test uses, since
 *               a process that stays alive is a process that can be counted.
 *   fd          how many files this process may hold open at once: one file is
 *               opened repeatedly until the kernel refuses, then all are
 *               released. The title's helpers share one title-wide open-file
 *               budget; whether a payload process has its own is unmeasured.
 *
 * It deliberately does nothing else: no direct memory, no GPU device, no fork,
 * no signal changes. Everything here is an operation that has already run on
 * this console many times.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FD_BUDGET_FILE "/data/payloadhost-budget.dat"
#define FD_BUDGET_MAX 8192

/* Exit evidence goes to a file, not to the stream: the stream is the thing that
 * broke. The host reads these over FTP, so "it ended with its starter" and "it
 * was orphaned" cannot be confused. */
#define MARKER_DIR "/data"

static void marker(const char *suffix)
{
    char path[80];
    const int length = snprintf(path, sizeof(path), MARKER_DIR "/payloadhost-%d.%s", getpid(), suffix);
    if (length <= 0 || (size_t)length >= sizeof(path))
        return;
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return;
    (void)write(fd, path, (size_t)length);
    (void)close(fd);
}

static void line(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    putchar('\n');
    fflush(stdout);

    /* Second copy on standard error, which reaches klog when a capture is
     * armed: if a step takes the console down, the elfldr stream dies with it
     * and the last line printed is the only evidence of what ran. */
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
    fflush(stderr);
}

static int requested(int argc, char **argv, const char *step)
{
    for (int i = 0; i < argc; ++i)
        if (argv[i] && !strcmp(argv[i], step))
            return 1;
    return 0;
}

static void report_identity(int argc, char **argv)
{
    line("payloadhost pid=%d ppid=%d uid=%d euid=%d gid=%d egid=%d", getpid(), getppid(), getuid(),
         geteuid(), getgid(), getegid());
    for (int i = 0; i < argc && i < 6; ++i)
        line("  argv[%d]=%s", i, argv[i] ? argv[i] : "(null)");
}

/* Stay alive, and die when the stream that started this process goes away. */
static void step_linger(void)
{
    unsigned ticks = 0;
    marker("alive");
    line("linger: marker %s/payloadhost-%d.alive written", MARKER_DIR, getpid());
    line("linger: heartbeat every 2 s; exit when stdout can no longer be written");
    for (;;) {
        usleep(2000 * 1000);
        ++ticks;
        if (printf("linger: alive %u\n", ticks) < 0 || fflush(stdout) != 0) {
            /* The stream is gone: whoever started this process has stopped
             * listening, which is the only lifetime signal available. SIGPIPE
             * is ignored so the failure is an error here, not a silent kill,
             * and the marker file is the evidence the host can read. */
            {
                char path[80];
                if (snprintf(path, sizeof(path), MARKER_DIR "/payloadhost-%d.alive", getpid()) > 0)
                    (void)unlink(path);
            }
            marker("gone");
            fprintf(stderr, "linger: stdout closed after %u ticks, exiting\n", ticks);
            return;
        }
    }
}

/* How many file descriptors this process may hold at once. */
static void step_fd(void)
{
    line("fd: about to open %s repeatedly until the kernel refuses", FD_BUDGET_FILE);
    const int seed = open(FD_BUDGET_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    line("fd: create -> %d%s", seed, seed < 0 ? " (failed)" : "");
    if (seed < 0)
        return;
    (void)write(seed, "x", 1);

    static int held[FD_BUDGET_MAX];
    unsigned count = 0;
    int last_error = 0;
    for (; count < FD_BUDGET_MAX; ++count) {
        errno = 0;
        const int fd = open(FD_BUDGET_FILE, O_RDONLY);
        if (fd < 0) {
            last_error = errno;
            break;
        }
        held[count] = fd;
    }
    line("fd: opened %u in addition to the process's own; last errno=%d (%s)", count, last_error,
         last_error ? strerror(last_error) : "ok");
    for (unsigned i = 0; i < count; ++i)
        (void)close(held[i]);
    (void)close(seed);
    (void)unlink(FD_BUDGET_FILE);
    line("fd: all released");
}

int main(int argc, char **argv)
{
    /* A stream that goes away must be an error this process can act on, not a
     * signal that kills it before it can write its marker. */
    (void)signal(SIGPIPE, SIG_IGN);
    report_identity(argc, argv);

    if (requested(argc, argv, "fd")) {
        step_fd();
        line("payloadhost done");
        return 0;
    }

    step_linger();
    line("payloadhost done");
    return 0;
}
