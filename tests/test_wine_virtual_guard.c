/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Host test of patch 1128's side in virtual.c, extracted from the applied tree by tests/extract_virtual_guard.py
 * (tools/host-tests.sh): a release inside the direct-memory engine stays reserved and is never unmapped; a mapping
 * that is not Wine's, recorded by the engine, becomes a system view unless a live view holds it; a refused commit
 * leaves every page byte as it was, those committed before included, and what an earlier piece changed follows them;
 * a commit into a free placeholder over such a mapping fails and the placeholder stays free; a reservation inside a
 * reserved area asks the engine first. Wine's page tables, views and the engine are a fake 4 MiB arena with 4 KiB pages
 * on 16 KiB host pages; the engine refuses any nonzero protection over the foreign range (errno EEXIST). */
#define __PROSPERO__ 1
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
typedef unsigned char BYTE; typedef int BOOL; typedef uintptr_t UINT_PTR, ULONG_PTR; typedef size_t SIZE_T;
typedef unsigned int NTSTATUS;
#define TRUE 1
#define FALSE 0
#define STATUS_SUCCESS 0
#define STATUS_INVALID_PARAMETER 0xc000000d
#define STATUS_NO_MEMORY 0xc0000017
#define STATUS_CONFLICTING_ADDRESSES 0xc0000018
#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define MEM_REPLACE_PLACEHOLDER 0x4000
#define VPROT_READ 0x01
#define VPROT_WRITE 0x02
#define VPROT_COMMITTED 0x20
#define VPROT_WRITEWATCH 0x40
#define VPROT_SYSTEM 0x0200
#define VPROT_PLACEHOLDER 0x0400
#define VPROT_FREE_PLACEHOLDER 0x0800
#define ROUND_ADDR(addr,mask) ((void *)((UINT_PTR)(addr) & ~(UINT_PTR)(mask)))
#define ROUND_SIZE(addr,size,mask) (((SIZE_T)(size) + ((UINT_PTR)(addr) & (mask)) + (mask)) & ~(UINT_PTR)(mask))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define TRACE(...) ((void)0)
#define ERR(...) fprintf(stderr, __VA_ARGS__)
struct list { struct list *next, *prev; };
#define LIST_ENTRY(elem, type, field) ((type *)((char *)(elem) - offsetof(type, field)))
#define LIST_FOR_EACH_ENTRY(elem, list, type, field) \
    for ((elem) = LIST_ENTRY((list)->next, type, field); &(elem)->field != (list); \
         (elem) = LIST_ENTRY((elem)->field.next, type, field))
struct reserved_area { struct list entry; void *base; size_t size; };
struct file_view { void *base; size_t size; unsigned int protect; };

enum { PAGE = 4096, HOST = 16384, PAGES = 1024 };
static char arena[PAGES * PAGE] __attribute__((aligned(1 << 21)));
static BYTE vprot[PAGES];
static int kernel[PAGES / 4];   /* the protection the engine holds, per host page */
static const UINT_PTR host_page_mask = HOST - 1;
static int use_kernel_writewatch, enable_write_exceptions;
static struct list reserved_areas = { &reserved_areas, &reserved_areas };

static size_t index_of(const void *addr) { return (size_t)((const char *)addr - arena) / PAGE; }
static int is_vprot_exec_write(BYTE v) { (void)v; return 0; }
static SIZE_T get_vprot_range_size(char *base, SIZE_T size, BYTE mask, BYTE *first)
{
    const size_t start = index_of(base), count = size / PAGE;
    size_t i = 1;
    *first = vprot[start];
    while (i < count && !((vprot[start + i] ^ *first) & mask)) i++;
    return i * PAGE;
}
static void set_page_vprot(const void *addr, size_t size, BYTE v) { memset(&vprot[index_of(addr)], v, size / PAGE); }
static void set_page_vprot_bits(const void *addr, size_t size, BYTE set, BYTE clear)
{
    for (size_t i = index_of(addr); i < index_of(addr) + size / PAGE; i++) vprot[i] = (BYTE)((vprot[i] & ~clear) | set);
}

/* The engine: a nonzero protection over [foreign_low, foreign_high) is refused before any change (EEXIST). Wine's
 * mprotect_range reaches it host page by host page here, as __wine_ps5_mprotect goes region piece by piece. */
