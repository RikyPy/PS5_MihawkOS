/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Where a helper's system memory is (src/pw_vm_census.h): the record layout, the walk, grouping by name and flags,
 * residency, ordering and the log line, over a fake address space. */
#include "../src/pw_vm_census.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct Fake {
    uint64_t start, end;
    int32_t protection;
    uint8_t flags;
    const char *name;
    uint64_t resident;
} Fake;

typedef struct Space {
    const Fake *maps;
    unsigned count;
    int fail_at;          /* query index that fails, -1 none */
    unsigned queries;
} Space;

enum { MIB = 1 << 20 };

static void encode(const Fake *f, uint8_t raw[PW_VM_RECORD])
{
    memset(raw, 0, PW_VM_RECORD);
    memcpy(raw, &f->start, 8);
    memcpy(raw + 8, &f->end, 8);
    memcpy(raw + 24, &f->protection, 4);
    raw[32] = f->flags;
    memcpy(raw + 33, f->name, strlen(f->name) < PW_VM_NAME ? strlen(f->name) : PW_VM_NAME);
}

static int query(void *context, uintptr_t address, uint8_t raw[PW_VM_RECORD])
{
    Space *space = context;
    if ((int)space->queries++ == space->fail_at) return -1;
    for (unsigned i = 0; i < space->count; i++)
        if (space->maps[i].end > address) { encode(&space->maps[i], raw); return 0; }
    return 1;
}

static uint64_t resident(void *context, uintptr_t start, uintptr_t end)
{
    Space *space = context;
    for (unsigned i = 0; i < space->count; i++)
        if (space->maps[i].start == start && space->maps[i].end == end) return space->maps[i].resident;
    return 0;
}

int main(void)
{
    /* A GPU process: Chromium's heap pages, two libc heap arenas, Wine's direct memory, a reservation, a stack. */
    const Fake maps[] = {
        {0x100000, 0x100000 + 64ull * MIB, 3, PW_VM_FLEXIBLE, "libc heap", 40ull * MIB},
        {0x200000000, 0x200000000 + 512ull * MIB, 3, PW_VM_FLEXIBLE | PW_VM_COMMITTED, "anon", 380ull * MIB},
        {0x300000000, 0x300000000 + 64ull * MIB, 3, PW_VM_FLEXIBLE, "libc heap", 20ull * MIB},
        {0x400000000, 0x400000000 + 600ull * MIB, 3, PW_VM_DIRECT, "wine", 600ull * MIB},
        {0x800000000, 0x800000000 + 10000ull * MIB, 0, 0, "reserved", 0},
        {0x2000000000, 0x2000000000 + 1ull * MIB, 3, PW_VM_FLEXIBLE | PW_VM_STACK, "stack", 1ull * MIB},
    };
    Space space = {maps, sizeof(maps) / sizeof(maps[0]), -1, 0};
    PwVmCensus census;

    /* The layout: fields at their offsets, the name terminated and printable. */
    uint8_t raw[PW_VM_RECORD];
    PwVmRecord record;
    Fake odd = {0x1000, 0x2000, 7, PW_VM_FLEXIBLE, "0123456789abcdef0123456789abcdefXYZ", 0};
    encode(&odd, raw);
    raw[34] = 0x01;
    pw_vm_record_decode(raw, &record);
    assert(record.start == 0x1000 && record.end == 0x2000 && record.protection == 7 && record.flags == PW_VM_FLEXIBLE);
    assert(strlen(record.name) == PW_VM_NAME && record.name[1] == '?' && record.name[0] == '0');

    /* The walk: every mapping, grouped, by kind; the reservation is not asked for residency, direct memory is
     * counted apart. */
    pw_vm_census_walk(&census, query, resident, &space);
    assert(census.mappings == 6 && !census.truncated && !census.failed && census.count == 5);
    assert(census.flexible == (64 + 512 + 64 + 1) * (uint64_t)MIB && census.direct == 600ull * MIB);
    assert(census.other == 10000ull * MIB);
    assert(census.resident == (40 + 380 + 20 + 1) * (uint64_t)MIB);
    assert(census.direct_resident == 600ull * MIB && !census.reserved_resident);
    /* Largest resident first: Wine's direct memory, the anonymous pages, then the two heap arenas together, then
     * the stack. */
    assert(!strcmp(census.groups[0].name, "wine") && census.groups[0].resident == 600ull * MIB);
    assert(!strcmp(census.groups[1].name, "anon") && census.groups[1].resident == 380ull * MIB);
    assert(!strcmp(census.groups[2].name, "libc heap") && census.groups[2].mappings == 2 &&
           census.groups[2].resident == 60ull * MIB && census.groups[2].bytes == 128ull * MIB);
    assert(!strcmp(census.groups[3].name, "stack"));

    /* The largest accessible mappings, largest first: the reservation (no access) is not one. */
    assert(census.big_count == 5 && census.big[0].start == 0x400000000 && census.big[0].bytes == 600ull * MIB);
    assert(census.big[1].start == 0x200000000 && census.big[4].start == 0x2000000000);
    char line[512];
    pw_vm_census_format_big(&census, line, sizeof(line));
    assert(!strncmp(line, "400000000+600MiB:02/3:wine 200000000+512MiB:11/3:anon ", 53));
    pw_vm_census_format(&census, 2, PW_VM_FLEXIBLE, line, sizeof(line));
    assert(!strcmp(line, "anon:11/3=380/512MiB/1 libc heap:01/3=60/128MiB/2"));
    pw_vm_census_format(&census, 9, PW_VM_DIRECT, line, sizeof(line));
    assert(!strcmp(line, "wine:02/3=600/600MiB/1"));
    assert(pw_vm_census_format(&census, 9, 0, line, 10) < 10 && strlen(line) < 10);

    /* Without residency: ordered by mapped size. */
    space.queries = 0;
    pw_vm_census_walk(&census, query, NULL, &space);
    assert(!strcmp(census.groups[0].name, "reserved") && !census.resident);

    /* A failing query stops the walk and says so. */
    space.queries = 0;
    space.fail_at = 2;
    pw_vm_census_walk(&census, query, resident, &space);
    assert(census.failed && census.mappings == 2);
    puts("vm census: record layout, walk, groups by name and flags, residency, order, log line, failure");
    return 0;
}
