/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pw_wine_pages.h"
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <sys/mman.h>

enum { PAGES = PW_WINE_PAGES_MAX_BYTES / PW_WINE_PAGES_PAGE };

static pthread_mutex_t pages_lock = PTHREAD_MUTEX_INITIALIZER;
static PwWinePagesOps pages_ops;
static uintptr_t pages_base;
static size_t pages_count;
static int pages_refused;   /* pw_wine_pages_no_range */
static uint64_t pages_used[PAGES / 64];

/* An execute fault's conversion takes the lock under virtual_mutex (pw_wine_dmem_ps5.c), so a holder is never suspended
 * or ended with it held: Wine's asynchronous handlers (the set patch 0923 defers) wait until it is released, as in
 * server_enter_uninterrupted_section. */
static void pages_enter(sigset_t *old)
{
    sigset_t block;
    sigemptyset(&block);
    sigaddset(&block, SIGUSR1);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGQUIT);
    pthread_sigmask(SIG_BLOCK, &block, old);
    pthread_mutex_lock(&pages_lock);
}
static void pages_leave(const sigset_t *old)
{
    pthread_mutex_unlock(&pages_lock);
    pthread_sigmask(SIG_SETMASK, old, NULL);
}

static int used(size_t page) { return (pages_used[page / 64] >> (page % 64)) & 1; }
static void mark(size_t first, size_t count, int in_use)
{
    for (size_t page = first; page < first + count; page++) {
        if (in_use) pages_used[page / 64] |= UINT64_C(1) << (page % 64);
        else pages_used[page / 64] &= ~(UINT64_C(1) << (page % 64));
    }
}

/* The first run of count free pages: its index, or (size_t)-1. Under the lock, with a range. */
static size_t first_free(size_t count)
{
    size_t run = 0;
    for (size_t page = 0; page < pages_count; page++) {
        if (used(page)) { run = 0; continue; }
        if (++run == count) return page + 1 - count;
    }
    return (size_t)-1;
}

/* [at, +count pages) laid back and free, or left taken (lost) when the reservation is not there. Under the lock: 1 when
 * the range is this one's, 0 when it is not. */
static int give_locked(uintptr_t at, size_t count)
{
    size_t first;
    if (!pages_base || at < pages_base || at >= pages_base + pages_count * PW_WINE_PAGES_PAGE) return 0;
    first = (at - pages_base) / PW_WINE_PAGES_PAGE;
    if (!((at - pages_base) % PW_WINE_PAGES_PAGE) && first + count <= pages_count &&
        !pages_ops.lay_back(pages_ops.context, at, count * PW_WINE_PAGES_PAGE))
        mark(first, count, 0);
    return 1;
}

int pw_wine_pages_init(const PwWinePagesOps *ops, uintptr_t base, size_t bytes)
{
    sigset_t old;
    int status = -1;

    pages_enter(&old);
    if (ops && ops->map && ops->lay_back && !pages_base && base && !(base % PW_WINE_PAGES_PAGE) &&
        bytes >= PW_WINE_PAGES_PAGE && bytes <= PW_WINE_PAGES_MAX_BYTES && !(bytes % PW_WINE_PAGES_PAGE)) {
        pages_ops = *ops;
        pages_base = base;
        pages_count = bytes / PW_WINE_PAGES_PAGE;
        memset(pages_used, 0, sizeof(pages_used));
        status = 0;
    }
    pages_leave(&old);
    return status;
}

void *pw_wine_pages_map(size_t bytes, int protection, int fd)
{
    const size_t count = (bytes + PW_WINE_PAGES_PAGE - 1) / PW_WINE_PAGES_PAGE;
    size_t first;
    uintptr_t at;
    sigset_t old;

    if (!bytes || fd < 0) { errno = EINVAL; return MAP_FAILED; }
    pages_enter(&old);
    if (!pages_base) {
        const int refused = pages_refused;
        pages_leave(&old);
        if (refused) {
            errno = ENOSPC;
            return MAP_FAILED;
        }
        return mmap(NULL, bytes, protection, MAP_SHARED, fd, 0);
    }
    if ((first = first_free(count)) == (size_t)-1) {
        pages_leave(&old);
        errno = ENOSPC;
        return MAP_FAILED;
    }
    at = pages_base + first * PW_WINE_PAGES_PAGE;
    if (pages_ops.map(pages_ops.context, at, count * PW_WINE_PAGES_PAGE, protection, fd)) {
        const int saved = errno ? errno : ENOMEM;
        if (pages_ops.lay_back(pages_ops.context, at, count * PW_WINE_PAGES_PAGE)) mark(first, count, 1);
        pages_leave(&old);
        errno = saved;
        return MAP_FAILED;
    }
    mark(first, count, 1);
    pages_leave(&old);
    return (void *)at;
}

void pw_wine_pages_unmap(void *address, size_t bytes)
{
    const uintptr_t at = (uintptr_t)address;
    const size_t count = (bytes + PW_WINE_PAGES_PAGE - 1) / PW_WINE_PAGES_PAGE;
    sigset_t old;

    if (!address || address == MAP_FAILED || !bytes) return;
    pages_enter(&old);
    if (give_locked(at, count)) {
        pages_leave(&old);
        return;
    }
    pages_leave(&old);
    munmap(address, bytes);
}

uintptr_t pw_wine_pages_take(size_t bytes)
{
    const size_t count = (bytes + PW_WINE_PAGES_PAGE - 1) / PW_WINE_PAGES_PAGE;
    uintptr_t at = 0;
    size_t first;
    sigset_t old;

    if (!bytes) return 0;
    pages_enter(&old);
    if (pages_base && (first = first_free(count)) != (size_t)-1) {
        mark(first, count, 1);
        at = pages_base + first * PW_WINE_PAGES_PAGE;
    }
    pages_leave(&old);
    return at;
}

void pw_wine_pages_give(uintptr_t address, size_t bytes)
{
    sigset_t old;

    if (!address || !bytes) return;
    pages_enter(&old);
    (void)give_locked(address, (bytes + PW_WINE_PAGES_PAGE - 1) / PW_WINE_PAGES_PAGE);
    pages_leave(&old);
}

uintptr_t pw_wine_pages_base(size_t *bytes)
{
    uintptr_t base;
    sigset_t old;

    pages_enter(&old);
    base = pages_base;
    if (bytes) *bytes = pages_count * PW_WINE_PAGES_PAGE;
    pages_leave(&old);
    return base;
}

void pw_wine_pages_no_range(void)
{
    sigset_t old;

    pages_enter(&old);
    pages_refused = 1;
    pages_leave(&old);
}

void pw_wine_pages_reset(void)
{
    sigset_t old;

    pages_enter(&old);
    pages_refused = 0;
    pages_base = 0;
    pages_count = 0;
    memset(pages_used, 0, sizeof(pages_used));
    pages_leave(&old);
}
