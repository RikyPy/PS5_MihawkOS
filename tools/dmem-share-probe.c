/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Can a second process map direct memory that a first process allocated from the application's pool?
 *
 * Why: a Windows helper's GPU host-visible buffers are shared with the title through physical shared memory
 * (native/pw_physical_shared.h), which consumes flexible backing, the ~989 MiB the shell counts as resident
 * (docs/PLAN.md, B3). Direct memory is the 11,712 MiB pool the helpers' Windows memory already uses. If a process
 * can map another's direct memory by its offset, shared buffers can come from the pool.
 *
 *   dmem-share-probe hold MIB     allocate MIB from the application pool (ForApp), map it, write a pattern, publish the
 *                                 offset in /data/ps5proton/dmem-share.txt, then wait for the other side and check its mark
 *   dmem-share-probe map [exec]   read the offset, map the same range in this process, check the pattern, write a mark;
 *                                 with exec, also ask the kernel for read+execute on the granted range (type granted-rx)
 *   dmem-share-probe exec PRX     one payload: does the kernel's own mprotect (sceKernelMprotect, system call 74) give
 *                                 execute here, per memory type: the application's direct memory RX and RWX, flexible
 *                                 anonymous memory, a shared file mapping on /data, and PRX's execute-only text RX?
 *   dmem-share-probe exec2 PID    the routes left when it does not (exec-grant-measure-2026-10-14-01: every type answered
 *                                 rc=0 with execute dropped): JIT shared memory (an executable view, its writable alias,
 *                                 one placed at a chosen address, and mprotect on the alias), then this process's
 *                                 credentials made the title's (PID) for one grant each, authid then caps, restored after.
 *   dmem-share-probe exec4        the kernel's own answers only (K1-K9 of the JIT design, klog/salvage review): built with
 *                                 the SDK's kernel_mprotect and kernel_set_vmem_protection wrapped to refuse, every map
 *                                 through libkernel's sceKernelMmap. exec2/exec3's jit-placed and jit-own-rwx results were
 *                                 the SDK's write (its libc mmap with PROT_EXEC calls kernel_mprotect), not the kernel's;
 *                                 both now map without execute through sceKernelMmap, then ask sceKernelMprotect, and
 *                                 every mode's DONE line says how many SDK writes were refused (sdk_writes).
 *   dmem-share-probe exec3        what a design on JIT shared memory needs: its own mapping read-write then mprotect
 *                                 read-execute, and read-write-execute at once; which pool a 256 MiB one draws from
 *                                 (flexible or direct, before and after); how many 16 KiB ones a process can hold (to 512).
 *
 * Why exec: helpers granted execute through the payload SDK, which writes the kernel's map entry without the map's lock
 * (kernel_set_vmem_protection), and now ask the kernel instead (wine/ps5/pw_wine_dmem_ps5.c). The system call is bounded by
 * each entry's max_protection, which the SDK never touched, and was never measured in a payload; elfldr itself uses the SDK
 * write for PF_X segments. One line per type gives the return code and the protection the kernel then reports; code runs
 * only where the kernel reports CPU execute (0x4), and module text is only read, where it reports read (0x1). A fault ends
 * the payload with a line naming the type; 60 s ends it in any case. 64 KiB of direct memory, no fork, no /dev/dmem.
 *
 * (An earlier version of this file also forked and mapped /dev/dmem*: that run crashed the console, 2026-10-12. Do not add either back.)
 * The kernel keeps a per-pid access list on each direct-memory range: without a grant the second process's map fails with
 * EACCES (0x8002000d, dmem-share-2026-10-12-01). The system compositor grants a client with sceKernelProtectDirectMemoryForPID
 * (ioctl 0xc018800f on the dmem device, read from libkernel_sys 10.01), so `hold` grants the pid `map` publishes.
 * Run the two as separate payloads (tools/run-dmem-share.py). No budget or kernel data is changed. */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <time.h>
#include <unistd.h>
#include "../wine/ps5/pw_wine_prx.h"

extern int sceKernelAllocateDirectMemoryForApp(int64_t, int64_t, size_t, size_t, int, int64_t *);
extern int sceKernelMapDirectMemory(void **, size_t, int, int, int64_t, size_t);
extern int sceKernelReleaseDirectMemory(int64_t, size_t);
extern int sceKernelMunmap(void *, size_t);
extern int sceKernelProtectDirectMemoryForPID(int64_t, size_t, int, int);
extern int64_t sceKernelGetDirectMemorySize(void);
extern int sceKernelGetResidentFmemCount(int, uint64_t *, uint64_t *);
extern int sceKernelMprotect(const void *, size_t, int);
extern int sceKernelQueryMemoryProtection(void *, void **, void **, uint32_t *);
extern int sceKernelLoadStartModule(const char *, size_t, const void *, unsigned, const void *, int *);
extern int sceKernelStopUnloadModule(int, size_t, const void *, unsigned, const void *, int *);
extern int sceKernelGetModuleInfo(int, void *);
extern int sceKernelJitCreateSharedMemory(const char *, size_t, int, int *);
extern int sceKernelAvailableFlexibleMemorySize(size_t *);
extern int sceKernelMmap(void *, size_t, int, int, int, off_t, void **);
extern int sceKernelReserveVirtualRange(void **, size_t, int, size_t);
extern int mincore(const void *, size_t, char *);
extern int sceKernelAvailableDirectMemorySize(int64_t, int64_t, size_t, int64_t *, size_t *);
extern int sceKernelJitCreateAliasOfSharedMemory(int, int, int *);
extern int sceKernelJitMapSharedMemory(int, int, void **);
extern uint64_t kernel_get_ucred_authid(pid_t);
extern int32_t kernel_set_ucred_authid(pid_t, uint64_t);
extern int32_t kernel_get_ucred_caps(pid_t, uint8_t[16]);
extern int32_t kernel_set_ucred_caps(pid_t, const uint8_t[16]);

