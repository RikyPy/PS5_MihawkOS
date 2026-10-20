/* SPDX-License-Identifier: GPL-3.0-or-later
 * gcprobe - what this process can see of the console's graphics kernel device,
 * what a child of it can see, and whether the AGC pool range is reservable.
 *
 * Run as a payload: websrv's payload launcher (POST /elfldr with pipe=1) streams
 * the probe's stdout back, and passes `args` as argv, so each step here is
 * opt-in and one send does one thing:
 *
 *   (no args)      the device question only - the known-good baseline
 *   args=child     plus: fork, and ask the same question from the child
 *   args=pool      plus: try to reserve a page inside the AGC pool range
 *
 * Why it is built this way: the first version of this probe added the fork and
 * a 2 MiB pool allocation to a working baseline in a single send, and the
 * console went down. Nothing was attributable, because an operation that takes
 * the console down takes the evidence with it. So now every risky step prints
 * what it is about to do *before* it does it, and the pool step asks for 64 KiB
 * and releases it again rather than seizing the whole range.
 *
 * Known before this probe, on firmware 10.01: a title has a working AGC device;
 * a payload has /dev/gc, opens it, and gets a genuine zero reply to the AGC
 * device's first request (_IOWR(0x81, 46, uint32_t) = 0xc004812e - a bare 46 is
 * a different request and answers ENOTTY); a Sony-managed helper gets ENOENT
 * for /dev/gc and EPERM when handed the owner's fd, before and after elevation.
 *
 * What each step would settle:
 *
 *   child  A forked child inherits its parent's devfs and jail. If it can open
 *          /dev/gc and get the zero reply, then "being a child process" is not
 *          what disqualifies a helper - its spawn context is, and parity is a
 *          spawn/mount change rather than a kernel patch. If the child is
 *          refused, the disqualification is per-process and deeper.
 *   pool   PS5PCEM's model of the guest device records that the driver's
 *          private pool "must remain at 0xfe0000000" with its small aperture at
 *          0xfe0200000. A page that can be reserved there and released says the
 *          range is not held away from this process.
 *
 * Nothing here writes to the device or submits work.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* Plain payload: declared here the way the title's child probe declares them. */
extern int64_t sceKernelGetDirectMemorySize(void);
extern int sceKernelAllocateDirectMemory(int64_t, int64_t, size_t, size_t, int, int64_t *);
extern int sceKernelReleaseDirectMemory(int64_t, size_t);

#define AGC_FIRST_REQUEST 0xc004812eUL
#define AGC_POOL_BASE 0xfe0000000LL
#define AGC_POOL_LIMIT 0xfe0200000LL
#define POOL_STEP_BYTES (64 * 1024)

static void line(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    putchar('\n');
    fflush(stdout);
    /* A second copy on standard error, which reaches klog when a capture is
     * armed: if an operation takes the console down, the elfldr stream can die
     * with it, and the last line it printed is the only evidence of what ran. */
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
    fflush(stderr);
}

static int requested(int argc, char **argv, const char *step)
{
    for (int i = 1; i < argc; ++i)
        if (!strcmp(argv[i], step))
            return 1;
    return 0;
}

/* The device question, asked from whichever process is calling. */
static void probe_device(const char *role)
{
    struct stat info;
    errno = 0;
    const int stat_status = stat("/dev/gc", &info);
    line("%s: stat /dev/gc -> %d errno=%d (%s)", role, stat_status, errno,
         errno ? strerror(errno) : "ok");

    errno = 0;
    const int fd = open("/dev/gc", O_RDWR);
    line("%s: open /dev/gc O_RDWR -> %d errno=%d (%s)", role, fd, errno,
         errno ? strerror(errno) : "ok");
    if (fd < 0)
        return;

    uint32_t reply = 0;
    errno = 0;
    const int zeroed = ioctl(fd, AGC_FIRST_REQUEST, &reply);
    line("%s:   #46 zeroed buffer -> %d errno=%d (%s) reply=%#x", role, zeroed, errno,
         errno ? strerror(errno) : "ok", reply);

    reply = 0xffffffffu;
    errno = 0;
    const int filled = ioctl(fd, AGC_FIRST_REQUEST, &reply);
    line("%s:   #46 0xff buffer   -> %d errno=%d (%s) reply=%#x", role, filled, errno,
         errno ? strerror(errno) : "ok", reply);
    close(fd);
}

