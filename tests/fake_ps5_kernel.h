/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The console's kernel memory map as measured, for host tests that run the real memory code of a helper (the
 * direct-memory engine and its console glue, the shared pages, the heap, the GPU client) and replay past console
 * crashes against it.
 *
 * Console addresses are host addresses: the fake owns the window [FK_LOW, FK_HIGH) of the host's address space, where
 * nothing else maps, and backs every entry it models with real host memory (anonymous memory, one sparse file for the
 * direct memory, so two mappings of one offset alias, and memfds), so the code under test reads, writes and runs what
 * it maps. A test links tests/fake_ps5_kernel.c with -Wl,--wrap= for mmap munmap mprotect mlock munlock mincore madvise
 * msync open fork pread pwrite, and without AddressSanitizer: console addresses below 0x2008fff7000 lie in its shadow
 * gap. The fake defines the sceKernel* functions itself.
 *
 * Measured rules modelled (docs/PLAN.md, "The PS5 memory backend"; tools/dmem-share-probe.c exec/exec4, K1-K10):
 * - the page is 16 KiB: an address or length that is not a multiple is EINVAL (0x80020016);
 * - a reservation is an entry with protection 0. sceKernelReserveVirtualRange 0x10 replaces anything there, 0x80 with
 *   it refuses where anything is, flags 0 goes to the hint when it is free and else to the first fit above it, no
 *   address to the first fit above the placement floor (the first free range above 8 GiB, 0x200000000); so does a
 *   mapping that names no fixed address, into any hole, a hole inside a range another component reserved included,
 *   and with fk_place_in_reservations into a reservation too;
 * - mmap: MAP_FIXED replaces, MAP_EXCL is ignored, execute asked at map time is dropped except on JIT objects (K8);
 * - mprotect succeeds on any entry and keeps execute only on JIT objects (K1, K2); on a reservation it backs nothing;
 * - sceKernelMapDirectMemory: execute refused (0x80020016, K8), an offset neither allocated nor granted refused (EACCES
 *   0x8002000d, dmem-share-2026-10-12-05), GPU bits (0x30) make a wired GPU entry; releasing direct memory unmaps it;
 * - a JIT object maps MAP_FIXED only at exactly its size (K4), pread and pwrite on it fail with ESPIPE (K5), and its
 *   mapping outlives the descriptor (K6);
 * - sceKernelVirtualQuery (NEXT, EACCES 0x8002000d past the last entry) and sceKernelQueryMemoryProtection read the
 *   table; physical shared memory is invisible to VirtualQuery (it reads as the reservation under it), mincore sees it;
 * - private anonymous memory is charged to flexible memory, PROT_NONE included (2026-10-06, record removed from the tree: a 32 GiB
 *   PROT_NONE mapping was refused, ENOMEM), against an optional budget.
 * Not measured, modelled as stated: an anonymous PROT_NONE mapping reads as a bare reservation to VirtualQuery; mprotect
 * keeps an entry's GPU bits.
 *
 * Hazards are recorded, never aborted on; a test asserts fk_hazards() == 0, and the same test run on the code that
 * crashed (a scratch copy with the guard taken out) shows the fake catches the crash:
 * - FK_OWNER: an unmap, a fixed mapping or reservation, or a protection change over an entry another maker made (fk_as
 *   names who calls; a reservation laid over a mapping, 0x10 without 0x80, belongs to no one: it gives space back);
 * - FK_UNWIRE: a teardown or replace of an mlocked entry, or of a GPU entry by another maker ("unwire timeout");
 * - FK_BIGBARE: one unmap of 1 GiB or more of bare reservation after fk_gpu_started (both hangs were such unmaps; the
 *   size is not measured, it is conservative);
 * - FK_SDKGRANT: the payload SDK's unlocked grant of execute was reached (kernel_mprotect, kernel_set_vmem_protection);
 * - FK_DMEMDEV: /dev/dmem opened or fork called (the 2026-10-12 console crash);
 * - FK_TITLEMEM: a physical shared object "title" made, mapped by another maker (the title's memory held by a helper,
 *   2026-10-06, record removed from the tree);
 * - FK_MAPPED: direct memory released while a mapping of it remains (the release unmaps it, measured: a hole opens in
 *   the range of whoever mapped it, and a second mapping kept by address, an alias of docs/MMU4K.md, would reach
 *   whatever lands there; invariant I4). */
#ifndef FAKE_PS5_KERNEL_H
#define FAKE_PS5_KERNEL_H
#include <stddef.h>
#include <stdint.h>

