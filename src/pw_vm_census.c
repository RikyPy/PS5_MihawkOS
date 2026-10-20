/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pw_vm_census.h"
#include <stdio.h>
#include <string.h>

void pw_vm_record_decode(const uint8_t raw[PW_VM_RECORD], PwVmRecord *out)
{
    uint64_t start, end;
    memset(out, 0, sizeof(*out));
    memcpy(&start, raw, 8);
    memcpy(&end, raw + 8, 8);
    out->start = (uintptr_t)start;
    out->end = (uintptr_t)end;
    memcpy(&out->protection, raw + 24, 4);
    memcpy(&out->type, raw + 28, 4);
    out->flags = raw[32];
    memcpy(out->name, raw + 33, PW_VM_NAME);
    out->name[PW_VM_NAME] = 0;
    for (size_t i = 0; out->name[i]; i++)
        if ((unsigned char)out->name[i] < 0x20 || (unsigned char)out->name[i] > 0x7e) out->name[i] = '?';
}

static PwVmGroup *group_of(PwVmCensus *census, const PwVmRecord *record)
{
    for (unsigned i = 0; i < census->count; i++)
        if (census->groups[i].flags == record->flags && !strcmp(census->groups[i].name, record->name))
            return &census->groups[i];
    if (census->count == PW_VM_GROUPS) return NULL;
    PwVmGroup *group = &census->groups[census->count++];
    memcpy(group->name, record->name, sizeof(group->name));
    group->flags = record->flags;
    group->protection = record->protection;
    return group;
}

void pw_vm_census_walk(PwVmCensus *census, PwVmQuery query, PwVmResident resident, void *context)
{
    uintptr_t address = 0;
    memset(census, 0, sizeof(*census));
    for (;;) {
        uint8_t raw[PW_VM_RECORD];
        PwVmRecord record;
        if (census->mappings == PW_VM_WALK_LIMIT) { census->truncated = 1; break; }
        const int status = query(context, address, raw);
        if (status == 1) break;
        if (status < 0) { census->failed = 1; break; }
        pw_vm_record_decode(raw, &record);
        if (record.end <= record.start || record.end <= address) { census->failed = 1; break; }
        const uint64_t bytes = record.end - record.start;
        census->mappings++;
        /* resident pages of every accessible mapping, direct memory apart: a 2026-10-11 run (record removed from the tree) showed the
         * kernel's resident count of a launcher's GPU process swing by 390 MiB while the mappings walked here, direct
         * ones excluded, held 90 to 130 MiB; the flags are the part of the layout least agreed on, so they do
         * not decide what is asked */
        const uint64_t in = resident ? resident(context, record.start, record.end) : 0;
        if (!record.protection) census->reserved_resident += in;
        else if (record.flags & PW_VM_DIRECT && !(record.flags & PW_VM_FLEXIBLE)) census->direct_resident += in;
        else census->resident += in;
        if (record.flags & PW_VM_FLEXIBLE) census->flexible += bytes;
        else if (record.flags & PW_VM_DIRECT) census->direct += bytes;
        else census->other += bytes;
        if (record.protection) {
            unsigned at = census->big_count < PW_VM_BIG ? census->big_count : PW_VM_BIG;
            while (at > 0 && census->big[at - 1].bytes < bytes) at--;
            if (at < PW_VM_BIG) {
                const unsigned last = census->big_count < PW_VM_BIG ? census->big_count : PW_VM_BIG - 1;
                for (unsigned i = last; i > at; i--) census->big[i] = census->big[i - 1];
                census->big[at].start = record.start;
                census->big[at].bytes = bytes;
                census->big[at].protection = record.protection;
                census->big[at].flags = record.flags;
                memcpy(census->big[at].name, record.name, sizeof(census->big[at].name));
                if (census->big_count < PW_VM_BIG) census->big_count++;
            }
        }
        PwVmGroup *group = group_of(census, &record);
        if (group) { group->mappings++; group->bytes += bytes; group->resident += in; }
        else census->unkept += bytes;
        address = record.end;
    }
    /* largest first */
    for (unsigned i = 1; i < census->count; i++)
        for (unsigned j = i; j > 0 && (census->groups[j].resident > census->groups[j - 1].resident ||
                                       (census->groups[j].resident == census->groups[j - 1].resident &&
                                        census->groups[j].bytes > census->groups[j - 1].bytes)); j--) {
            PwVmGroup swap = census->groups[j];
            census->groups[j] = census->groups[j - 1];
            census->groups[j - 1] = swap;
        }
}

size_t pw_vm_census_format(const PwVmCensus *census, unsigned n, uint8_t mask, char *out, size_t size)
{
    size_t used = 0;
    unsigned shown = 0;
    if (!size) return 0;
    out[0] = 0;
    for (unsigned i = 0; i < census->count && shown < n; i++) {
        const PwVmGroup *g = &census->groups[i];
        if (mask && !(g->flags & mask)) continue;
        const int wrote = snprintf(out + used, size - used, "%s%s:%02x/%x=%llu/%lluMiB/%u", shown ? " " : "",
                                   g->name[0] ? g->name : "-", g->flags, (unsigned)g->protection,
                                   (unsigned long long)(g->resident >> 20), (unsigned long long)(g->bytes >> 20),
                                   g->mappings);
        if (wrote < 0 || (size_t)wrote >= size - used) break;
        used += (size_t)wrote;
        shown++;
    }
    return used;
}

size_t pw_vm_census_format_big(const PwVmCensus *census, char *out, size_t size)
{
    size_t used = 0;
    if (!size) return 0;
    out[0] = 0;
    for (unsigned i = 0; i < census->big_count; i++) {
        const PwVmBig *b = &census->big[i];
        const int wrote = snprintf(out + used, size - used, "%s%llx+%lluMiB:%02x/%x:%s", i ? " " : "",
                                   (unsigned long long)b->start, (unsigned long long)(b->bytes >> 20), b->flags,
                                   (unsigned)b->protection, b->name[0] ? b->name : "-");
        if (wrote < 0 || (size_t)wrote >= size - used) break;
        used += (size_t)wrote;
    }
    return used;
}