#define OFFER "/data/ps5proton/dmem-share.txt"
#define DONE "/data/ps5proton/dmem-share-done.txt"
#define PIDFILE "/data/ps5proton/dmem-share-pid.txt"
#define EXEC_FILE "/data/ps5proton/exec-grant.bin"
#define EXEC "PS5_PROTON_EXEC_GRANT"
#define PAGE 16384u

/* Never the SDK's mprotect or mmap with PROT_EXEC here: that is the kernel write being replaced. */
typedef struct Grant { const char *type; void *address; int run, rc, query; uint32_t protection; } Grant;
static const unsigned char exec_code[] = {0xb8, 0x2a, 0, 0, 0, 0xc3}; /* mov eax, 42; ret */
static char fault_line[96] = EXEC " fault outside a case\n";
static void exec_fault(int sig)
{
    (void)sig;
    (void)write(1, fault_line, strlen(fault_line));
    _exit(3);
}
static void exec_deadline(int sig)
{
    static const char line[] = EXEC " deadline: 60 s passed\n";
    (void)sig;
    (void)write(1, line, sizeof(line) - 1);
    _exit(4);
}
static void exec_guard(void)
{
    signal(SIGSEGV, exec_fault);
    signal(SIGBUS, exec_fault);
    signal(SIGILL, exec_fault);
    signal(SIGALRM, exec_deadline);
    alarm(60);
}
/* Ask the kernel for protection on the page at address; record its answer and what the map then holds. */
static Grant grant(const char *type, void *address, size_t bytes, int protection, int run)
{
    Grant g = {type, address, run, 0, 0, 0};
    void *start = NULL, *end = NULL;
    g.rc = sceKernelMprotect(address, bytes, protection);
    g.query = sceKernelQueryMemoryProtection(address, &start, &end, &g.protection);
    printf(EXEC " type=%s protection=%d rc=%#x query=%#x prot=%#x\n", type, protection, (unsigned)g.rc, (unsigned)g.query,
           (unsigned)g.protection);
    fflush(stdout);
    return g;
}
/* Run the code (or read module text) only where the kernel reports it may be. */
static void use(const Grant *g)
{
    const uint32_t needed = g->run ? 0x4 : 0x1;
    int value;
    if (g->query || !(g->protection & needed)) {
        printf(EXEC " type=%s used=skipped\n", g->type);
        fflush(stdout);
        return;
    }
    snprintf(fault_line, sizeof(fault_line), EXEC " type=%s used=fault\n", g->type);
    value = g->run ? ((int (*)(void))(uintptr_t)g->address)() : *(volatile const unsigned char *)g->address;
    printf(EXEC " type=%s used=%s\n", g->type, !g->run || value == 42 ? "ok" : "wrong");
    fflush(stdout);
}

static void census(const char *phase)
{
    uint64_t resident = 0, mapped = 0;
    int status = sceKernelGetResidentFmemCount(getpid(), &resident, &mapped);
    printf("dmem-share %s pid=%d status=%#x resident_kib=%" PRIu64 " mapped_kib=%" PRIu64 "\n", phase, getpid(),
           (unsigned)status, resident * 16, mapped * 16);
    fflush(stdout);
}

static unsigned char pattern(size_t at) { return (unsigned char)((at * 131u + (at >> 14) * 7u + 5u) & 0xff); }

static int wait_for(const char *path, int seconds)
{
    for (int i = 0; i < seconds * 10; ++i) {
        if (access(path, F_OK) == 0) return 0;
        usleep(100000);
    }
    return -1;
}

static int hold(size_t bytes, int fresh)
{
    int64_t offset = -1;
    void *address = NULL;
    if (fresh) {
        unlink(OFFER);
        unlink(DONE);
        unlink(PIDFILE);
    }
    census("hold-before");
    int other = 0;
    if (wait_for(PIDFILE, 60)) { printf("PS5_PROTON_DMEM_SHARE FAIL side=hold: the other side never published its pid\n"); return 1; }
    {
        FILE *f = fopen(PIDFILE, "r");
        if (!f || fscanf(f, "%d", &other) != 1 || other <= 0) { printf("PS5_PROTON_DMEM_SHARE FAIL side=hold: unreadable pid\n"); return 1; }
        fclose(f);
    }
    int allocation = sceKernelAllocateDirectMemoryForApp(0, sceKernelGetDirectMemorySize(), bytes, PAGE, 0x0c, &offset);
    int mapping = allocation ? -1 : sceKernelMapDirectMemory(&address, bytes, 3, 0, offset, PAGE);
    printf("dmem-share hold allocation=%#x map=%#x offset=%" PRId64 " bytes=%zu\n", (unsigned)allocation, (unsigned)mapping,
           offset, bytes);
    fflush(stdout);
    if (allocation || mapping) return 1;
    volatile unsigned char *p = address;
    for (size_t at = 0; at < bytes; ++at) p[at] = pattern(at);
    census("hold-written");
    int grant = sceKernelProtectDirectMemoryForPID(offset, bytes, 0x33, other);
    printf("dmem-share hold: grant pid %d on offset %" PRId64 " for %zu bytes with prot 0x33: %#x\n", other, offset, bytes,
           (unsigned)grant);
    fflush(stdout);
    if (grant) {
        sceKernelMunmap(address, bytes);
        sceKernelReleaseDirectMemory(offset, bytes);
        printf("PS5_PROTON_DMEM_SHARE FAIL side=hold: grant refused\n");
        return 1;
    }
    FILE *file = fopen(OFFER ".tmp", "w");
    if (!file) { printf("dmem-share hold: cannot write the offer (errno %d)\n", errno); return 1; }
    fprintf(file, "%" PRId64 " %zu\n", offset, bytes);
    fclose(file);
    rename(OFFER ".tmp", OFFER);
    int waited = wait_for(DONE, 60);
    int mark_ok = 1;
    for (size_t at = bytes - PAGE; at < bytes; ++at)
        if (p[at] != (unsigned char)(pattern(at) ^ 0xa5)) { mark_ok = 0; break; }
    printf("dmem-share hold: other side %s, its mark %s\n", waited ? "never finished" : "finished",
           mark_ok ? "seen here (the memory is shared)" : "NOT seen here");
    census("hold-after");
    int unmap = sceKernelMunmap(address, bytes), release = sceKernelReleaseDirectMemory(offset, bytes);
    printf("PS5_PROTON_DMEM_SHARE %s side=hold mark=%d unmap=%#x release=%#x\n",
           !waited && mark_ok && !unmap && !release ? "PASS" : "FAIL", mark_ok, (unsigned)unmap, (unsigned)release);
    fflush(stdout);
    return !(!waited && mark_ok && !unmap && !release);
}

