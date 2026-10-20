/* SPDX-License-Identifier: GPL-3.0-or-later */
/* cc -std=gnu11 tests/test_pw_wine_pages.c wine/ps5/pw_wine_pages.c -lpthread -o /tmp/pages && /tmp/pages */
#define _GNU_SOURCE
#include "../wine/ps5/pw_wine_pages.h"
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

/* A kernel that records what it was asked: the range is never left to the kernel, freed pages get a reservation
 * laid back, and a refused map leaves nothing taken (window pages in an emulator, 2026-10-14; record removed from the tree). */
static unsigned maps, lay_backs;
static uintptr_t last_map, last_lay_back;
static size_t last_map_bytes;
static int refuse_next, suspend_in_map, lay_back_fails;
static volatile sig_atomic_t suspended;
static void on_suspend(int signal_number) { (void)signal_number; suspended = 1; }
static int fake_map(void *context, uintptr_t address, size_t bytes, int protection, int fd)
{
    (void)context; (void)protection; (void)fd;
    /* Wine's SIGUSR1 (a suspend) waits until the lock is released: a holder never parks with it. */
    if (suspend_in_map) { raise(SIGUSR1); assert(!suspended); }
    if (refuse_next) { refuse_next = 0; errno = ENOMEM; return -1; }
    maps++; last_map = address; last_map_bytes = bytes;
    return 0;
}
static int fake_lay_back(void *context, uintptr_t address, size_t bytes)
{
    (void)context; (void)bytes;
    lay_backs++; last_lay_back = address;
    return lay_back_fails ? -1 : 0;
}

