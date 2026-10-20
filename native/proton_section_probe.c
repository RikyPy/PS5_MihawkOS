/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/pw_memory_file.h"
#include "../wine/ps5/pw_wine_section.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
extern int sceKernelReserveVirtualRange(void **, size_t, int, size_t);
extern int sceKernelMunmap(void *, size_t);
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
static int unmap(void *c, uintptr_t at, size_t n)
{
    (void)c;
    return munmap((void *)at, n);
}
static int reserve(void *c, uintptr_t at, size_t n)
{
    (void)c;
    void *p = (void *)at;
    return sceKernelReserveVirtualRange(&p, n, 0x90, 16384) || p != (void *)at;
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
static int worker_failures;
static void *worker(void *arg)
{
    uintptr_t i = (uintptr_t)arg;
    const size_t offset = ((size_t)8 << 30) + i * 65536;
    for (unsigned j = 1; j <= 12; j++)
    {
        full[offset] = j;
        if (alias[offset] != j)
        {
            __atomic_add_fetch(&worker_failures, 1, __ATOMIC_RELAXED);
            return NULL;
        }
        alias[offset + 16384] = (unsigned char)(j + 1);
        if (full[offset + 16384] != j + 1)
        {
            __atomic_add_fetch(&worker_failures, 1, __ATOMIC_RELAXED);
            return NULL;
        }
    }
    return NULL;
}
#define CHECK(expression)                                                                               \
    do                                                                                                  \
    {                                                                                                   \
        if (!(expression))                                                                              \
        {                                                                                               \
            printf("SECTION_NATIVE FAIL line=%d errno=%d\n", __LINE__, errno);                          \
            goto done;                                                                                  \
        }                                                                                               \
    } while (0)
int main(void)
{
    PwWineSectionOps ops = {NULL, 16384, retain, closed, mapped, protect, flush, unmap, reserve};
    PwWineSectionView a = {0}, b = {0}, overlap = {0};
    int a_added = 0, b_added = 0, fd = -1, created = 0, passed = 0, have_segv = 0, have_bus = 0,
        arena_reserved = 0;
    struct sigaction act = {0}, old_bus = {0}, old_segv = {0};
    char path[100];
    snprintf(path, sizeof(path), "/data/ps5proton/run/section-pager-probe-%d", getpid());
    size_t n = (size_t)13824 << 20, arena_n = (size_t)64 << 30;
    void *arena = (void *)UINT64_C(0x8000000000);
    CHECK(!pw_wine_section_init(&pager, &ops, pages, 4));
    if (sceKernelReserveVirtualRange(&arena, arena_n, 0x90, 65536))
    {
        puts("SECTION_NATIVE FAIL stage=reserve");
        return 1;
    }
    arena_reserved = 1;
    CHECK(arena == (void *)UINT64_C(0x8000000000));
    first_base = (uintptr_t)arena;
    first_size = n;
    full = arena;
    alias = (void *)((uintptr_t)arena + ((size_t)32 << 30));
    fd = pw_memory_file_alloc(path, n);
    created = fd >= 0;
    CHECK(created);
    CHECK(pw_memory_file_collect(path) == 0);
    /* SCM_RIGHTS itself retains the shared lease while the packet is queued. */
    int pair[2];
    CHECK(!socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair));
    char value = 1, space[CMSG_SPACE(sizeof(int))] = {0};
    struct iovec io = {&value, 1};
    struct msghdr message = {
        .msg_iov = &io, .msg_iovlen = 1, .msg_control = space, .msg_controllen = sizeof(space)};
    struct cmsghdr *header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(header), &fd, sizeof(fd));
    CHECK(sendmsg(pair[0], &message, 0) == 1);
    CHECK(!close(fd));
    fd = -1;
    CHECK(pw_memory_file_collect(path) == 0);
    message.msg_controllen = sizeof(space);
    CHECK(recvmsg(pair[1], &message, 0) == 1);
    memcpy(&fd, CMSG_DATA(CMSG_FIRSTHDR(&message)), sizeof(fd));
    CHECK(!close(pair[0]));
    CHECK(!close(pair[1]));
    CHECK(!pw_wine_section_add(&pager, &a, (uintptr_t)full, n, fd, 0));
    a_added = 1;
    CHECK(!pw_wine_section_add(&pager, &b, (uintptr_t)alias, n, fd, 0));
    b_added = 1;
    CHECK(pw_wine_section_add(&pager, &overlap, (uintptr_t)full, 16384, fd, 0) < 0);
    {
        int old = fd;
        fd = -1;
        CHECK(!close(old));
    }
    act.sa_sigaction = fault;
    act.sa_flags = SA_SIGINFO;
    sigemptyset(&act.sa_mask);
    CHECK(!sigaction(SIGSEGV, &act, &old_segv));
    have_segv = 1;
    CHECK(!sigaction(SIGBUS, &act, &old_bus));
    have_bus = 1;
    CHECK(full[0] == 0 && full[n - 1] == 0 && alias[n - 1] == 0);
    full[n - 1] = 0x51;
    CHECK(alias[n - 1] == 0x51);
    alias[n - 1] = 0x71;
    CHECK(full[n - 1] == 0x71);
    pthread_t threads[8];
    unsigned started = 0;
    int thread_error = 0;
    for (uintptr_t i = 0; i < 8; i++)
    {
        int rc = pthread_create(&threads[i], NULL, worker, (void *)i);
        if (rc)
        {
            thread_error = rc;
            break;
        }
        started++;
    }
    for (unsigned i = 0; i < started; i++)
        if (pthread_join(threads[i], NULL))
            thread_error = 1;
    printf("SECTION_NATIVE threads=%u errors=%d\n", started, thread_error);
    fflush(stdout);
    CHECK(!thread_error && started == 8 && !worker_failures);
    CHECK(full[n - 1] == 0x71);
    PwWineSectionRetry r = {0};
    CHECK(!pw_wine_section_fault(&pager, (uintptr_t)full, 1, 2, 0, &r));
    first_protection = 1;
    CHECK(pw_wine_section_protect(&pager, (uintptr_t)full, n, 1) == 1);
    CHECK(full[n - 1] == 0x71);
    first_protection = 3;
    CHECK(pw_wine_section_protect(&pager, (uintptr_t)full, n, 3) == 1);
    pthread_mutex_lock(&mutex);
    int rc = pw_wine_section_remove(&pager, &a);
    pthread_mutex_unlock(&mutex);
    a_added = a.fd >= 0;
    CHECK(!rc);
    CHECK(alias[n - 1] == 0x71);
    CHECK(pw_memory_file_collect(path) == 0);
    pthread_mutex_lock(&mutex);
    rc = pw_wine_section_remove(&pager, &b);
    pthread_mutex_unlock(&mutex);
    b_added = b.fd >= 0;
    CHECK(!rc);
    CHECK(pw_memory_file_collect(path) == 1);
    puts("SECTION_NATIVE backing_lease=PASS queued=1 surviving_alias=1 collected=1");
    CHECK(!pager.views && !pager.count && pager.peak == 4 && pager.evictions > 0 && !pager.failures);
    passed = 1;
