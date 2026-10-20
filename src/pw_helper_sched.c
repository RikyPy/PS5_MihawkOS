/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pw_helper_sched.h"
#include <errno.h>
#include <string.h>

/* The value of entry if it is NAME=..., else NULL. */
static const char *value_of(const char *entry, const char *name)
{
    const size_t length = strlen(name);
    return !strncmp(entry, name, length) && entry[length] == '=' ? entry + length + 1 : NULL;
}

/* Decimal digits up to stop, at most 4 of them; *text is left at stop. -1 when anything else is there. */
static int decimal(const char **text, char stop)
{
    int value = 0, digits = 0;
    while (**text >= '0' && **text <= '9') {
        if (++digits > 4) return -1;
        value = value * 10 + (*(*text)++ - '0');
    }
    return digits && **text == stop ? value : -1;
}

static int priority_pair(const char *text, int *service, int *program)
{
    const int s = decimal(&text, ',');
    if (s < 0) return EINVAL;
    ++text;
    const int p = decimal(&text, 0);
    if (p < 0 || s < PW_HELPER_SCHED_HIGHEST || s > p || p > PW_HELPER_SCHED_LOWEST) return EINVAL;
    *service = s;
    *program = p;
    return 0;
}

static int mask(const char *text, uint64_t *out)
{
    uint64_t value = 0;
    unsigned digits = 0;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) text += 2;
    for (; *text; ++text, ++digits) {
        const char c = *text;
        const int nibble = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (nibble < 0 || digits == 16) return EINVAL;
        value = value << 4 | (uint64_t)nibble;
    }
    if (!value) return EINVAL;
    *out = value;
    return 0;
}

void pw_helper_sched_read(const char *block, size_t bytes, PwHelperSched *out)
{
    *out = (PwHelperSched){.round_robin = 1, .service = PW_HELPER_SCHED_SERVICE_DEFAULT,
                           .program = PW_HELPER_SCHED_PROGRAM_DEFAULT};
    for (size_t at = 0; at < bytes && block[at];) {
        const char *entry = block + at, *end = memchr(entry, 0, bytes - at), *value;
        if (!end) break;
        at += (size_t)(end - entry) + 1;
        int error = 0;
        if ((value = value_of(entry, "WINE_PS5_HELPER_SCHED"))) {
            if (!strcmp(value, "rr")) out->round_robin = 1;
            else if (!strcmp(value, "keep")) out->round_robin = 0;
            else error = EINVAL;
        } else if ((value = value_of(entry, "WINE_PS5_HELPER_PRIORITY"))) {
            error = priority_pair(value, &out->service, &out->program);
        } else if ((value = value_of(entry, "WINE_PS5_HELPER_CPUS"))) {
            error = mask(value, &out->cpus);
        } else if ((value = value_of(entry, "WINE_PS5_SCHED_CENSUS"))) {
            if (!strcmp(value, "1") || !strcmp(value, "0")) out->census = value[0] == '1';
            else error = EINVAL;
        }
        if (error) out->error = error;
    }
}

static int measured(int policy) { return policy == PW_HELPER_SCHED_FIFO || policy == PW_HELPER_SCHED_RR; }

int pw_helper_sched_plan(const PwHelperSched *settings, int tier, int entry_policy, int plain_policy, int plain_priority,
                         uint64_t entry_cpus, PwHelperSchedPlan *out)
{
    *out = (PwHelperSchedPlan){0};
    if (!settings->round_robin) return 0;
    if (!measured(entry_policy) || !measured(plain_policy)) return EINVAL;
    if (tier == PW_HELPER_SCHED_SERVICE) {
        if (settings->service) *out = (PwHelperSchedPlan){1, PW_HELPER_SCHED_RR, settings->service, 0};
        return 0;
    }
    if (tier != PW_HELPER_SCHED_PROGRAM) return EINVAL;
    const int priority = settings->program ? settings->program : plain_priority;
    if (priority < PW_HELPER_SCHED_HIGHEST || priority > PW_HELPER_SCHED_LOWEST) return EINVAL;
    *out = (PwHelperSchedPlan){1, PW_HELPER_SCHED_RR, priority, 0};
    if (!settings->cpus) return 0;
    if (!entry_cpus || (settings->cpus & ~entry_cpus)) return EINVAL;
    out->cpus = settings->cpus;
    return 0;
}

int pw_helper_sched_wanted(const char *comm, int pid, int ppid, int websrv, int elfldr)
{
    if ((websrv > 0 && (ppid == websrv || pid == websrv)) || (elfldr > 0 && (ppid == elfldr || pid == elfldr)))
        return 1;
    return !strcmp(comm, "eboot.bin") || !strncmp(comm, "SceShell", 8) || !strncmp(comm, "SceSysCore", 10);
}

int pw_helper_sched_group(PwHelperSchedGroup *groups, unsigned cap, unsigned *count, unsigned type, unsigned priority,
                          uint64_t cpus)
{
    for (unsigned i = 0; i < *count; ++i)
        if (groups[i].type == type && groups[i].priority == priority && groups[i].cpus == cpus) {
            groups[i].threads++;
            return 0;
        }
    if (*count >= cap) return -1;
    groups[(*count)++] = (PwHelperSchedGroup){type, priority, cpus, 1};
    return 0;
}