static void step_child(void)
{
    line("payload: about to fork; the child inherits this devfs and jail");
    errno = 0;
    const pid_t child = fork();
    line("payload: fork returned %d errno=%d (%s)", (int)child, errno,
         errno ? strerror(errno) : "ok");
    if (child == 0) {
        line("child pid=%d uid=%d euid=%d", getpid(), getuid(), geteuid());
        probe_device("child");
        line("child done");
        fflush(stdout);
        _exit(0);
    }
    if (child > 0) {
        int status = 0;
        if (waitpid(child, &status, 0) == child)
            line("payload: child status=%#x", status);
        else
            line("payload: waitpid failed errno=%d (%s)", errno, strerror(errno));
    }
}

static void step_pool(void)
{
    line("payload: about to reserve %d bytes inside the AGC pool range %#llx-%#llx",
         POOL_STEP_BYTES, (unsigned long long)AGC_POOL_BASE, (unsigned long long)AGC_POOL_LIMIT);
    int64_t physical = -1;
    errno = 0;
    const int status = sceKernelAllocateDirectMemory(AGC_POOL_BASE, AGC_POOL_LIMIT, POOL_STEP_BYTES,
                                                     POOL_STEP_BYTES, 12, &physical);
    line("payload: reserve in pool range -> %d errno=%d (%s) physical=%#llx", status, errno,
         errno ? strerror(errno) : "ok", (unsigned long long)physical);
    if (status == 0 && physical > 0) {
        errno = 0;
        const int released = sceKernelReleaseDirectMemory(physical, POOL_STEP_BYTES);
        line("payload: release -> %d errno=%d (%s)", released, errno,
             errno ? strerror(errno) : "ok");
    }

    /* The same request outside the range, for contrast; freed the same way. */
    int64_t anywhere = -1;
    errno = 0;
    const int anywhere_status = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(),
                                                              POOL_STEP_BYTES, POOL_STEP_BYTES, 12,
                                                              &anywhere);
    line("payload: reserve %d bytes anywhere -> %d errno=%d (%s) physical=%#llx", POOL_STEP_BYTES,
         anywhere_status, errno, errno ? strerror(errno) : "ok", (unsigned long long)anywhere);
    if (anywhere_status == 0 && anywhere > 0)
        (void)sceKernelReleaseDirectMemory(anywhere, POOL_STEP_BYTES);
}

int main(int argc, char **argv)
{
    const int want_child = requested(argc, argv, "child");
    const int want_pool = requested(argc, argv, "pool");

    line("gcprobe payload pid=%d uid=%d euid=%d gid=%d egid=%d steps child=%d pool=%d",
         getpid(), getuid(), geteuid(), getgid(), getegid(), want_child, want_pool);

    DIR *dev = opendir("/dev");
    if (!dev) {
        line("opendir /dev -> -1 errno=%d (%s)", errno, strerror(errno));
    } else {
        struct dirent *entry;
        int total = 0, interesting = 0;
        while ((entry = readdir(dev)) != NULL) {
            ++total;
            const char *name = entry->d_name;
            if (strstr(name, "gc") || strstr(name, "gpu") || strstr(name, "agc") ||
                strstr(name, "dce") || strstr(name, "graph") || strstr(name, "video")) {
                line("dev: %s", name);
                ++interesting;
            }
        }
        line("/dev entries total=%d graphics-ish=%d", total, interesting);
        closedir(dev);
    }

    probe_device("payload");

    /* Fork first: it is the measurement that decides the parity question, and
     * the cheaper of the two to lose the console to if that is what happens. */
    if (want_child)
        step_child();
    if (want_pool)
        step_pool();

    line("gcprobe done");
    return 0;
}