#define FK_LOW ((uintptr_t)0x100000000)
#define FK_HIGH ((uintptr_t)0x20000000000)
enum { FK_PAGE = 16384 };
enum { FK_RESERVED = 1, FK_ANON, FK_DIRECT, FK_FILE, FK_PHYSHM, FK_JIT, FK_PRX };
enum { FK_OWNER = 1, FK_UNWIRE = 2, FK_BIGBARE = 4, FK_SDKGRANT = 8, FK_DMEMDEV = 16, FK_TITLEMEM = 32,
       FK_MAPPED = 64 };
/* What a hook (fk_on_next) runs before, and what the call log (fk_calls, fk_touched) counts. */
enum { FK_OP_RESERVE, FK_OP_RESERVE_FIXED, FK_OP_RESERVE_NOOVERWRITE, FK_OP_MMAP, FK_OP_MAP_DIRECT, FK_OP_MUNMAP,
       FK_OP_MPROTECT, FK_OP_MLOCK, FK_OP_RELEASE_DIRECT, FK_OP_COUNT };

typedef struct FkEntry {
    uintptr_t start, end;
    int kind;
    unsigned protection;   /* CPU 0x7, GPU 0x30 */
    unsigned wired;        /* mlock count */
    int64_t offset;        /* direct memory's, or the object's */
    char maker[16];
} FkEntry;

/* An empty map, the window bare, no hazards; callers are "wine" until fk_as. Once per process (a test case). */
void fk_init(void);
/* Who makes the calls that follow (a component: "wine", "gpu-client", "loader", "helper", "title"). */
void fk_as(const char *maker);
/* An entry of another component anywhere, over whatever is there, without a hazard: protection with GPU bits makes a
 * wired GPU entry; wired, an mlocked one. Backed like the kernel's own. */
void fk_plant(int kind, uintptr_t at, size_t bytes, unsigned protection, const char *maker, int wired);
/* A mapping made with no fixed address, by maker: the first fit at or above the hint (and the floor). Its address, 0. */
uintptr_t fk_place(int kind, uintptr_t hint, size_t bytes, unsigned protection, const char *maker);
/* From now on an unmap of a bare reservation of 1 GiB or more is FK_BIGBARE. */
void fk_gpu_started(void);
void fk_set_floor(uintptr_t floor);
/* A reservation laid over a mapping (0x10 without 0x80) answers success and leaves the mapping (check_laid's case). */
void fk_lay_ignored(int on);
/* A physical shared mapping answers success with nothing mapped (the window page that vanished). */
void fk_phantom_physhm(int on);
/* A mapping with no fixed address may be placed inside a reservation, as the console is believed to have placed the GPU
 * client's wired memory inside Wine's (not measured: the kernel may only have used a hole). */
void fk_place_in_reservations(int on);
/* The next CPU-only sceKernelMapDirectMemory is refused. */
void fk_refuse_cpu_direct_once(void);
/* Private anonymous memory the process may have charged at once; 0, none. */
void fk_flexible_budget(size_t bytes);
size_t fk_charged(void);
/* Direct memory: taken (the engine's and the ForApp allocator), and granted by another process (the GPU owner's). */
int64_t fk_dmem_allocate(size_t bytes);
void fk_grant(int64_t offset, size_t bytes);
/* fn runs, as "intruder", just before the next call of op (once); fk_fail_next makes the next call of op fail. */
void fk_on_next(int op, void (*fn)(void));
void fk_fail_next(int op);
/* A PRX module placed by the kernel: text (protection 0x4), read-only data (0x1) and data (0x3), info filled as
 * sceKernelGetModuleInfo's record (wine/ps5/pw_wine_prx.h). Its text address; unloaded by the kernel with
 * fk_unload_module. */
uintptr_t fk_load_module(size_t text, size_t rodata, size_t data, unsigned char info[0x160]);
void fk_unload_module(uintptr_t text);

unsigned fk_hazards(void);
const char *fk_report(void);
void fk_clear_hazards(void);
/* The entry holding at: 1, or 0. fk_next: the entry holding at or the next one. */
int fk_entry(uintptr_t at, FkEntry *entry);
int fk_next(uintptr_t at, FkEntry *entry);
/* Bytes of [low, high) no entry holds: where a mapping with no address can land. */
size_t fk_free_bytes(uintptr_t low, uintptr_t high);
/* Calls of op, all, or those that covered part of [low, high); the largest such call's bytes. */
unsigned fk_calls(int op);
unsigned fk_touched(int op, uintptr_t low, uintptr_t high);
size_t fk_largest(int op);
void fk_clear_calls(void);
/* What was written to klog (sceKernelDebugOutText). */
const char *fk_klog(void);

#endif
