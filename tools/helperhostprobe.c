/*
 * helperhostprobe - could a process the console's payload loader starts host a Windows process?
 *
 * Every Windows process but the title's own runs in a helper the console starts for the title
 * (sceSystemServiceAddLocalProcess), and it runs seven at once (docs/WINDOWS_PROCESSES.md, "How many processes").
 * A payload elfldr starts is not the title's: it does not count against those seven. Whether it could carry Wine
 * is what this asks, one thing per send, in what a helper needs:
 *
 *   (no args)  who it is, whether it reads the runtime under /data, and the memory sizes the kernel reports
 *   memory     how much anonymous memory it gets, in 64 MiB steps up to 1 GiB, each touched and released
 *   dmem       whether it gets 256 MiB of direct memory, mapped and touched, then released
 *   ranges     whether it reserves the GPU broker's two host ranges at their fixed addresses, then releases them
 *   module     whether it loads (and unloads) the runtime's ntdll.prx, as the helper loader does
 *   channel    whether it and the title hand each other descriptors over a Unix-domain socket under /data: it
 *              listens, the title (native/proton_channel_probe.cpp) connects and sends a socket end and a shared
 *              page; this answers through the socket, reads the page, sends a socket end back and waits for the
 *              title's reply through it (tools/run-channel-test.py runs both halves)
 *
 * Sent with websrv's payload launcher (POST /elfldr, pipe=1), which streams stdout back
 * (tools/run-helperhostprobe.py). Each step prints what it is about to do before it does it, so a console that goes
 * down leaves the step that took it down on the wire (tools/gcprobe.c learned that the hard way).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

extern int64_t sceKernelGetDirectMemorySize(void);
extern int sceKernelAvailableFlexibleMemorySize(size_t *);
extern int sceKernelAllocateDirectMemory(int64_t, int64_t, size_t, size_t, int, int64_t *);
extern int sceKernelMapDirectMemory(void **, size_t, int, int, int64_t, size_t);
extern int sceKernelReleaseDirectMemory(int64_t, size_t);
extern int sceKernelReserveVirtualRange(void **, size_t, int, size_t);
extern int sceKernelMunmap(void *, size_t);
extern int sceKernelLoadStartModule(const char *, size_t, const void *, unsigned, const void *, int *);
extern int sceKernelStopUnloadModule(int, size_t, const void *, unsigned, const void *, int *);

#define RUNTIME "/data/ps5proton/runtime/ge11-7/lib/wine/x86_64-unix"
#define MIB (1024ull * 1024ull)

static void line(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    putchar('\n');
    fflush(stdout);
}

static int requested(int argc, char **argv, const char *step)
{
    for (int i = 0; i < argc; ++i)   /* websrv may pass args as the whole argv, with no program name first */
        if (argv[i] && !strcmp(argv[i], step)) return 1;
    return 0;
}

static void baseline(void)
{
    struct stat st;
    size_t flexible = 0;
    const int flexible_status = sceKernelAvailableFlexibleMemorySize(&flexible);
    line("memory: direct %lld MiB in all; flexible available %llu MiB (status %#x)",
         (long long)(sceKernelGetDirectMemorySize() / (int64_t)MIB), (unsigned long long)(flexible / MIB),
         (unsigned)flexible_status);
    const int stat_status = stat(RUNTIME "/ntdll.prx", &st);
    line("runtime: stat ntdll.prx -> %d errno=%d size=%lld", stat_status, stat_status ? errno : 0,
         stat_status ? 0ll : (long long)st.st_size);
    const int fd = open(RUNTIME "/ntdll.prx", O_RDONLY);
    unsigned char head[4] = {0};
    const ssize_t got = fd >= 0 ? read(fd, head, sizeof(head)) : -1;
    line("runtime: open -> %d errno=%d, first bytes %02x %02x %02x %02x (%zd read)", fd, fd < 0 ? errno : 0,
         head[0], head[1], head[2], head[3], got);
    if (fd >= 0) close(fd);
}

static void anonymous_memory(void)
{
    unsigned long long largest = 0;
    for (unsigned long long size = 64 * MIB; size <= 1024 * MIB; size += 64 * MIB) {
        line("memory: about to map and touch %llu MiB", size / MIB);
        void *block = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        if (block == MAP_FAILED) {
            line("memory: %llu MiB refused errno=%d", size / MIB, errno);
            break;
        }
        for (unsigned long long i = 0; i < size; i += 16384) ((volatile unsigned char *)block)[i] = (unsigned char)i;
        munmap(block, size);
        largest = size;
    }
    line("memory: largest anonymous block mapped, touched and released: %llu MiB", largest / MIB);
}

