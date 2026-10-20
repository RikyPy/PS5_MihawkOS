/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "../wine/ps5/pw_wine_section.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
static PwWineSection pager;
static PwWineSectionPage pages[4];
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local PwWineSectionRetry retry;
static uintptr_t first_base;
static size_t first_size;
static unsigned first_protection = 3;
static int retain(void *c, int fd)
{
    (void)c;
    return dup(fd);
}
static int closed(void *c, int fd)
{
    (void)c;
    return close(fd);
}
static int mapped(void *c, uintptr_t at, size_t n, unsigned prot, int fd, uint64_t off)
{
    (void)c;
    return mmap((void *)at, n, (int)prot, MAP_SHARED | MAP_FIXED, fd, (off_t)off) != (void *)at;
}
static int protect(void *c, uintptr_t at, size_t n, unsigned prot)
{
    (void)c;
    return mprotect((void *)at, n, (int)prot);
}
static int flush(void *c, uintptr_t at, size_t n)
{
    (void)c;
    return msync((void *)at, n, MS_SYNC);
}
static unsigned unmaps, reserves, lays;
static int unmap(void *c, uintptr_t at, size_t n)
{
    (void)c;
    unmaps++;
    return munmap((void *)at, n);
}
static int reserve(void *c, uintptr_t at, size_t n)
{
    (void)c;
    reserves++;
    return mmap((void *)at, n, PROT_NONE,
                MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0) != (void *)at;
}
/* A guard laid over the page in one call: a fixed mapping replaces it. */
static int lay(void *c, uintptr_t at, size_t n)
{
    (void)c;
    lays++;
    return mmap((void *)at, n, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0) != (void *)at;
}
/* A mapping that is not Wine's inside the view's guards; the cache's map must never reach it. */
static uintptr_t foreign_at;
static unsigned foreign_checks, maps;
static int foreign(void *c, uintptr_t at, size_t n)
{
    (void)c;
    foreign_checks++;
    return foreign_at && at < foreign_at + 16384 && foreign_at < at + n;
}
static int counted_map(void *c, uintptr_t at, size_t n, unsigned prot, int fd, uint64_t off)
{
    maps++;
    assert(!foreign_at || at >= foreign_at + 16384 || foreign_at >= at + n);
    return mapped(c, at, n, prot, fd, off);
}
static void fault(int sig, siginfo_t *info, void *ctx)
{
    (void)ctx;
    int saved = errno;
    pthread_mutex_lock(&mutex);
    uintptr_t address = (uintptr_t)info->si_addr;
    unsigned prot = address >= first_base && address - first_base < first_size ? first_protection : 3;
    int rc = pw_wine_section_fault(&pager, address, prot, 1, sig == SIGBUS, &retry);
    pthread_mutex_unlock(&mutex);
    if (rc != 1)
        _exit(90 + (rc < 0));
    errno = saved;
}
static volatile unsigned char *full, *alias;
static void *worker(void *arg)
{
    uintptr_t i = (uintptr_t)arg;
    const size_t offset = ((size_t)8 << 30) + i * 65536;
    for (unsigned j = 1; j <= 12; j++)
    {
        full[offset] = j;
        assert(alias[offset] == j);
        alias[offset + 16384] = (unsigned char)(j + 1);
        assert(full[offset + 16384] == j + 1);
    }
    return NULL;
}
int main(void)
{
    PwWineSectionOps ops = {NULL, 16384, retain, closed, mapped, protect, flush, unmap, reserve, NULL, NULL};
    assert(!pw_wine_section_init(&pager, &ops, pages, 4));
    size_t n = (size_t)13824 << 20, arena_n = (size_t)64 << 30;
    void *arena = mmap(NULL, arena_n, PROT_NONE, MAP_ANONYMOUS | MAP_PRIVATE | MAP_NORESERVE, -1, 0);
    assert(arena != MAP_FAILED);
    first_base = (uintptr_t)arena;
    first_size = n;
    full = arena;
    alias = (void *)((uintptr_t)arena + ((size_t)32 << 30));
    char path[] = "/tmp/proton-section-host-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0 && !ftruncate(fd, n));
    PwWineSectionView a, b, overlap;
    assert(!pw_wine_section_add(&pager, &a, (uintptr_t)full, n, fd, 0));
    assert(!pw_wine_section_add(&pager, &b, (uintptr_t)alias, n, fd, 0));
    assert(pw_wine_section_add(&pager, &overlap, (uintptr_t)full, 16384, fd, 0) < 0);
    assert(!close(fd));
    struct sigaction act = {0}, old_bus, old_segv;
    act.sa_sigaction = fault;
    act.sa_flags = SA_SIGINFO;
    sigemptyset(&act.sa_mask);
    assert(!sigaction(SIGSEGV, &act, &old_segv));
    assert(!sigaction(SIGBUS, &act, &old_bus));
    assert(full[0] == 0 && full[n - 1] == 0 && alias[n - 1] == 0);
    full[n - 1] = 0x51;
    assert(alias[n - 1] == 0x51);
    alias[n - 1] = 0x71;
    assert(full[n - 1] == 0x71);
    pthread_t threads[8];
    for (uintptr_t i = 0; i < 8; i++)
        assert(!pthread_create(&threads[i], NULL, worker, (void *)i));
    for (unsigned i = 0; i < 8; i++)
        assert(!pthread_join(threads[i], NULL));
    assert(full[n - 1] == 0x71); /* pressure must preserve file contents */
    PwWineSectionRetry r = {0};
    assert(!pw_wine_section_fault(&pager, (uintptr_t)full, 1, 2, 0, &r));
    first_protection = 1;
    assert(pw_wine_section_protect(&pager, (uintptr_t)full, n, 1) == 1);
    assert(full[n - 1] == 0x71);
    first_protection = 3;
    assert(pw_wine_section_protect(&pager, (uintptr_t)full, n, 3) == 1);
    pthread_mutex_lock(&mutex);
    assert(!pw_wine_section_remove(&pager, &a));
    pthread_mutex_unlock(&mutex);
    assert(alias[n - 1] == 0x71);
    pthread_mutex_lock(&mutex);
    assert(!pw_wine_section_remove(&pager, &b));
    pthread_mutex_unlock(&mutex);
    assert(!pager.views && !pager.count && pager.peak == 4 && pager.evictions > 0 && !pager.failures);
    const unsigned long long evictions = pager.evictions;
    /* With a lay op an evicted page goes back to a guard in one call: no unmap, no reserve, no hole between them. */
    {
        PwWineSectionOps laid = ops;
        PwWineSectionView c;
        const int small = memfd_create("laid", 0);
        laid.lay = lay;
        assert(small >= 0 && !ftruncate(small, 8 * 16384));
        assert(!pw_wine_section_init(&pager, &laid, pages, 4));
        assert(!pw_wine_section_add(&pager, &c, (uintptr_t)full, 8 * 16384, small, 0));
        unmaps = reserves = lays = 0;
        for (unsigned i = 0; i < 8; i++) full[i * 16384] = (unsigned char)(i + 1);
        pthread_mutex_lock(&mutex);
        assert(!pw_wine_section_remove(&pager, &c));
        pthread_mutex_unlock(&mutex);
        assert(lays == 8 && pager.evictions == 8 && !unmaps && !reserves && !pager.failures);
        assert(!close(small));
    }
    /* The GPU client's mapping placed in an unmapped page of the view (late-library 2026-10-15): its fault maps nothing
     * there and fails (stage 9), the view stays usable, the page beside maps; one check per fault. */
    {
        PwWineSectionOps guarded = ops;
        PwWineSectionView g;
        PwWineSectionRetry r2 = {0};
        const int small = memfd_create("guarded", 0), radv = memfd_create("radv", 0);
        guarded.map = counted_map;
        guarded.foreign = foreign;
        assert(small >= 0 && !ftruncate(small, 8 * 16384) && radv >= 0 && !ftruncate(radv, 16384));
        assert(!pw_wine_section_init(&pager, &guarded, pages, 4));
        assert(!pw_wine_section_add(&pager, &g, (uintptr_t)full, 8 * 16384, small, 0));
        foreign_at = (uintptr_t)full + 3 * 16384;
        assert(mmap((void *)foreign_at, 16384, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, radv, 0) ==
               (void *)foreign_at);
        ((volatile unsigned char *)foreign_at)[0] = 0x5a;
        maps = foreign_checks = 0;
        assert(pw_wine_section_fault(&pager, foreign_at, 3, 1, 0, &r2) == -1 && pager.failed_stage == 9);
        assert(!g.failed && !maps && foreign_checks == 1 && !pager.count);
        assert(pw_wine_section_fault(&pager, foreign_at - 16384, 3, 1, 0, &r2) == 1 && maps == 1 && foreign_checks == 2);
        assert(pw_wine_section_fault_run(&pager, foreign_at - 2 * 16384, 3, 1, 0, &r2, 4) == 1 && maps == 2);
        assert(((volatile unsigned char *)foreign_at)[0] == 0x5a);
        assert(mmap((void *)foreign_at, 16384, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) ==
               (void *)foreign_at);
        foreign_at = 0;
        pthread_mutex_lock(&mutex);
        assert(!pw_wine_section_remove(&pager, &g));
        pthread_mutex_unlock(&mutex);
        assert(!close(small) && !close(radv));
    }
    assert(!sigaction(SIGSEGV, &old_segv, NULL));
    assert(!sigaction(SIGBUS, &old_bus, NULL));
    assert(!munmap(arena, arena_n));
    assert(!unlink(path));
    printf("SECTION_HOST PASS aliases=2 virtual_bytes=%zu threads=8 fault_cache_pages=4 faults=%llu "
           "evictions=%llu cleanup=1\n",
           n, (unsigned long long)pager.faults, evictions);
}