done:
    if (a_added && pw_wine_section_remove(&pager, &a))
        passed = 0;
    if (b_added && pw_wine_section_remove(&pager, &b))
        passed = 0;
    if (have_bus && sigaction(SIGBUS, &old_bus, NULL))
        passed = 0;
    if (have_segv && sigaction(SIGSEGV, &old_segv, NULL))
        passed = 0;
    int unmapped = arena_reserved ? sceKernelMunmap(arena, arena_n) : 0,
        closed_fd = fd >= 0 ? close(fd) : 0;
    int unlinked = 0;
    if (created && !pager.views)
    {
        if (pw_memory_file_collect(path) < 0)
            unlinked = 1;
        else
            unlinked = rmdir(path);
    }
    if (unmapped || closed_fd || unlinked || pager.views)
        passed = 0;
    printf("SECTION_NATIVE %s aliases=2 virtual_bytes=%zu threads=8 cache_pages=4 faults=%llu "
           "evictions=%llu cleanup=%d stage=%u failures=%llu\n",
           passed ? "PASS" : "FAIL", n, (unsigned long long)pager.faults,
           (unsigned long long)pager.evictions, !unmapped && !closed_fd && !unlinked && !pager.views,
           pager.failed_stage, (unsigned long long)pager.failures);
    return !passed;
}