static void direct_memory(void)
{
    const size_t size = 256 * MIB;
    int64_t physical = 0;
    line("dmem: about to allocate %zu MiB of direct memory", size / MIB);
    const int allocated = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), size, 64 * 1024, 0,
                                                        &physical);
    line("dmem: allocate -> %#x", (unsigned)allocated);
    if (allocated) return;
    void *mapped = NULL;
    const int map_status = sceKernelMapDirectMemory(&mapped, size, PROT_READ | PROT_WRITE, 0, physical, 64 * 1024);
    line("dmem: map -> %#x at %p", (unsigned)map_status, mapped);
    if (!map_status) {
        for (size_t i = 0; i < size; i += 16384) ((volatile unsigned char *)mapped)[i] = (unsigned char)i;
        line("dmem: touched; unmap -> %d", sceKernelMunmap(mapped, size));
    }
    line("dmem: release -> %#x", (unsigned)sceKernelReleaseDirectMemory(physical, size));
}

static void host_ranges(void)
{
    /* The GPU broker's client ranges (src/pw_gpu_broker.h): a helper reserves them before its Vulkan module maps
     * anything, at these addresses, as the owner has them. */
    static const struct { uintptr_t base; size_t size; } ranges[] = {
        {0x240000000ull, 512 * MIB}, {0x3000000000ull, 16384 * MIB}};
    for (unsigned i = 0; i < 2; ++i) {
        void *at = (void *)ranges[i].base;
        line("ranges: about to reserve %zu MiB at %#lx", ranges[i].size / MIB, (unsigned long)ranges[i].base);
        const int status = sceKernelReserveVirtualRange(&at, ranges[i].size, 0x10 /* MAP_FIXED */, 0);
        line("ranges: reserve -> %#x at %p", (unsigned)status, at);
        if (!status) line("ranges: release -> %d", sceKernelMunmap(at, ranges[i].size));
    }
}

static void load_module(void)
{
    int result = 0;
    line("module: about to load " RUNTIME "/ntdll.prx");
    const int handle = sceKernelLoadStartModule(RUNTIME "/ntdll.prx", 0, NULL, 0, NULL, &result);
    line("module: load -> %#x (start result %d)", (unsigned)handle, result);
    if (handle > 0) {
        line("module: about to unload");
        line("module: unload -> %#x", (unsigned)sceKernelStopUnloadModule(handle, 0, NULL, 0, NULL, &result));
    }
}


#define CHANNEL_DIR "/data/ps5proton/run"
/* /data cannot hold a socket (bind: EOPNOTSUPP, channel-01): the socket goes where one can be bound, and its path
 * into this plain file under /data, which the title reads. */
#define CHANNEL_NOTE CHANNEL_DIR "/helper-channel.path"
static char channel_path[104];

static int readable(int fd, int milliseconds)
{
    struct pollfd p = {fd, POLLIN, 0};
    return poll(&p, 1, milliseconds) == 1 && (p.revents & POLLIN);
}