static int map(int exec)
{
    int64_t offset = -1;
    size_t bytes = 0;
    {
        FILE *f = fopen(PIDFILE ".tmp", "w");
        if (f) { fprintf(f, "%d\n", (int)getpid()); fclose(f); rename(PIDFILE ".tmp", PIDFILE); }
    }
    if (wait_for(OFFER, 40)) { printf("PS5_PROTON_DMEM_SHARE FAIL side=map: no offer\n"); return 1; }
    FILE *file = fopen(OFFER, "r");
    if (!file || fscanf(file, "%" SCNd64 " %zu", &offset, &bytes) != 2) {
        printf("PS5_PROTON_DMEM_SHARE FAIL side=map: unreadable offer\n");
        return 1;
    }
    fclose(file);
    census("map-before");
    void *address = NULL;
    int mapping = sceKernelMapDirectMemory(&address, bytes, 3, 0, offset, PAGE);
    printf("dmem-share map status=%#x offset=%" PRId64 " bytes=%zu\n", (unsigned)mapping, offset, bytes);
    fflush(stdout);
    int contents = 0, unmap = -1;
    if (!mapping) {
        volatile unsigned char *p = address;
        contents = 1;
        size_t wrong = 0;
        for (size_t at = 0; at < bytes; ++at)
            if (p[at] != pattern(at)) { contents = 0; ++wrong; }
        printf("dmem-share map: pattern %s (%zu wrong bytes of %zu)\n", contents ? "matches" : "DIFFERS", wrong, bytes);
        for (size_t at = bytes - PAGE; at < bytes; ++at) p[at] = (unsigned char)(pattern(at) ^ 0xa5);
        census("map-written");
        /* Before DONE: a fault here leaves the holder waiting out its 60 s, never releasing memory still mapped. */
        if (exec) {
            exec_guard();
            memcpy(address, exec_code, sizeof(exec_code));
            const Grant g = grant("granted-rx", address, PAGE, 5, 1);
            use(&g);
            alarm(0);
        }
    }
    FILE *done = fopen(DONE, "w");
    if (done) { fputs("done\n", done); fclose(done); }
    if (!mapping) unmap = sceKernelMunmap(address, bytes);
    printf("PS5_PROTON_DMEM_SHARE %s side=map status=%#x contents=%d unmap=%#x\n",
           !mapping && contents && !unmap ? "PASS" : "FAIL", (unsigned)mapping, contents, (unsigned)unmap);
    fflush(stdout);
    return !(!mapping && contents && !unmap);
}