static char *foreign_low, *foreign_high;
static unsigned engine_calls, checks;
static int engine_protect(char *at, int prot)
{
    engine_calls++;
    if (prot && at < foreign_high && at + HOST > foreign_low) {
        errno = EEXIST;
        return -1;
    }
    kernel[index_of(at) / 4] = prot;
    return 0;
}
static int mprotect_range(void *base, size_t size, BYTE set, BYTE clear)
{
    char *at = ROUND_ADDR(base, host_page_mask), *end = (char *)base + size;
    (void)set; (void)clear;
    for (; at < end; at += HOST) {
        int prot = 0;
        for (int i = 0; i < 4; i++) {
            const BYTE v = vprot[index_of(at) + i];
            if (v & VPROT_COMMITTED) prot |= v & (VPROT_READ | VPROT_WRITE);
        }
        if (engine_protect(at, prot)) return -1;
    }
    return 0;
}
int __wine_ps5_dmem_check(void *base, size_t size)
{
    checks++;
    if ((char *)base < foreign_high && (char *)base + size > foreign_low) {
        errno = EEXIST;
        return -1;
    }
    return 0;
}
static struct { void *start; size_t size; int prot, flags; } anon[8];
static unsigned anon_count, munmaps, added;
static void *anon_mmap_fixed(void *start, size_t size, int prot, int flags)
{
    if (anon_count < 8) anon[anon_count].start = start, anon[anon_count].size = size, anon[anon_count].prot = prot,
                        anon[anon_count].flags = flags;
    anon_count++;
    if ((char *)start >= arena && (char *)start < arena + sizeof(arena))
        for (size_t h = index_of(start) / 4; h < (index_of(start) + size / PAGE) / 4; h++) kernel[h] = prot;
    return start;
}
static void kernel_writewatch_register_range(struct file_view *view, void *base, size_t size)
{
    (void)view; (void)base; (void)size;
}
static void reset_write_watches(void *base, size_t size) { (void)base; (void)size; }
static int fake_munmap(void *start, size_t size) { (void)start; (void)size; munmaps++; return 0; }
#define munmap fake_munmap
static size_t unmap_area_above_user_limit(void *addr, size_t size) { (void)addr; return size; }
static int __wine_ps5_section_arena_contains(uintptr_t base, size_t size) { (void)base; (void)size; return 0; }
static int keep_result;
static unsigned keeps;
int __wine_ps5_dmem_keep(void *base, size_t size) { (void)base; (void)size; keeps++; return keep_result; }
static void mmap_add_reserved_area(void *addr, SIZE_T size) { (void)addr; (void)size; added++; }

/* Wine's views, and the holes the engine recorded. */
static struct file_view views[8];
static unsigned view_count;
static struct file_view *find_view(const void *addr, size_t size)
{
    (void)size;
    for (unsigned i = 0; i < view_count; i++)
        if ((const char *)addr >= (char *)views[i].base && (const char *)addr < (char *)views[i].base + views[i].size)
            return &views[i];
    return NULL;
}
static struct file_view *find_view_range(const void *addr, size_t size)
{
    for (unsigned i = 0; i < view_count; i++)
        if ((const char *)addr < (char *)views[i].base + views[i].size &&
            (char *)views[i].base < (const char *)addr + size)
            return &views[i];
    return NULL;
}
static NTSTATUS create_view(struct file_view **ret, void *base, size_t size, unsigned int protect)
{
    views[view_count] = (struct file_view){ base, size, protect };
    *ret = &views[view_count++];
    return STATUS_SUCCESS;
}
static struct { void *base; size_t size; } holes[4];
static unsigned hole_count, hole_next;
int __wine_ps5_native_hole(void **base, size_t *size)
{
    if (hole_next == hole_count) return 0;
    *base = holes[hole_next].base;
    *size = holes[hole_next++].size;
    return 1;
}

#include "virtual_guard.inc"

static void reset(void)
{
    memset(vprot, 0, sizeof(vprot));
    memset(kernel, 0, sizeof(kernel));
    foreign_low = foreign_high = NULL;
    engine_calls = checks = anon_count = munmaps = added = keeps = view_count = hole_count = hole_next = 0;
}