static void channel(void)
{
    mkdir(CHANNEL_DIR, 0777);
    chmod(CHANNEL_DIR, 0777);
    unlink(CHANNEL_NOTE);
    /* The file systems mounted, and which of them takes a socket. */
    struct statfs mounts[64];
    const int mounted = getfsstat(mounts, sizeof(mounts), MNT_NOWAIT);
    for (int i = 0; i < mounted && i < 64; ++i)
        line("channel: mount %s type %s", mounts[i].f_mntonname, mounts[i].f_fstypename);
    static const char *const candidates[] = {"/system_tmp", "/tmp", "/var/run", "/user/temp", "/dev/shm",
                                             "/data/ps5proton/run"};
    int listener = -1;
    for (unsigned i = 0; i < sizeof(candidates) / sizeof(candidates[0]) && listener < 0; ++i) {
        snprintf(channel_path, sizeof(channel_path), "%s/ps5proton-helper.sock", candidates[i]);
        unlink(channel_path);
        const int candidate = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un address;
        memset(&address, 0, sizeof(address));
        address.sun_family = AF_UNIX;
        strncpy(address.sun_path, channel_path, sizeof(address.sun_path) - 1);
        const int bound = candidate >= 0 ? bind(candidate, (struct sockaddr *)&address, sizeof(address)) : -1;
        line("channel: bind %s -> %d errno=%d", channel_path, bound, bound ? errno : 0);
        if (!bound && !listen(candidate, 1)) { chmod(channel_path, 0777); listener = candidate; }
        else if (candidate >= 0) close(candidate);
    }
    if (listener < 0) { line("channel: no file system here takes a socket"); return; }
    const int note = open(CHANNEL_NOTE, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    const int noted = note >= 0 && write(note, channel_path, strlen(channel_path)) == (ssize_t)strlen(channel_path);
    if (note >= 0) close(note);
    chmod(CHANNEL_NOTE, 0666);
    line("channel: path noted in " CHANNEL_NOTE " %d", noted);
    line("channel: listening at %s", channel_path);
    const int connection = readable(listener, 150000) ? accept(listener, NULL, NULL) : -1;
    line("channel: accept -> %d errno=%d", connection, connection < 0 ? errno : 0);
    close(listener);
    unlink(channel_path);
    unlink(CHANNEL_NOTE);
    if (connection < 0) return;

    /* The title's socket end and shared page. */
    char text[160] = {0}, control[CMSG_SPACE(sizeof(int) * 4)];
    memset(control, 0, sizeof(control));
    struct iovec io = {text, sizeof(text) - 1};
    struct msghdr message;
    memset(&message, 0, sizeof(message));
    message.msg_iov = &io;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    const ssize_t got = readable(connection, 20000) ? recvmsg(connection, &message, 0) : -1;
    int fds[4] = {-1, -1, -1, -1}, count = 0;
    for (struct cmsghdr *header = CMSG_FIRSTHDR(&message); got > 0 && header; header = CMSG_NXTHDR(&message, header))
        if (header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS)
            for (size_t i = 0; i < (header->cmsg_len - CMSG_LEN(0)) / sizeof(int) && count < 4; ++i)
                memcpy(&fds[count++], CMSG_DATA(header) + i * sizeof(int), sizeof(int));
    line("channel: received %zd bytes \"%s\" and %d descriptors", got, got > 0 ? text : "", count);
    int answered = 0, page_ok = 0;
    if (count == 2) {
        static const char answer[] = "payload: through the socket you sent";
        answered = send(fds[0], answer, sizeof(answer), 0) == (ssize_t)sizeof(answer);
        void *page = mmap(NULL, 65536, PROT_READ, MAP_SHARED, fds[1], 0);
        page_ok = page != MAP_FAILED && !strcmp((const char *)page, "ps5-proton channel shared memory");
        line("channel: answered through the title's socket %d; shared page mapped %d, pattern %s", answered,
             page != MAP_FAILED, page_ok ? "matches" : "differs");
        if (page != MAP_FAILED) munmap(page, 65536);
    }
    for (int i = 0; i < count; ++i) close(fds[i]);

    /* A socket end of this process's, to the title. */
    int pair[2] = {-1, -1}, reply_ok = 0;
    if (!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair)) {
        char back_control[CMSG_SPACE(sizeof(int))];
        memset(back_control, 0, sizeof(back_control));
        static const char note[] = "payload: a socket end of mine";
        struct iovec back_io = {(void *)note, sizeof(note)};
        struct msghdr back;
        memset(&back, 0, sizeof(back));
        back.msg_iov = &back_io;
        back.msg_iovlen = 1;
        back.msg_control = back_control;
        back.msg_controllen = sizeof(back_control);
        struct cmsghdr *header = CMSG_FIRSTHDR(&back);
        header->cmsg_level = SOL_SOCKET;
        header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(header), &pair[1], sizeof(int));
        const ssize_t sent = sendmsg(connection, &back, 0);
        close(pair[1]);
        char reply[160] = {0};
        reply_ok = sent > 0 && readable(pair[0], 20000) && recv(pair[0], reply, sizeof(reply) - 1, 0) > 0 &&
                   !strcmp(reply, "title: through the socket you sent");
        line("channel: sent a socket end %zd; the title's reply through it: %s", sent, reply_ok ? reply : "none");
        close(pair[0]);
    }
    char verdict[96];
    snprintf(verdict, sizeof(verdict), "payload verdict answer=%d page=%d reply=%d", answered, page_ok, reply_ok);
    send(connection, verdict, strlen(verdict) + 1, 0);
    line("channel: %s", verdict);
    close(connection);
}

int main(int argc, char **argv)
{
    line("helperhostprobe pid=%d uid=%d euid=%d gid=%d argc=%d", getpid(), getuid(), geteuid(), getgid(), argc);
    for (int i = 0; i < argc && i < 8; ++i) line("  argv[%d]=%s", i, argv[i] ? argv[i] : "(null)");
    baseline();
    if (requested(argc, argv, "memory")) anonymous_memory();
    if (requested(argc, argv, "dmem")) direct_memory();
    if (requested(argc, argv, "ranges")) host_ranges();
    if (requested(argc, argv, "module")) load_module();
    if (requested(argc, argv, "channel")) channel();
    line("helperhostprobe done");
    return 0;
}
