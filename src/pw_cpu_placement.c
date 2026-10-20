/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pw_cpu_placement.h"
#include <string.h>

#define SWITCH_NAME "WINE_PS5_HELPER_CPUS_WIDEN"

int pw_cpu_placement_enabled(const char *block, size_t bytes)
{
    if (!block || !bytes) return 0;
    for (size_t offset = 0; offset < bytes && block[offset]; ) {
        const char *entry = block + offset;
        size_t left = bytes - offset;
        const char *end = memchr(entry, 0, left);
        if (!end) return 0;                       /* unterminated: nothing to trust */
        if ((size_t)(end - entry) > sizeof(SWITCH_NAME) &&
            !memcmp(entry, SWITCH_NAME "=", sizeof(SWITCH_NAME)) && !strcmp(entry + sizeof(SWITCH_NAME), "1"))
            return 1;
        offset += (size_t)(end - entry) + 1;
    }
    return 0;                                     /* empty list, or the switch unset */
}

static int read_level(const PwCpuPlacementOps *ops, int level, uint64_t *mask, PwCpuPlacement *out)
{
    int error = 0;
    *mask = 0;
    if (ops->read(level, mask, &error)) {
        out->error = PW_CPU_PLACEMENT_ERR_READ;
        out->errno_value = error;
        return -1;
    }
    return 0;
}

static int restore(const PwCpuPlacementOps *ops, PwCpuPlacement *out)
{
    int error = 0;
    if (out->old_set > 0 && !ops->move(out->old_set, &error)) return 0;
    out->restore_failed = 1;
    out->error = PW_CPU_PLACEMENT_ERR_RESTORE;
    if (error) out->errno_value = error;
    return -1;
}

/* After every change: the helper must be able to run on all of target. Returns -1 on failure, always. */
static int verify(const PwCpuPlacementOps *ops, PwCpuPlacement *out, int moved)
{
    if (read_level(ops, PW_CPU_PLACEMENT_WHICH, &out->after_affinity, out) ||
        read_level(ops, PW_CPU_PLACEMENT_SET, &out->after_set, out) ||
        read_level(ops, PW_CPU_PLACEMENT_ROOT, &out->after_root, out)) {
        if (moved) (void)restore(ops, out);
        return -1;
    }
    if ((out->after_affinity & out->target) == out->target) return 0;
    out->error = PW_CPU_PLACEMENT_ERR_VERIFY;
    if (moved) (void)restore(ops, out);
    return -1;
}

int pw_cpu_placement_apply(const PwCpuPlacementOps *ops, int enabled, uint64_t target, PwCpuPlacement *out)
{
    int error = 0, moved = 0;
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    out->target = target;
    out->status = PW_CPU_PLACEMENT_OFF;
    if (!enabled) return 0;
    if (!ops || !ops->read || !ops->create_set || !ops->get_set || !ops->move || !ops->set_affinity) {
        out->error = PW_CPU_PLACEMENT_ERR_ARGS;
        return -1;
    }
    if (!target) {
        out->error = PW_CPU_PLACEMENT_ERR_TARGET;
        return -1;
    }
    if (read_level(ops, PW_CPU_PLACEMENT_WHICH, &out->affinity, out) ||
        read_level(ops, PW_CPU_PLACEMENT_SET, &out->set, out) ||
        read_level(ops, PW_CPU_PLACEMENT_ROOT, &out->root, out))
        return -1;
    /* The root set is the widest this process may ever be; a target outside it cannot be reached at all. */
    if (target & ~out->root) {
        out->error = PW_CPU_PLACEMENT_ERR_NOT_IN_ROOT;
        return -1;
    }
    if ((out->set & target) == target && out->affinity == target) {
        out->after_affinity = out->affinity;
        out->after_set = out->set;
        out->after_root = out->root;
        out->status = PW_CPU_PLACEMENT_ALREADY;
        return 0;
    }
    /* Leaving the set: its id is what a failed attempt moves back to, so read it first. */
    if ((out->set & target) != target) {
        if (ops->get_set(&out->old_set, &error)) {
            out->error = PW_CPU_PLACEMENT_ERR_SET_ID;
            out->errno_value = error;
            return -1;
        }
        if (ops->create_set(&out->new_set, &error)) {
            out->error = PW_CPU_PLACEMENT_ERR_SET_CREATE;
            out->errno_value = error;
            return -1;
        }
        /* Narrow the new set itself when the kernel lets this process: every thread that inherits it is then inside
         * target, and no privilege-bearing process-wide affinity call is needed. A refusal here is not fatal. */
        if (ops->set_mask && ops->set_mask(out->new_set, target, &error)) out->set_mask_error = error;
        if (ops->move(out->new_set, &error)) {
            out->error = PW_CPU_PLACEMENT_ERR_SET_MOVE;
            out->errno_value = error;
            return -1;      /* the move failed: the process is still in its old set */
        }
        moved = 1;
        if (read_level(ops, PW_CPU_PLACEMENT_WHICH, &out->after_affinity, out)) {
            (void)restore(ops, out);
            return -1;
        }
    }
    /* The set holds the target now (or already did). Narrow this thread to exactly it when the kernel lets a payload:
     * a refusal that still leaves every target CPU reachable is reported, not undone; one that would leave the helper
     * narrower is undone. */
    if (!moved) out->after_affinity = out->affinity;
    if (out->after_affinity != target) {
        if (ops->set_affinity(target, &error)) {
            if ((out->after_affinity & target) == target) {
                out->narrow_error = error;
            } else {
                out->error = PW_CPU_PLACEMENT_ERR_AFFINITY;
                out->errno_value = error;
                if (moved) (void)restore(ops, out);
                return -1;
            }
        }
    }
    if (verify(ops, out, moved)) return -1;
    out->status = PW_CPU_PLACEMENT_MOVED;
    return 0;
}