int main(void)
{
    const PwWinePagesOps ops = { NULL, fake_map, fake_lay_back };
    const uintptr_t base = 0xfbf8000000u;
    const size_t page = PW_WINE_PAGES_PAGE, bytes = 64 * page;

    /* No range: mmap(NULL) as before, and unmap is munmap. */
    int fd = memfd_create("pages", 0);
    assert(fd >= 0 && !ftruncate(fd, 3 * page));
    void *plain = pw_wine_pages_map(3 * page, PROT_READ | PROT_WRITE, fd);
    assert(plain != MAP_FAILED && !maps);
    ((volatile char *)plain)[0] = 1;
    pw_wine_pages_unmap(plain, 3 * page);
    assert(!lay_backs);

    assert(pw_wine_pages_init(&ops, base + 1, bytes) == -1);
    assert(pw_wine_pages_init(&ops, base, PW_WINE_PAGES_MAX_BYTES + page) == -1);
    assert(!pw_wine_pages_init(&ops, base, bytes));
    assert(pw_wine_pages_init(&ops, base, bytes) == -1);   /* one range per process */

    /* Mixed sizes, first-fit, page-aligned, inside the range, never overlapping. */
    uintptr_t a = (uintptr_t)pw_wine_pages_map(page + 1, PROT_READ, 7);
    uintptr_t b = (uintptr_t)pw_wine_pages_map(10 * page, PROT_READ, 7);
    uintptr_t c = (uintptr_t)pw_wine_pages_map(page, PROT_READ, 7);
    assert(a == base && last_map_bytes == page);
    assert(b == base + 2 * page && c == base + 12 * page && maps == 3);

    /* Freed pages get the reservation back and are reused. */
    pw_wine_pages_unmap((void *)b, 10 * page);
    assert(lay_backs == 1 && last_lay_back == b);
    uintptr_t d = (uintptr_t)pw_wine_pages_map(4 * page, PROT_READ, 7);
    assert(d == b);

    /* A refused map is laid back and takes nothing; a full range refuses without asking the kernel. */
    refuse_next = 1;
    unsigned laid = lay_backs;
    assert(pw_wine_pages_map(page, PROT_READ, 7) == MAP_FAILED && errno == ENOMEM);
    assert(lay_backs == laid + 1 && last_lay_back == b + 4 * page);
    uintptr_t e = (uintptr_t)pw_wine_pages_map(page, PROT_READ, 7);
    assert(e == b + 4 * page);
    /* A range that could not be laid back, after a free or a refused map, is lost: never mapped again. */
    lay_back_fails = 1;
    pw_wine_pages_unmap((void *)e, page);
    refuse_next = 1;
    assert(pw_wine_pages_map(page, PROT_READ, 7) == MAP_FAILED);
    lay_back_fails = 0;
    uintptr_t lost_next = (uintptr_t)pw_wine_pages_map(page, PROT_READ, 7);
    assert(lost_next == e + 2 * page);
    pw_wine_pages_unmap((void *)lost_next, page);
    unsigned before = maps;
    assert(pw_wine_pages_map(64 * page, PROT_READ, 7) == MAP_FAILED && errno == ENOSPC && maps == before);
    assert(pw_wine_pages_map(page, PROT_READ, -1) == MAP_FAILED && errno == EINVAL);

    /* An address outside the range is munmapped, not laid back. */
    before = lay_backs;
    void *outside = mmap(NULL, page, PROT_READ, MAP_SHARED, fd, 0);
    assert(outside != MAP_FAILED);
    pw_wine_pages_unmap(outside, page);
    assert(lay_backs == before);

    /* A suspend that lands while the lock is held runs once it is released, and the mask is as it was. */
    sigset_t mask;
    signal(SIGUSR1, on_suspend);
    suspend_in_map = 1;
    assert(pw_wine_pages_map(page, PROT_READ, 7) != MAP_FAILED && suspended);
    suspend_in_map = 0;
    assert(!pthread_sigmask(SIG_BLOCK, NULL, &mask) && !sigismember(&mask, SIGUSR1));

    /* Taken for the caller's own fixed mapping (an alias): first-fit, the kernel not asked; given back, the reservation
     * laid back and the run free again; a run not laid back stays taken (lost); too large or no range, 0. */
    before = maps;
    laid = lay_backs;
    uintptr_t t = pw_wine_pages_take(2 * page), u = pw_wine_pages_take(page + 1);
    assert(t >= base && t + 2 * page <= base + bytes && !((t - base) % page));
    assert(u >= base && u + 2 * page <= base + bytes && (u >= t + 2 * page || u + 2 * page <= t));
    assert(maps == before && lay_backs == laid);
    pw_wine_pages_give(t, 2 * page);
    assert(lay_backs == laid + 1 && last_lay_back == t && pw_wine_pages_take(2 * page) == t);
    lay_back_fails = 1;
    pw_wine_pages_give(t, 2 * page);
    lay_back_fails = 0;
    uintptr_t v = pw_wine_pages_take(2 * page);
    assert(lay_backs == laid + 2 && v && v != t && v != u);
    pw_wine_pages_give(v, 2 * page);
    pw_wine_pages_give(u, 2 * page);
    assert(pw_wine_pages_take(2 * page) == (u < v ? u : v));
    assert(!pw_wine_pages_take(bytes) && !pw_wine_pages_take(0));
    laid = lay_backs;
    pw_wine_pages_give(base + bytes, page);   /* outside the range: ignored */
    assert(lay_backs == laid);

    size_t size;
    assert(pw_wine_pages_base(&size) == base && size == bytes);
    pw_wine_pages_reset();
    assert(!pw_wine_pages_base(NULL) && !pw_wine_pages_take(page));
    /* A helper whose range was refused maps nothing where the kernel would put it. */
    pw_wine_pages_no_range();
    errno = 0;
    assert(pw_wine_pages_map(page, PROT_READ, fd) == MAP_FAILED && errno == ENOSPC);
    pw_wine_pages_reset();
    plain = pw_wine_pages_map(page, PROT_READ, fd);
    assert(plain != MAP_FAILED);
    pw_wine_pages_unmap(plain, page);
    close(fd);
    puts("shared pages: fixed in the helper's own range, first-fit, laid back when freed, refusals laid back and take "
         "nothing, a range not laid back lost, mmap(NULL) without a range but never in a helper whose range was refused, "
         "no suspend while the lock is held, runs taken for the caller's own mapping and given back laid back, passed");
}
