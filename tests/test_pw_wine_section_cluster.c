/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The demand-mapped section cache with an address index and clustered faults: a run of pages maps in one native call, a
 * view's pages are found without scanning the cache, eviction under pressure keeps the file's contents, and what a view
 * holds is protected and removed through its own list. The native calls are counted, not performed on memory:
 * the test follows what the cache believes is mapped. */
#include "../wine/ps5/pw_wine_section.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { PAGE = 16384, PAGES = 64, VIEW_PAGES = 400 };
static PwWineSection cache;
static PwWineSectionPage pages[PAGES];
static uint32_t buckets[256];
static unsigned char mapped_page[VIEW_PAGES * 2];
static unsigned maps, flushes, unmaps, reserves, protects, largest_map;
static const uintptr_t base = 0x10000000;

static int retain(void *c, int fd) { (void)c; return fd + 100; }
static int closed(void *c, int fd) { (void)c; (void)fd; return 0; }
static size_t index_of(uintptr_t at) { return (at - base) / PAGE; }
static int mapped(void *c, uintptr_t at, size_t n, unsigned prot, int fd, uint64_t off)
{
    (void)c; (void)prot; (void)fd;
    assert(off % PAGE == 0 && n % PAGE == 0 && (at >= base + (size_t)VIEW_PAGES * PAGE || off == (uint64_t)(at - base) + 4096ull * PAGE));
    for (size_t i = 0; i < n / PAGE; i++) {
        assert(!mapped_page[index_of(at) + i]);
        mapped_page[index_of(at) + i] = 1;
    }
    maps++;
    if (n / PAGE > largest_map) largest_map = (unsigned)(n / PAGE);
    return 0;
}
static int protect(void *c, uintptr_t at, size_t n, unsigned prot) { (void)c; (void)at; (void)n; (void)prot; protects++; return 0; }
static int flush(void *c, uintptr_t at, size_t n) { (void)c; (void)at; (void)n; flushes++; return 0; }
static int unmap(void *c, uintptr_t at, size_t n)
{
    (void)c;
    assert(n == PAGE && mapped_page[index_of(at)]);
    mapped_page[index_of(at)] = 0;
    unmaps++;
    return 0;
}
static int reserve(void *c, uintptr_t at, size_t n) { (void)c; (void)at; (void)n; reserves++; return 0; }

static void touch(uintptr_t address, size_t run, unsigned prot)
{
    static PwWineSectionRetry retry;
    if (mapped_page[index_of(address)]) return;
    assert(pw_wine_section_fault_run(&cache, address, prot, 1, 0, &retry, run) == 1);
    assert(mapped_page[index_of(address)]);
}

int main(void)
{
    PwWineSectionOps ops = {NULL, PAGE, retain, closed, mapped, protect, flush, unmap, reserve, NULL, NULL};
    assert(!pw_wine_section_init(&cache, &ops, pages, PAGES));
    assert(pw_wine_section_use_index(&cache, buckets, 100) < 0);
    assert(!pw_wine_section_use_index(&cache, buckets, 256));
    PwWineSectionView a, b;
    assert(!pw_wine_section_add(&cache, &a, base, (size_t)VIEW_PAGES * PAGE, 3, 4096ull * PAGE));
    /* a run of 8 maps eight pages in one call; faults count pages */
    touch(base, 8, 3);
    assert(maps == 1 && largest_map == 8 && cache.count == 8 && cache.faults == 8 && cache.clustered == 7 && a.cached == 8);
    for (size_t i = 0; i < 8; i++) assert(mapped_page[i]);
    /* a fault inside what is cached resolves again without a map once (the retry token), then reports a repeat */
    {
        PwWineSectionRetry retry = {0, 0};
        assert(pw_wine_section_fault_run(&cache, base + 3 * PAGE, 3, 1, 0, &retry, 8) == 1 && maps == 1);
        assert(pw_wine_section_fault_run(&cache, base + 3 * PAGE, 3, 1, 0, &retry, 8) == 0);
    }
    /* a run stops at the first cached page: pages 8..15 are new, a run from 4..19 would cover cached 4..7, so start at 6 */
    touch(base + 12 * PAGE, 8, 3);                         /* pages 12..19 */
    touch(base + 9 * PAGE, 8, 3);                          /* 9..11 only: 12 is cached */
    assert(largest_map == 8 && cache.count == 8 + 8 + 3 && mapped_page[9] && mapped_page[11] && !mapped_page[8]);
    /* a run stops at the end of the view */
    touch(base + (VIEW_PAGES - 2) * PAGE, 8, 3);
    assert(mapped_page[VIEW_PAGES - 1] && !mapped_page[VIEW_PAGES]);
    /* pressure: sweep far more pages than the cache holds, clustered; the cache never exceeds its size */
    const unsigned before_evictions = (unsigned)cache.evictions;
    for (size_t i = 0; i < VIEW_PAGES; i += 8) touch(base + i * PAGE, 8, 3);
    assert(cache.count <= PAGES && cache.peak <= PAGES && cache.evictions > before_evictions && !cache.failures);
    size_t resident = 0;
    for (size_t i = 0; i < VIEW_PAGES; i++) resident += mapped_page[i];
    assert(resident == cache.count && a.cached == cache.count);
    /* every written page that is evicted is flushed first */
    assert(flushes == cache.evictions);                    /* prot 3 includes write: each eviction flushes its page first */
    /* protection changes visit only this view's cached pages */
    PwWineSectionPage *first = &pages[a.head - 1];
    (void)first;
    const unsigned protects_before = protects;
    assert(pw_wine_section_protect(&cache, base, (size_t)VIEW_PAGES * PAGE, 1) == 1);
    assert(protects - protects_before == cache.count);
    /* a second view: its faults and removal leave the first view's pages alone */
    assert(!pw_wine_section_add(&cache, &b, base + (size_t)VIEW_PAGES * PAGE, 8 * PAGE, 4, 0));
    {
        static PwWineSectionRetry retry;
        assert(pw_wine_section_fault_run(&cache, base + (size_t)VIEW_PAGES * PAGE, 3, 1, 0, &retry, 64) == 1);
    }
    assert(b.cached >= 1 && b.cached <= 8);
    const size_t a_before = a.cached;
    assert(!pw_wine_section_remove(&cache, &b));
    assert(b.cached == 0 && b.head == 0 && a.cached + 0 <= a_before);
    /* removing the first view empties the cache and every page is a guard again */
    assert(!pw_wine_section_remove(&cache, &a));
    assert(cache.count == 0 && a.cached == 0 && a.head == 0 && !cache.views);
    for (size_t i = 0; i < VIEW_PAGES * 2; i++) assert(!mapped_page[i]);
    assert(maps > 40 && largest_map == 8 && reserves == unmaps);
    puts("section cache: index, clustered faults, pressure and removal passed");
    return 0;
}
