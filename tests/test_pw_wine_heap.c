/* SPDX-License-Identifier: GPL-3.0-or-later
 * The heap's backing hook: spans and large blocks come from a backing when one
 * is set, return to it when a large block is freed, and fail when it refuses:
 * the heap never maps by itself once a backing is set. */
#include "../wine/ps5/pw_wine_heap.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

static unsigned maps, unmaps, refuse;
static size_t mapped_now;

static void *fake_map(size_t bytes)
{
    void *p;
    assert(bytes % 16384 == 0);
    if (refuse) return NULL;
    p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(p != MAP_FAILED);
    maps++;
    mapped_now += bytes;
    return p;
}
static void fake_unmap(void *address, size_t bytes)
{
    assert(bytes % 16384 == 0 && mapped_now >= bytes);
    mapped_now -= bytes;
    unmaps++;
    assert(!munmap(address, bytes));
}

int main(void)
{
    PwWineHeapStats before, after;
    void *early_large, *small, *large, *large2, *fallback;

    /* Before a backing exists the heap maps by itself. */
    early_large = pw_wine_heap_malloc(200000);
    assert(early_large);
    memset(early_large, 1, 200000);

    pw_wine_heap_set_backing(fake_map, fake_unmap);

    /* A small block opens a span through the backing. */
    small = pw_wine_heap_malloc(100);
    assert(small && maps >= 1);
    memset(small, 2, 100);

    /* A large block comes from it, and goes back to it. */
    maps = unmaps = 0;
    large = pw_wine_heap_malloc(300000);
    assert(large && maps == 1);
    memset(large, 3, 300000);
    assert(pw_wine_heap_usable_size(large) >= 300000);
    pw_wine_heap_free(large);
    assert(unmaps == 1 && mapped_now % (1u << 20) == 0); /* only whole spans remain */

    /* A block taken before the backing is released without it. */
    unmaps = 0;
    pw_wine_heap_free(early_large);
    assert(unmaps == 0);

    /* realloc across the large threshold moves between mappings. */
    large = pw_wine_heap_malloc(100000);
    memset(large, 4, 100000);
    large2 = pw_wine_heap_realloc(large, 400000);
    assert(large2 && ((unsigned char *)large2)[99999] == 4);
    pw_wine_heap_free(large2);

    /* A refusing backing fails the allocation: no mapping of the heap's own, which the kernel would place. */
    refuse = 1;
    maps = unmaps = 0;
    fallback = pw_wine_heap_malloc(250000);
    assert(!fallback && maps == 0);
    refuse = 0;
    fallback = pw_wine_heap_malloc(250000);
    assert(fallback && maps == 1);
    memset(fallback, 5, 250000);
    pw_wine_heap_free(fallback);
    assert(unmaps == 1);

    /* Statistics stay consistent: everything large is back, only spans remain. */
    pw_wine_heap_stats(&before);
    large = pw_wine_heap_malloc(500000);
    pw_wine_heap_stats(&after);
    assert(after.mapped_bytes > before.mapped_bytes);
    pw_wine_heap_free(large);
    pw_wine_heap_stats(&after);
    assert(after.mapped_bytes == before.mapped_bytes && after.large_live == before.large_live);

    pw_wine_heap_free(small);
    puts("pw_wine_heap: backing ok");
    return 0;
}
