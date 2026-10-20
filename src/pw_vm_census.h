/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_VM_CENSUS_H
#define PW_VM_CENSUS_H
/* Where a helper's system memory is: its mappings grouped by name and kind (B3, docs/PLAN.md).
 *
 * user-desktop-2026-10-10-02: a launcher's GPU process held 46, then 376, then 46, then 325 MiB of resident system memory
 * (sceKernelGetResidentFmemCount) while Wine's direct memory (523 MiB), Wine's Unix heap (71 MiB) and its mapped DLL
 * images (302 MiB, the same in every launcher helper that stayed near 50 MiB) did not move. No counter named the memory,
 * so the next step is the one that names it: a walk of the process's mappings (sceKernelVirtualQuery with NEXT),
 * grouped by the mapping's name and flags, largest first.
 *
 * The record, 72 bytes (libkernel; the public reimplementations agree, and pw_wine_vm.c reads its first two fields):
 *   void *start, *end; u64 offset; s32 protection; s32 memory type; u8 flags (bit 0 flexible, 1 direct, 2 stack,
 *   3 pooled, 4 committed); char name[32]; padding
 * The helper logs the first record in hex, so a layout the firmware does not share shows in the first run. */
#include <stddef.h>
#include <stdint.h>

enum {
    PW_VM_RECORD = 72,
    PW_VM_FLEXIBLE = 1u, PW_VM_DIRECT = 2u, PW_VM_STACK = 4u, PW_VM_POOLED = 8u, PW_VM_COMMITTED = 16u,
    PW_VM_NAME = 32, PW_VM_GROUPS = 48, PW_VM_BIG = 8,
    PW_VM_WALK_LIMIT = 65536,     /* mappings walked at most; a walk that reaches it says so */
};

/* One record's fields. */
typedef struct PwVmRecord {
    uintptr_t start, end;
    int32_t protection, type;
    uint8_t flags;
    char name[PW_VM_NAME + 1];
} PwVmRecord;

/* Mappings of one name and flags. */
typedef struct PwVmGroup {
    char name[PW_VM_NAME + 1];
    uint8_t flags;
    int32_t protection;          /* the first mapping's */
    uint32_t mappings;
    uint64_t bytes;
    uint64_t resident;           /* resident bytes, when the walk had a resident callback */
} PwVmGroup;

/* One of the largest mappings, whole: the kernel's own map (tools/console-memory.py map=PID) lists the same ranges by
 * address, so the two are matched there (2026-10-11, record removed from the tree: 8 regions, 228 MiB, fully resident in the
 * kernel's count and absent from this walk's residency). */
typedef struct PwVmBig {
    uintptr_t start;
    uint64_t bytes;
    int32_t protection;
    uint8_t flags;
    char name[PW_VM_NAME + 1];
} PwVmBig;

typedef struct PwVmCensus {
    PwVmGroup groups[PW_VM_GROUPS];
    PwVmBig big[PW_VM_BIG];      /* the largest accessible mappings, largest first */
    unsigned big_count;
    unsigned count;              /* groups kept */
    unsigned mappings;           /* mappings walked */
    uint64_t flexible, direct, other; /* bytes, by kind (flexible wins when both bits are set) */
    uint64_t resident;           /* resident bytes of the accessible mappings that are not direct memory */
    uint64_t reserved_resident;  /* resident bytes of the inaccessible mappings (protection 0), asked from 2026-10-11 */
    uint64_t direct_resident;    /* resident bytes of the accessible direct mappings (asked from 2026-10-11) */
    uint64_t unkept;             /* bytes in groups that did not fit */
    int truncated;               /* the walk stopped at PW_VM_WALK_LIMIT */
    int failed;                  /* the walk stopped at a query that failed */
} PwVmCensus;

/* The next mapping at or after address into raw (PW_VM_RECORD bytes): 0, 1 when there is none, < 0 on failure. */
typedef int (*PwVmQuery)(void *context, uintptr_t address, uint8_t raw[PW_VM_RECORD]);
/* Resident bytes in [start, end) (mincore on the console); called for each mapping,
 * direct memory and reservations too, whose pages are counted apart. NULL: not counted. */
typedef uint64_t (*PwVmResident)(void *context, uintptr_t start, uintptr_t end);

#ifdef __cplusplus
extern "C" {
#endif
/* A record's fields from its bytes (the name always terminated). */
void pw_vm_record_decode(const uint8_t raw[PW_VM_RECORD], PwVmRecord *out);
/* Walks every mapping through query and groups them; groups sorted by resident bytes, then mapped bytes, largest
 * first. */
void pw_vm_census_walk(PwVmCensus *census, PwVmQuery query, PwVmResident resident, void *context);
/* "start+MiB:flags/protection:name ..." for the largest mappings (what PwVmCensus.big holds), into out. */
size_t pw_vm_census_format_big(const PwVmCensus *census, char *out, size_t size);
/* "name:flags/protection=resident/mapped MiB/mappings ..." for the n first groups with flags & mask (0: every group),
 * into out. */
size_t pw_vm_census_format(const PwVmCensus *census, unsigned n, uint8_t mask, char *out, size_t size);
#ifdef __cplusplus
}
#endif
#endif