int main(void)
{
    struct file_view plain = { arena, sizeof(arena), 0 }, *view;
    char *const a = arena + 16 * PAGE;

    /* A release inside the engine stays reserved: a protection-0 replace and a reserved area, no munmap. The address
     * pool's own release, and a range the engine does not own, are unmapped as before. */
    reset();
    unmap_area(a, 16 * PAGE);
    assert(keeps == 1 && !munmaps && anon_count == 1 && anon[0].start == a && anon[0].size == 16 * PAGE &&
           anon[0].prot == PROT_NONE && added == 1);
    ps5_release_to_system = TRUE;
    unmap_area(a, 16 * PAGE);
    ps5_release_to_system = FALSE;
    assert(keeps == 1 && munmaps == 1);
    keep_result = -1;
    unmap_area(a, 16 * PAGE);
    assert(keeps == 2 && munmaps == 2);
    keep_result = 0;

    /* Holes: one inside a live view stays that view's; one in free space becomes a system view. */
    reset();
    views[view_count++] = (struct file_view){ arena, 64 * PAGE, VPROT_PLACEHOLDER | VPROT_FREE_PLACEHOLDER };
    holes[0].base = arena + 8 * PAGE;
    holes[0].size = 4 * PAGE;
    holes[1].base = arena + 128 * PAGE;
    holes[1].size = 4 * PAGE;
    hole_count = 2;
    ps5_claim_native_holes();
    assert(view_count == 2 && views[1].base == arena + 128 * PAGE && views[1].size == 4 * PAGE &&
           views[1].protect == VPROT_SYSTEM && hole_next == 2);

    /* A commit over committed data and a foreign mapping: refused, and the committed host page keeps COMMITTED, its
     * protection and the kernel's (its piece was changed before the refusal and follows the restored bytes). */
    reset();
    set_page_vprot(a, HOST, VPROT_COMMITTED | VPROT_READ);
    kernel[index_of(a) / 4] = VPROT_READ;
    foreign_low = a + HOST, foreign_high = a + 2 * HOST;
    errno = 0;
    assert(!set_vprot(&plain, a, 2 * HOST, VPROT_COMMITTED | VPROT_READ | VPROT_WRITE) && errno == EEXIST);
    for (int i = 0; i < 4; i++) assert(vprot[index_of(a) + i] == (VPROT_COMMITTED | VPROT_READ));
    for (int i = 4; i < 8; i++) assert(!vprot[index_of(a) + i]);
    assert(kernel[index_of(a) / 4] == VPROT_READ && !kernel[index_of(a) / 4 + 1]);
    /* Without the foreign mapping the same commit goes through. */
    foreign_low = foreign_high = NULL;
    assert(set_vprot(&plain, a, 2 * HOST, VPROT_COMMITTED | VPROT_READ | VPROT_WRITE));
    assert(kernel[index_of(a) / 4 + 1] == (VPROT_READ | VPROT_WRITE));
    /* shortcut: more than 64 runs of page bytes are not kept; only nothing is left committed. */
    reset();
    for (int i = 0; i < 130; i++) vprot[index_of(a) + i] = i % 2 ? 0 : VPROT_COMMITTED | VPROT_READ;
    foreign_low = a + 128 * PAGE, foreign_high = a + 132 * PAGE;
    assert(!set_vprot(&plain, a, 132 * PAGE, VPROT_COMMITTED | VPROT_READ) && errno == EEXIST);
    for (int i = 0; i < 132; i++) assert(!(vprot[index_of(a) + i] & VPROT_COMMITTED));

    /* An emulator's commit into a placeholder (VirtualAlloc2 MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER) over a
     * foreign mapping: refused, never a success over uncommitted pages; the placeholder is free again, its pages
     * reserved, and the retry once the mapping is gone succeeds. */
    reset();
    char *const p = arena + 256 * PAGE;
    views[view_count++] = (struct file_view){ p, 64 * PAGE, VPROT_PLACEHOLDER | VPROT_FREE_PLACEHOLDER };
    foreign_low = p + 32 * PAGE, foreign_high = p + 36 * PAGE;
    view = NULL;
    errno = 0;
    assert(map_view_replace(&view, p, 64 * PAGE, MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER,
                            VPROT_COMMITTED | VPROT_READ | VPROT_WRITE) == STATUS_CONFLICTING_ADDRESSES && !view);
    assert(views[0].protect == (VPROT_PLACEHOLDER | VPROT_FREE_PLACEHOLDER));
    for (int i = 0; i < 64; i++) assert(!vprot[index_of(p) + i] && !kernel[(index_of(p) + i) / 4]);
    assert(anon_count == 1 && anon[0].start == p && anon[0].size == 64 * PAGE && anon[0].prot == PROT_NONE);
    foreign_low = foreign_high = NULL;
    assert(map_view_replace(&view, p, 64 * PAGE, MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER,
                            VPROT_COMMITTED | VPROT_READ | VPROT_WRITE) == STATUS_SUCCESS && view == &views[0]);
    assert(views[0].protect == (VPROT_COMMITTED | VPROT_READ | VPROT_WRITE | VPROT_PLACEHOLDER));
    assert(kernel[index_of(p + 32 * PAGE) / 4] == (VPROT_READ | VPROT_WRITE));

    /* A reservation inside a reserved area asks the engine first and is refused over a foreign mapping; a commit leaves
     * the question to the engine's own replace (one query per call). */
    reset();
    foreign_low = a + HOST, foreign_high = a + 2 * HOST;
    errno = 0;
    assert(reserved_area_mmap(a, 4 * HOST, PROT_NONE) == MAP_FAILED && errno == EEXIST && checks == 1 && !anon_count);
    assert(reserved_area_mmap(a + 2 * HOST, 2 * HOST, PROT_NONE) == a + 2 * HOST && checks == 2 && anon_count == 1);
    assert(reserved_area_mmap(a, 4 * HOST, PROT_READ | PROT_WRITE) == a && checks == 2 && anon_count == 2);

    puts("virtual.c guard (patch 1128): releases kept reserved, holes made system views, a refused commit undone "
         "page by page, a refused placeholder commit failed and the placeholder free again, reservations in a reserved "
         "area asked first, passed");
    return 0;
}