/* Every grant is asked first and used after, so one fault cannot hide the other types' answers. */
static int exec_types(const char *prx)
{
    Grant g[5];   /* direct-rx, direct-rwx, flexible-rx, file-rx, module-rx */
    unsigned n = 0;
    exec_guard();

    int64_t offset = -1;
    void *direct = NULL;
    const int allocation = sceKernelAllocateDirectMemoryForApp(0, sceKernelGetDirectMemorySize(), 4 * PAGE, 4 * PAGE, 0x0c,
                                                               &offset);
    const int mapping = allocation ? -1 : sceKernelMapDirectMemory(&direct, 4 * PAGE, 3, 0, offset, PAGE);
    printf(EXEC " type=direct allocation=%#x map=%#x\n", (unsigned)allocation, (unsigned)mapping);
    fflush(stdout);
    if (!mapping) {
        memcpy(direct, exec_code, sizeof(exec_code));
        memcpy((char *)direct + PAGE, exec_code, sizeof(exec_code));
        g[n++] = grant("direct-rx", direct, PAGE, 5, 1);
        g[n++] = grant("direct-rwx", (char *)direct + PAGE, PAGE, 7, 1);
    }

    void *flexible = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    printf(EXEC " type=flexible map=%d errno=%d\n", flexible == MAP_FAILED ? -1 : 0, flexible == MAP_FAILED ? errno : 0);
    if (flexible != MAP_FAILED) {
        memcpy(flexible, exec_code, sizeof(exec_code));
        g[n++] = grant("flexible-rx", flexible, PAGE, 5, 1);
    }

    struct statfs fs = {0};
    const int fs_status = statfs("/data", &fs);
    const int fd = open(EXEC_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
    void *file = fd >= 0 && !ftruncate(fd, PAGE) && pwrite(fd, exec_code, sizeof(exec_code), 0) == (ssize_t)sizeof(exec_code)
                 ? mmap(NULL, PAGE, PROT_READ, MAP_SHARED, fd, 0) : MAP_FAILED;
    printf(EXEC " type=file statfs=%d noexec=%d fd=%d map=%d errno=%d\n", fs_status, !!(fs.f_flags & MNT_NOEXEC), fd,
           file == MAP_FAILED ? -1 : 0, file == MAP_FAILED ? errno : 0);
    if (file != MAP_FAILED) g[n++] = grant("file-rx", file, PAGE, 5, 1);

    int result = 0;
    const int module = sceKernelLoadStartModule(prx, 0, NULL, 0, NULL, &result);
    unsigned char info[PW_PRX_MODULE_INFO_BYTES] = {0};
    const uint64_t info_bytes = sizeof(info);
    PwPrxSegment segments[PW_PRX_MAX_SEGMENTS];
    uint32_t count = 0;
    char name[PW_PRX_MAX_NAME];
    memcpy(info, &info_bytes, sizeof(info_bytes));
    if (module > 0 && sceKernelGetModuleInfo(module, info) >= 0 && !pw_prx_parse_module_info(info, name, segments, &count))
        for (uint32_t i = 0; i < count; i++)
            if (segments[i].protection & PROT_EXEC) {
                g[n++] = grant("module-rx", (void *)segments[i].address, segments[i].size, PROT_READ | PROT_EXEC, 0);
                break;
            }
    printf(EXEC " type=module load=%#x segments=%u\n", (unsigned)module, count);
    fflush(stdout);

    for (unsigned i = 0; i < n; i++) use(&g[i]);
    alarm(0);

    if (module > 0) (void)sceKernelStopUnloadModule(module, 0, NULL, 0, NULL, &result);
    if (file != MAP_FAILED) munmap(file, PAGE);
    if (fd >= 0) { close(fd); unlink(EXEC_FILE); }
    if (flexible != MAP_FAILED) munmap(flexible, PAGE);
    if (!mapping) sceKernelMunmap(direct, 4 * PAGE);
    if (!allocation) sceKernelReleaseDirectMemory(offset, 4 * PAGE);
    printf(EXEC " DONE types=%u\n", n);
    fflush(stdout);
    return 0;
}

/* Query the protection the kernel reports at address without asking for any. */
static Grant seen(const char *type, void *address, int run)
{
    Grant g = {type, address, run, 0, 0, 0};
    void *start = NULL, *end = NULL;
    g.query = sceKernelQueryMemoryProtection(address, &start, &end, &g.protection);
    printf(EXEC " type=%s query=%#x prot=%#x\n", type, (unsigned)g.query, (unsigned)g.protection);
    fflush(stdout);
    return g;
}

/* A direct-memory page with the code in it, read-write; *offset is released by the caller. */
static void *direct_page(int64_t *offset)
{
    void *address = NULL;
    *offset = -1;
    if (sceKernelAllocateDirectMemoryForApp(0, sceKernelGetDirectMemorySize(), PAGE, PAGE, 0x0c, offset)) return NULL;
    if (sceKernelMapDirectMemory(&address, PAGE, 3, 0, *offset, PAGE)) {
        sceKernelReleaseDirectMemory(*offset, PAGE);
        *offset = -1;
        return NULL;
    }
    memcpy(address, exec_code, sizeof(exec_code));
    return address;
}

/* The SDK's kernel write, wrapped at link time (-Wl,--wrap=...): any call is refused and counted, so nothing in exec2,
 * exec3 or exec4 can reach it, and tools/run-dmem-share.py checks the disassembly for a direct call. */
static unsigned sdk_writes;
int __wrap_kernel_mprotect(int pid, unsigned long addr, unsigned long len, int prot)
{
    (void)pid; (void)addr; (void)len; (void)prot;
    ++sdk_writes;
    errno = EPERM;
    return -1;
}
int __wrap_kernel_set_vmem_protection(int pid, unsigned long addr, unsigned long len, int prot)
{
    (void)pid; (void)addr; (void)len; (void)prot;
    ++sdk_writes;
    errno = EPERM;
    return -1;
}
/* libkernel's mmap (system call 477 under Sony's checks), never the SDK's libc mmap. */
static void *kmap(void *address, size_t bytes, int protection, int flags, int fd, off_t offset)
{
    void *out = NULL;
    const int rc = sceKernelMmap(address, bytes, protection, flags, fd, offset, &out);
    if (rc) errno = (int)((unsigned)rc & 0xffff);
    return rc ? MAP_FAILED : out;
}
static int exec_routes(pid_t title)
{
    exec_guard();
    /* JIT shared memory: the SDK's crt sets the jitshm capability (crt/patch.c); the title was refused (PS5_Platform). */
    int fd = -1, alias = -1;
    void *rx = NULL, *rw = MAP_FAILED, *placed = MAP_FAILED, *reserve = MAP_FAILED;
    const int create = sceKernelJitCreateSharedMemory("proton-exec-probe", 4 * PAGE, 7, &fd);
    const int made_alias = create ? -1 : sceKernelJitCreateAliasOfSharedMemory(fd, 3, &alias);
    const int mapped = create ? -1 : sceKernelJitMapSharedMemory(fd, 5, &rx);
    if (!made_alias) rw = mmap(NULL, 4 * PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, alias, 0);
    printf(EXEC " type=jit create=%#x alias=%#x map=%#x rw=%d errno=%d\n", (unsigned)create, (unsigned)made_alias,
           (unsigned)mapped, rw == MAP_FAILED ? -1 : 0, rw == MAP_FAILED ? errno : 0);
    fflush(stdout);
    if (rw != MAP_FAILED) {
        memcpy(rw, exec_code, sizeof(exec_code));
        memcpy((char *)rw + PAGE, exec_code, sizeof(exec_code));
        memcpy((char *)rw + 2 * PAGE, exec_code, sizeof(exec_code));
    }
    if (!mapped && rw != MAP_FAILED) { const Grant g = seen("jit-rx", rx, 1); use(&g); }
    /* Executable memory where a program asks for it: the shared memory placed over a reservation. */
    if (!create && rw != MAP_FAILED) {
        reserve = mmap(NULL, 4 * PAGE, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (reserve != MAP_FAILED)
            placed = kmap(reserve, 4 * PAGE, PROT_READ, MAP_SHARED | MAP_FIXED, fd, 0);
        printf(EXEC " type=jit-placed map=%d errno=%d at_reserve=%d\n", placed == MAP_FAILED ? -1 : 0,
               placed == MAP_FAILED ? errno : 0, placed == reserve);
        fflush(stdout);
        if (placed != MAP_FAILED) { const Grant g = grant("jit-placed", (char *)placed + PAGE, PAGE, 5, 1); use(&g); }
        /* The writable alias turned executable in place, as Wine changes protection on memory it already maps. */
        const Grant g = grant("jit-alias-rx", (char *)rw + 2 * PAGE, PAGE, 5, 1);
        use(&g);
    }
    if (placed != MAP_FAILED) munmap(placed, 4 * PAGE);
    else if (reserve != MAP_FAILED) munmap(reserve, 4 * PAGE);
    if (rw != MAP_FAILED) munmap(rw, 4 * PAGE);
    if (!mapped) munmap(rx, 4 * PAGE);
    if (alias >= 0) close(alias);
    if (fd >= 0) close(fd);

    /* The credentials: one grant with the title's authid, one with its caps, each restored at once. */
    const uint64_t own = kernel_get_ucred_authid(getpid()), theirs = kernel_get_ucred_authid(title);
    uint8_t own_caps[16] = {0}, their_caps[16] = {0};
    const int caps_read = kernel_get_ucred_caps(getpid(), own_caps) | kernel_get_ucred_caps(title, their_caps);
    printf(EXEC " type=credentials authid_self=%#" PRIx64 " authid_title=%#" PRIx64 " caps_read=%d caps_differ=%d\n", own,
           theirs, caps_read, memcmp(own_caps, their_caps, 16) != 0);
    fflush(stdout);
    int64_t offset = -1;
    void *page = theirs ? direct_page(&offset) : NULL;
    if (page) {
        const int set = kernel_set_ucred_authid(getpid(), theirs);
        const Grant g = grant("authid-direct-rx", page, PAGE, 5, 1);
        const int restored = kernel_set_ucred_authid(getpid(), own);
        printf(EXEC " type=authid-direct-rx set=%d restored=%d now=%#" PRIx64 "\n", set, restored,
               kernel_get_ucred_authid(getpid()));
        fflush(stdout);
        use(&g);
        sceKernelMunmap(page, PAGE);
        sceKernelReleaseDirectMemory(offset, PAGE);
    }
    page = !caps_read ? direct_page(&offset) : NULL;
    if (page) {
        const int set = kernel_set_ucred_caps(getpid(), their_caps);
        const Grant g = grant("caps-direct-rx", page, PAGE, 5, 1);
        const int restored = kernel_set_ucred_caps(getpid(), own_caps);
        printf(EXEC " type=caps-direct-rx set=%d restored=%d\n", set, restored);
        fflush(stdout);
        use(&g);
        sceKernelMunmap(page, PAGE);
        sceKernelReleaseDirectMemory(offset, PAGE);
    }
    alarm(0);
    printf(EXEC " DONE routes sdk_writes=%u\n", sdk_writes);
    fflush(stdout);
    return 0;
}

static void pools(const char *phase)
{
    size_t flexible = 0, largest = 0;
    int64_t at = 0;
    const int f = sceKernelAvailableFlexibleMemorySize(&flexible);
    const int d = sceKernelAvailableDirectMemorySize(0, sceKernelGetDirectMemorySize(), PAGE, &at, &largest);
    printf(EXEC " type=pools phase=%s flexible_kib=%zu (%#x) direct_largest_kib=%zu (%#x)\n", phase, flexible >> 10,
           (unsigned)f, largest >> 10, (unsigned)d);
    fflush(stdout);
}

static int exec_jit_limits(void)
{
    exec_guard();
    int fd = -1;
    if (!sceKernelJitCreateSharedMemory("proton-exec-own", 2 * PAGE, 7, &fd)) {
        void *own = mmap(NULL, 2 * PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        printf(EXEC " type=jit-own-rw map=%d errno=%d\n", own == MAP_FAILED ? -1 : 0, own == MAP_FAILED ? errno : 0);
        fflush(stdout);
        if (own != MAP_FAILED) {
            memcpy(own, exec_code, sizeof(exec_code));
            const Grant g = grant("jit-own-rw-then-rx", own, PAGE, 5, 1);
            use(&g);
            munmap(own, 2 * PAGE);
        }
        void *rwx = kmap(NULL, 2 * PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        printf(EXEC " type=jit-own-rwx map=%d errno=%d\n", rwx == MAP_FAILED ? -1 : 0, rwx == MAP_FAILED ? errno : 0);
        fflush(stdout);
        if (rwx != MAP_FAILED) {
            memcpy(rwx, exec_code, sizeof(exec_code));
            const Grant g = grant("jit-own-rwx", rwx, PAGE, 7, 1);
            use(&g);
            munmap(rwx, 2 * PAGE);
        }
        close(fd);
    }
    pools("before-256mib");
    fd = -1;
    const int big = sceKernelJitCreateSharedMemory("proton-exec-big", 256u << 20, 7, &fd);
    void *rw = MAP_FAILED;
    int alias = -1;
    if (!big && !sceKernelJitCreateAliasOfSharedMemory(fd, 3, &alias))
        rw = mmap(NULL, 256u << 20, PROT_READ | PROT_WRITE, MAP_SHARED, alias, 0);
    printf(EXEC " type=jit-256mib create=%#x rw=%d\n", (unsigned)big, rw == MAP_FAILED ? -1 : 0);
    fflush(stdout);
    pools("created-256mib");
    if (rw != MAP_FAILED) {
        census("jit-before-touch");
        for (size_t at = 0; at < (256u << 20); at += PAGE) ((volatile char *)rw)[at] = 1;
        pools("touched-256mib");
        census("jit-touched");
        alarm(0);
        sleep(40);   /* ShellCore's FMEM line (every ~30 s) shows which pool holds it */
        alarm(60);
        census("jit-held");
        munmap(rw, 256u << 20);
    }
    if (alias >= 0) close(alias);
    if (fd >= 0) close(fd);
    pools("closed-256mib");
    static int many[512];
    unsigned made = 0;
    int last = 0;
    for (; made < 512; made++)
        if ((last = sceKernelJitCreateSharedMemory("proton-exec-many", PAGE, 7, &many[made]))) break;
    printf(EXEC " type=jit-count made=%u last=%#x\n", made, (unsigned)last);
    fflush(stdout);
    for (unsigned i = 0; i < made; i++) close(many[i]);
    pools("closed-many");
    alarm(0);
    printf(EXEC " DONE limits sdk_writes=%u\n", sdk_writes);
    fflush(stdout);
    return 0;
}

static int jit_fd(size_t bytes)
{
    int fd = -1;
    return sceKernelJitCreateSharedMemory("proton-exec4", bytes, 7, &fd) ? -1 : fd;
}
static void line(const char *text)
{
    printf(EXEC " %s\n", text);
    fflush(stdout);
}
static uint64_t now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}
static const unsigned char code43[] = {0xb8, 0x2b, 0, 0, 0, 0xc3}; /* mov eax, 43; ret */

/* K10: does taking write away from an RWX JIT mapping stop writes? (vm-protect-probe E7 on the JIT build: a write to
 * code under write-watch, re-protected RX after RWX, was not seen; exec-kernel run K2 measured only RX<->RW.) */
static sigjmp_buf k10_jump;
static void k10_fault(int sig) { (void)sig; siglongjmp(k10_jump, 1); }
static int k10_write_faults(volatile unsigned char *at)
{
    struct sigaction old_segv, old_bus, catch = {0};
    catch.sa_handler = k10_fault;
    sigemptyset(&catch.sa_mask);
    sigaction(SIGSEGV, &catch, &old_segv);
    sigaction(SIGBUS, &catch, &old_bus);
    int faulted = 1;
    if (!sigsetjmp(k10_jump, 1)) { at[200] = 0x77; faulted = 0; }
    sigaction(SIGSEGV, &old_segv, NULL);
    sigaction(SIGBUS, &old_bus, NULL);
    return faulted;
}
static void k10(void)
{
    static const struct { const char *name; int from, to; } steps[] = {
        {"7-to-5", 7, 5}, {"7-to-1", 7, 1}, {"7-to-3", 7, 3}, {"3-to-5", 3, 5}, {"5-to-7-to-5", 7, 5}};
    char text[200];
    for (unsigned i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        int fd = jit_fd(PAGE);
        void *m = fd >= 0 ? kmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) : MAP_FAILED;
        if (fd >= 0) close(fd);
        if (m == MAP_FAILED) { line("type=k10 map-failed"); continue; }
        memcpy(m, exec_code, sizeof(exec_code));
        if (i == 4) (void)sceKernelMprotect(m, PAGE, 5);
        const int a = sceKernelMprotect(m, PAGE, steps[i].from), b = sceKernelMprotect(m, PAGE, steps[i].to);
        void *s0, *s1; uint32_t prot = 0;
        (void)sceKernelQueryMemoryProtection(m, &s0, &s1, &prot);
        const int faulted = k10_write_faults(m);
        snprintf(text, sizeof(text), "type=k10 step=%s rc=%#x/%#x prot=%#x write_faulted=%d expected=%d", steps[i].name,
                 (unsigned)a, (unsigned)b, (unsigned)prot, faulted, !(steps[i].to & 2));
        line(text);
        munmap(m, PAGE);
    }
}

static int exec_kernel(const char *prx)
{
    char text[200];
    exec_guard();
    k10();
    /* K1: the main fd mapped RW, then RWX by the kernel. */
    int fd = jit_fd(2 * PAGE);
    void *m = fd >= 0 ? kmap(NULL, 2 * PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) : MAP_FAILED;
    snprintf(text, sizeof(text), "type=k1-map fd=%d map=%d errno=%d", fd, m == MAP_FAILED ? -1 : 0, errno);
    line(text);
    if (m != MAP_FAILED) {
        memcpy(m, exec_code, sizeof(exec_code));
        const Grant g = grant("k1-rwx", m, PAGE, 7, 1);
        use(&g);
        if ((g.protection & 7) == 7) {
            memcpy(m, code43, sizeof(code43));
            snprintf(fault_line, sizeof(fault_line), EXEC " type=k1-rewrite used=fault\n");
            snprintf(text, sizeof(text), "type=k1-rewrite value=%d", ((int (*)(void))(uintptr_t)m)());
            line(text);
        }
        /* K2: RX -> RW -> RX regains execute, 1000 toggles each queried, then the same after close(fd). */
        for (int round = 0; round < 2; round++) {
            if (round) { close(fd); fd = -1; }
            unsigned bad = 0;
            const uint64_t t0 = now_us();
            for (int i = 0; i < 1000; i++) {
                void *a, *b; uint32_t prot = 0;
                if (sceKernelMprotect(m, PAGE, i & 1 ? 3 : 5) || sceKernelQueryMemoryProtection(m, &a, &b, &prot) ||
                    (prot & 7) != (uint32_t)(i & 1 ? 3 : 5)) bad++;
            }
            const uint64_t t1 = now_us();
            (void)sceKernelMprotect(m, PAGE, 5);
            snprintf(fault_line, sizeof(fault_line), EXEC " type=k2 used=fault\n");
            const int value = ((int (*)(void))(uintptr_t)m)();
            snprintf(text, sizeof(text), "type=k2 after_close=%d toggles=1000 wrong=%u us_per_toggle=%.1f value=%d", round, bad,
                     (double)(t1 - t0) / 1000.0, value);
            line(text);
        }
        munmap(m, 2 * PAGE);
    }
    if (fd >= 0) close(fd);

    /* K3: a live direct page holding code becomes JIT memory at the same address, contents kept. pwrite into a JIT
     * object is refused (exec-kernel-2026-10-14-01), so the copy goes through a temporary read-write mapping. */
    int64_t offset = -1;
    void *page = direct_page(&offset);
    if (page) {
        memset((char *)page + 64, 0x5a, 64);
        const int ro = sceKernelMprotect(page, PAGE, PROT_READ);
        fd = jit_fd(PAGE);
        errno = 0;
        const ssize_t pwrote = fd >= 0 ? pwrite(fd, page, PAGE, 0) : -1;
        const int pwrite_errno = errno;
        void *scratch = fd >= 0 ? kmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) : MAP_FAILED;
        if (scratch != MAP_FAILED) { memcpy(scratch, page, PAGE); munmap(scratch, PAGE); }
        void *over = scratch != MAP_FAILED ? kmap(page, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) : MAP_FAILED;
        const int release = over == page ? sceKernelReleaseDirectMemory(offset, PAGE) : -1;
        const int intact = over == page && ((unsigned char *)page)[64] == 0x5a && ((unsigned char *)page)[127] == 0x5a &&
                           !memcmp(page, exec_code, sizeof(exec_code));
        snprintf(text, sizeof(text), "type=k3 ro=%#x fd=%d pwrite=%zd errno=%d scratch=%d same_address=%d release=%#x intact=%d",
                 (unsigned)ro, fd, pwrote, pwrite_errno, scratch != MAP_FAILED, over == page, (unsigned)release, intact);
        line(text);
        if (over == page && intact) {
            const Grant g = grant("k3-rx", page, PAGE, 5, 1);
            use(&g);
        }
        if (over == page) munmap(page, PAGE);
        else { sceKernelMunmap(page, PAGE); sceKernelReleaseDirectMemory(offset, PAGE); }
        if (fd >= 0) close(fd);
    }

    /* K4: MAP_FIXED over a reservation, an anonymous mapping and a private file mapping. Variants: the object sized to
     * the mapping or larger, and mapped once at a free address first (as K3 did) or not. */
    for (int target = 0; target < 3; target++)
        for (int variant = 0; variant < 3; variant++) {
            void *base = NULL;
            int file = -1, made;
            if (target == 0) made = sceKernelReserveVirtualRange(&base, PAGE, 0, PAGE) ? -1 : 0;
            else if (target == 1) made = (base = kmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0)) == MAP_FAILED ? -1 : 0;
            else {
                file = open(EXEC_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
                made = file < 0 || ftruncate(file, PAGE) ? -1 : 0;
                if (!made) made = (base = kmap(NULL, PAGE, PROT_READ, MAP_PRIVATE, file, 0)) == MAP_FAILED ? -1 : 0;
            }
            fd = jit_fd(variant == 1 ? 2 * PAGE : PAGE);   /* 0: sized, 1: larger, 2: sized and premapped */
            if (variant == 2 && fd >= 0) {
                void *first = kmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
                if (first != MAP_FAILED) munmap(first, PAGE);
            }
            void *over = MAP_FAILED;
            int rc = -1;
            if (!made && fd >= 0) rc = sceKernelMmap(base, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0, &over);
            int value = -1;
            if (!rc && over == base) {
                memcpy(over, exec_code, sizeof(exec_code));
                const Grant g = grant("k4", over, PAGE, 5, 1);
                if (!g.query && (g.protection & 4)) {
                    snprintf(fault_line, sizeof(fault_line), EXEC " type=k4 used=fault\n");
                    value = ((int (*)(void))(uintptr_t)over)();
                }
            }
            snprintf(text, sizeof(text), "type=k4 target=%s variant=%s made=%d rc=%#x same_address=%d value=%d",
                     target == 0 ? "reserve" : target == 1 ? "anon" : "file",
                     variant == 0 ? "sized" : variant == 1 ? "larger" : "premapped", made, (unsigned)rc, !rc && over == base, value);
            line(text);
            if (!made) munmap(base, PAGE);
            if (fd >= 0) close(fd);
            if (file >= 0) { close(file); unlink(EXEC_FILE); }
        }

    /* K5: pwrite/pread round-trip; a new object reads zeros. */
    fd = jit_fd(PAGE);
    if (fd >= 0) {
        static unsigned char out[PAGE], in[PAGE];
        for (unsigned i = 0; i < PAGE; i++) out[i] = (unsigned char)(i * 7 + 1);
        errno = 0;
        const ssize_t zero_read = pread(fd, in, PAGE, 0);
        const int read_errno = errno;
        int zeros = 1;
        for (unsigned i = 0; i < PAGE; i++) if (in[i]) { zeros = 0; break; }
        const ssize_t w = pwrite(fd, out, PAGE, 0), r = pread(fd, in, PAGE, 0);
        snprintf(text, sizeof(text), "type=k5 zero_read=%zd errno=%d zeros=%d write=%zd read=%zd equal=%d", zero_read,
                 read_errno, zeros, w, r, !memcmp(in, out, PAGE));
        line(text);
        close(fd);
    }

    /* K6: the fd closed, the code still runs; munmap of the last mapping gives the memory back. */
    census("k6-before");
    fd = jit_fd(64 * PAGE);
    m = fd >= 0 ? kmap(NULL, 64 * PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) : MAP_FAILED;
    if (m != MAP_FAILED) {
        for (unsigned i = 0; i < 64; i++) memcpy((char *)m + i * PAGE, exec_code, sizeof(exec_code));
        close(fd); fd = -1;
        census("k6-touched-closed");
        (void)sceKernelMprotect(m, 64 * PAGE, 5);
        snprintf(fault_line, sizeof(fault_line), EXEC " type=k6 used=fault\n");
        snprintf(text, sizeof(text), "type=k6 value=%d", ((int (*)(void))(uintptr_t)((char *)m + 63 * PAGE))());
        line(text);
        munmap(m, 64 * PAGE);
        census("k6-unmapped");
    }
    if (fd >= 0) close(fd);

    /* K7: 4096 objects of 16 KiB, fd closed, each RX and queried. */
    static void *maps[4096];
    unsigned made = 0, executable = 0;
    for (; made < 4096; made++) {
        fd = jit_fd(PAGE);
        maps[made] = fd >= 0 ? kmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) : MAP_FAILED;
        if (fd >= 0) close(fd);
        if (maps[made] == MAP_FAILED) break;
        ((volatile char *)maps[made])[0] = 1;
        void *a, *b; uint32_t prot = 0;
        if (!sceKernelMprotect(maps[made], PAGE, 5) && !sceKernelQueryMemoryProtection(maps[made], &a, &b, &prot) && (prot & 4))
            executable++;
    }
    census("k7-held");
    for (unsigned i = 0; i < made; i++) munmap(maps[i], PAGE);
    census("k7-unmapped");
    snprintf(text, sizeof(text), "type=k7 made=%u executable=%u", made, executable);
    line(text);

    /* K8: execute asked at map time, through the kernel only. */
    void *anon = kmap(NULL, PAGE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (anon != MAP_FAILED) { const Grant g = seen("k8-anon-rwx-map", anon, 1); (void)g; munmap(anon, PAGE); }
    else line("type=k8-anon-rwx-map refused");
    offset = -1;
    void *direct = NULL;
    if (!sceKernelAllocateDirectMemoryForApp(0, sceKernelGetDirectMemorySize(), PAGE, PAGE, 0x0c, &offset)) {
        const int rc = sceKernelMapDirectMemory(&direct, PAGE, 7, 0, offset, PAGE);
        snprintf(text, sizeof(text), "type=k8-direct-map7 rc=%#x", (unsigned)rc);
        line(text);
        if (!rc) { (void)seen("k8-direct-map7", direct, 1); sceKernelMunmap(direct, PAGE); }
        sceKernelReleaseDirectMemory(offset, PAGE);
    }

    /* K9: a PRX with no protection call: its text stays execute-only; mlock and mincore on it. */
    int result = 0;
    const int module = sceKernelLoadStartModule(prx, 0, NULL, 0, NULL, &result);
    unsigned char info[PW_PRX_MODULE_INFO_BYTES] = {0};
    const uint64_t info_bytes = sizeof(info);
    PwPrxSegment segments[PW_PRX_MAX_SEGMENTS];
    uint32_t count = 0;
    char name[PW_PRX_MAX_NAME];
    memcpy(info, &info_bytes, sizeof(info_bytes));
    if (module > 0 && sceKernelGetModuleInfo(module, info) >= 0 && !pw_prx_parse_module_info(info, name, segments, &count))
        for (uint32_t i = 0; i < count; i++)
            if (segments[i].protection & PROT_EXEC) {
                void *a, *b; uint32_t prot = 0;
                const int locked = mlock(segments[i].address, segments[i].size);
                static char residency[4096];
                const size_t pages = (segments[i].size + PAGE - 1) / PAGE;
                const int core = pages <= sizeof(residency) ? mincore(segments[i].address, segments[i].size, residency) : -1;
                unsigned resident = 0;
                if (!core) for (size_t p = 0; p < pages; p++) resident += residency[p] & 1;
                (void)sceKernelQueryMemoryProtection((void *)segments[i].address, &a, &b, &prot);
                snprintf(text, sizeof(text), "type=k9 mlock=%d errno=%d mincore=%d resident=%u of %zu prot=%#x", locked,
                         locked ? errno : 0, core, resident, pages, (unsigned)prot);
                line(text);
                if (!locked) munlock(segments[i].address, segments[i].size);
                break;
            }
    if (module > 0) (void)sceKernelStopUnloadModule(module, 0, NULL, 0, NULL, &result);
    alarm(0);
    snprintf(text, sizeof(text), "DONE kernel sdk_writes=%u", sdk_writes);
    line(text);
    return 0;
}

int main(int argc, char **argv)
{
    for (int i = 0; i < argc; ++i) {
        if (!strcmp(argv[i], "hold") && i + 1 < argc) {
            unsigned long mib = strtoul(argv[i + 1], NULL, 10);
            if (!mib || mib > 512) return 2;
            return hold((size_t)mib << 20, !(i + 2 < argc && !strcmp(argv[i + 2], "keep")));
        }
        if (!strcmp(argv[i], "map")) return map(i + 1 < argc && !strcmp(argv[i + 1], "exec"));
        if (!strcmp(argv[i], "exec") && i + 1 < argc) return exec_types(argv[i + 1]);
        if (!strcmp(argv[i], "exec3")) return exec_jit_limits();
        if (!strcmp(argv[i], "exec4") && i + 1 < argc) return exec_kernel(argv[i + 1]);
        if (!strcmp(argv[i], "exec2") && i + 1 < argc) return exec_routes((pid_t)strtol(argv[i + 1], NULL, 10));
    }
    return 2;
}
