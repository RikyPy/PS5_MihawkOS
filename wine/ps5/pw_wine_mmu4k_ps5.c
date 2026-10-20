/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The console side of the 4 KiB MMU (pw_wine_mmu4k.h, docs/MMU4K.md sections 4.2, 7-10), linked into ntdll: the slot
 * memory (one 64 KiB JIT object, the only memory a payload's kernel lets execute, exec-kernel-2026-10-14), the
 * console's alias ops (a payload's direct-memory run or a direct section's mapped chunk, mapped again CPU read-write
 * at a run of the helper's shared-page range), the WINE_PS5_MMU4K switch, and the census export. Called by
 * virtual_init (patch 1134) under nothing; every refusal leaves the engine off, so every store beside a read-only
 * 4 KiB page keeps 1122's step. */
#include "pw_wine_alias.h"
#include "pw_wine_mmu4k.h"
#include "pw_wine_pages.h"
#include "pw_wine_perf.h"
#include "pw_wine_section_ps5.h"
#include "../../include/prospero_win.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

extern int sceKernelJitCreateSharedMemory(const char *name, size_t bytes, int max_protection, int *fd);
extern int sceKernelMprotect(const void *address, size_t bytes, int protection);
extern int sceKernelMunmap(void *address, size_t bytes);
extern int sceKernelMapDirectMemory(void **address, size_t bytes, int protection, int flags, int64_t offset,
                                    size_t alignment);
int __wine_ps5_dmem_backing(uintptr_t address, int64_t *offset, uintptr_t *low, uintptr_t *high);

enum { KERNEL_FIXED = 0x10, CPU_READ_WRITE = 0x03, ALIGNMENT = 0x4000 };

/* The direct memory behind a host page: a payload's direct run first, then a direct section's chunk in its view. Each
 * op tags what the virtual mutex's holder does (pw_wine_perf.h), so a stall line tells the engine's spin lock and a
 * section's fstat (backing) from a kernel call over direct memory (give, map). */
/* The program range the last backing answer covered, for alias_map's line (both run under the alias module's lock). */
static uintptr_t backing_low, backing_high;
static int alias_backing(void *context, uintptr_t address, int64_t *dmem, uintptr_t *low, uintptr_t *high)
{
    const char *was = pw_wine_perf_vlock_doing("alias-backing");
    (void)context;
    const int found = __wine_ps5_dmem_backing(address, dmem, low, high) ||
                      __wine_ps5_section_backing(address, dmem, low, high);
    if (found) { backing_low = *low; backing_high = *high; }
    pw_wine_perf_vlock_doing(was);
    return found;
}
static uintptr_t alias_take(void *context, size_t bytes)
{
    const char *was = pw_wine_perf_vlock_doing("alias-take");
    (void)context;
    const uintptr_t address = pw_wine_pages_take(bytes);
    pw_wine_perf_vlock_doing(was);
    return address;
}
/* WINE_PS5_ALIAS_GIVE=unmap: the window's direct memory is unmapped before the reservation is laid back, as a release of
 * Wine's own direct memory does, instead of the reservation replacing the live mapping. An A/B switch: with eviction off
 * A Unity game ran 469 s with no stall (2026-10-16; record removed from the tree; in git history); with it on, each of three runs stalled 31-37 s in, a
 * thread 15-30 s inside a kernel mprotect of direct memory, shortly after windows began to be given back. */
static int give_unmaps;
static void alias_give(void *context, uintptr_t address, size_t bytes)
{
    const char *was = pw_wine_perf_vlock_doing("alias-give");
    (void)context;
    if (give_unmaps) (void)sceKernelMunmap((void *)address, bytes);
    pw_wine_pages_give(address, bytes);
    fprintf(stderr, "wine-ps5: alias give at=%#lx bytes=%#zx\n", (unsigned long)address, bytes);
    pw_wine_perf_vlock_doing(was);
}
/* CPU read-write, fixed over the reservation: the protection and memory type a payload maps its own runs with. */
static int alias_map(void *context, uintptr_t address, size_t bytes, int64_t dmem)
{
    const char *was = pw_wine_perf_vlock_doing("alias-map");
    void *at = (void *)address;
    (void)context;
    const int failed = sceKernelMapDirectMemory(&at, bytes, CPU_READ_WRITE, KERNEL_FIXED, dmem, ALIGNMENT) ||
                       at != (void *)address;
    /* Each window's memory on record, a few a second: a stall's direct-memory range is matched against them. */
    fprintf(stderr, "wine-ps5: alias map at=%#lx bytes=%#zx dmem=%#llx program=%#lx-%#lx%s\n", (unsigned long)address,
            bytes, (unsigned long long)dmem, (unsigned long)backing_low, (unsigned long)backing_high,
            failed ? " refused" : "");
    pw_wine_perf_vlock_doing(was);
    return failed;
}
const PwWineAliasOps __wine_ps5_alias_ops = { NULL, alias_backing, alias_take, alias_give, alias_map };

static uint64_t xcr0(void)
{
    uint32_t low, high;
    __asm__ volatile("xgetbv" : "=a"(low), "=d"(high) : "c"(0));
    return (uint64_t)high << 32 | low;
}

static int off(const char *why, int error)
{
    fprintf(stderr, "wine-ps5: mmu4k off: %s (%d); stores beside read-only 4 KiB pages step (patch 1122)\n", why, error);
    return -1;
}

/* 0 with the engine on, -1 off; one log line either way. */
int __wine_ps5_mmu4k_init(const PwWineMmu4kOps *ops)
{
    const char *setting = getenv("WINE_PS5_MMU4K");
    void *slots;
    int fd = -1, rc;

    if (setting && setting[0] == '0') return off("WINE_PS5_MMU4K=0", 0);
    if (!pw_wine_pages_base(NULL)) return off("no shared-page range", 0);
    if ((rc = sceKernelJitCreateSharedMemory("pw-mmu4k", PW_WINE_MMU4K_SLOT_BYTES, PROT_READ | PROT_WRITE | PROT_EXEC,
                                             &fd)))
        return off("JIT object refused (a title has none)", rc);
    slots = pw_wine_pages_map(PW_WINE_MMU4K_SLOT_BYTES, PROT_READ | PROT_WRITE, fd);
    close(fd);   /* the mapping keeps the object (K6) */
    if (slots == MAP_FAILED) return off("slot mapping refused", 0);
    if ((rc = sceKernelMprotect(slots, PW_WINE_MMU4K_SLOT_BYTES, PROT_READ | PROT_WRITE | PROT_EXEC))) {
        pw_wine_pages_unmap(slots, PW_WINE_MMU4K_SLOT_BYTES);
        return off("slots not made executable", rc);
    }
    if ((rc = pw_wine_alias_init(&__wine_ps5_alias_ops)) != PW_OK && rc != PW_ERR_STATE) return off("aliases", rc);
    const char *evict = getenv("WINE_PS5_ALIAS_EVICT");
    if (evict && evict[0] == '0') pw_wine_alias_set_eviction(0);
    const char *limit = getenv("WINE_PS5_ALIAS_WINDOWS");
    if (limit && *limit) pw_wine_alias_set_windows((unsigned)strtoul(limit, NULL, 10));
    const char *give = getenv("WINE_PS5_ALIAS_GIVE");
    give_unmaps = give && !strcmp(give, "unmap");
    if ((rc = pw_wine_mmu4k_init(ops, slots, PW_WINE_MMU4K_SLOT_BYTES, xcr0() & 7)) != PW_OK) return off("engine", rc);
    fprintf(stderr, "wine-ps5: mmu4k on: slots %p, %u run stacks, alias eviction %s, give %s\n", slots,
            (unsigned)PW_WINE_MMU4K_STACKS, evict && evict[0] == '0' ? "off" : "on",
            give_unmaps ? "unmaps first" : "lays the reservation over the mapping");
    return 0;
}

/* The census (docs/MMU4K.md section 10), in this order: runs, insns, the nine exits (pw_wine_mmu4k.h's order, yield
 * last), stepfall, stack_full, stack_damage, top[4] (opcode << 32 | count), alias maps, map failures, forgets,
 * evictions, noalias. Fills at most count and returns how many exist (23). */
unsigned int __wine_ps5_mmu4k_stats(uint64_t *out, unsigned int count)
{
    PwWineMmu4kStats engine;
    PwWineAliasStats alias;
    pw_wine_mmu4k_stats(&engine);
    pw_wine_alias_stats(&alias);
    uint64_t values[2 + PW_WINE_MMU4K_EXITS + 3 + 4 + 5];
    unsigned n = 0;
    values[n++] = engine.runs;
    values[n++] = engine.insns;
    for (unsigned i = 0; i < PW_WINE_MMU4K_EXITS; i++) values[n++] = engine.exits[i];
    values[n++] = engine.stepfall;
    values[n++] = engine.stack_full;
    values[n++] = engine.stack_damage;
    for (unsigned i = 0; i < 4; i++) values[n++] = (uint64_t)engine.top[i] << 32 | (uint32_t)engine.top_count[i];
    values[n++] = alias.maps;
    values[n++] = alias.map_failures;
    values[n++] = alias.forgets;
    values[n++] = alias.evictions;
    values[n++] = alias.noalias;
    for (unsigned i = 0; out && i < count && i < n; i++) out[i] = values[i];
    return n;
}
