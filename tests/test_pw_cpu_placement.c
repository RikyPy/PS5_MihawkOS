/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/pw_cpu_placement.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

/* A fake kernel: the process's three levels before and after, the set ids, and which call fails. */
typedef struct Fake {
    uint64_t level[3];   /* WHICH (main thread), SET, ROOT: read before anything changes */
    uint64_t after[3];   /* WHICH, SET, ROOT: read once a move or an affinity change is in effect */
    int changed;         /* a change is in effect: read answers `after` */
    int created;         /* sets handed out by create_set */
    int moves;           /* move() calls, a restore included */
    int moved_to;
    int mask_set;        /* set_mask() was called and succeeded */
    int affinity_calls;  /* affinity() calls seen */
    uint64_t affinity_mask;
    int no_affinity_effect;  /* the affinity call "succeeds" but the read stays as it was */
    int thread_keeps_mask;   /* the move puts the process in the set but the thread keeps its old mask */
    int fail_create, fail_get, fail_set_mask, fail_move, fail_affinity, fail_restore;
} Fake;

/* The ops carry no user pointer (the module has no globals); one fake is current per test. */
static Fake *current;

static int read_op(int level, uint64_t *mask, int *error)
{
    (void)error;
    assert(current);
    *mask = current->changed ? current->after[level] : current->level[level];
    return 0;
}
static int create_op(int *set_id, int *error)
{
    assert(current);
    if (current->fail_create) { *error = EPERM; return -1; }
    *set_id = ++current->created;
    return 0;
}
static int get_op(int *set_id, int *error)
{
    assert(current);
    if (current->fail_get) { *error = ESRCH; return -1; }
    *set_id = 11; /* where the helper is now */
    return 0;
}
static int set_mask_op(int set_id, uint64_t mask, int *error)
{
    assert(current);
    (void)set_id;
    if (current->fail_set_mask) { *error = EPERM; return -1; }
    current->mask_set = 1;
    /* The new set holds the target, but the process has not moved into it yet: `changed` stays as it is. */
    current->after[PW_CPU_PLACEMENT_SET] = mask;
    current->after[PW_CPU_PLACEMENT_WHICH] = mask;
    return 0;
}
static int move_op(int set_id, int *error)
{
    assert(current);
    if (current->fail_restore && set_id == 11) { *error = EINVAL; return -1; }
    if (current->fail_move && set_id != 11) { *error = EINVAL; return -1; }
    ++current->moves;
    current->moved_to = set_id;
    current->changed = 1;
    current->after[PW_CPU_PLACEMENT_ROOT] = current->level[PW_CPU_PLACEMENT_ROOT];
    /* Without a set-wide mask the moved-into set is as wide as the root set. */
    if (!current->mask_set) {
        current->after[PW_CPU_PLACEMENT_SET] = current->level[PW_CPU_PLACEMENT_ROOT];
        current->after[PW_CPU_PLACEMENT_WHICH] = current->thread_keeps_mask ? current->level[PW_CPU_PLACEMENT_WHICH]
                                                                           : current->level[PW_CPU_PLACEMENT_ROOT];
    }
    return 0;
}
static int affinity_op(uint64_t mask, int *error)
{
    assert(current);
    if (current->fail_affinity) { *error = EINVAL; return -1; }
    ++current->affinity_calls;
    current->affinity_mask = mask;
    if (!current->no_affinity_effect) {
        current->changed = 1;
        current->after[PW_CPU_PLACEMENT_WHICH] = mask;
    }
    return 0;
}

static const PwCpuPlacementOps ops = {read_op, create_op, get_op, set_mask_op, move_op, affinity_op};

/* The measured helper: own set 0xea00 (CPUs 9, 11, 13-15), root 0xffff; the title's CPUs 0-12 = 0x1fff. */
static void helper(Fake *f)
{
    memset(f, 0, sizeof(*f));
    f->level[PW_CPU_PLACEMENT_WHICH] = 0xea00;
    f->level[PW_CPU_PLACEMENT_SET] = 0xea00;
    f->level[PW_CPU_PLACEMENT_ROOT] = 0xffff;
    f->after[PW_CPU_PLACEMENT_ROOT] = 0xffff;
    current = f;
}

static void switch_cases(void)
{
    static const char on[] = "WINE_PS5_HELPER_CPUS_WIDEN=1";
    static const char off[] = "WINE_PS5_HELPER_CPUS_WIDEN=0";
    char two[] = "DXVK_HUD=1\0WINE_PS5_HELPER_CPUS_WIDEN=1\0\0";
    char first[] = "\0WINE_PS5_HELPER_CPUS_WIDEN=1\0\0";
    assert(!pw_cpu_placement_enabled(NULL, 0));
    assert(!pw_cpu_placement_enabled(on, 0));
    assert(pw_cpu_placement_enabled(on, sizeof(on)));
    assert(!pw_cpu_placement_enabled(on, sizeof(on) - 1));
    assert(!pw_cpu_placement_enabled(off, sizeof(off)));
    assert(!pw_cpu_placement_enabled("WINE_PS5_HELPER_CPUS_WIDEN=10", 29));
    assert(!pw_cpu_placement_enabled("WINE_PS5_HELPER_CPUS_WIDEN", 26));
    assert(!pw_cpu_placement_enabled(first, sizeof(first)));
    assert(pw_cpu_placement_enabled(two, sizeof(two) - 1));
}

static void off_case(void)
{
    Fake f;
    PwCpuPlacement out;
    helper(&f);
    assert(!pw_cpu_placement_apply(&ops, 0, PW_CPU_PLACEMENT_TITLE_CPUS, &out));
    assert(out.status == PW_CPU_PLACEMENT_OFF && !out.error && !f.moves && !f.created && !f.affinity_calls);
}

static void already_case(void)
{
    Fake f;
    PwCpuPlacement out;
    helper(&f);
    f.level[PW_CPU_PLACEMENT_WHICH] = 0x1fff;
    f.level[PW_CPU_PLACEMENT_SET] = 0xffff;
    assert(!pw_cpu_placement_apply(&ops, 1, PW_CPU_PLACEMENT_TITLE_CPUS, &out));
    assert(out.status == PW_CPU_PLACEMENT_ALREADY && !out.error && !f.moves && !f.created && !f.affinity_calls);
}

/* The console: cpuset() and cpuset_setid() work and the set's mask can be narrowed, so the move alone places every
 * thread; no process-wide affinity call (EPERM there) is needed. */
static void moved_by_set_mask_case(void)
{
    Fake f;
    PwCpuPlacement out;
    helper(&f);
    assert(!pw_cpu_placement_apply(&ops, 1, PW_CPU_PLACEMENT_TITLE_CPUS, &out));
    assert(out.status == PW_CPU_PLACEMENT_MOVED && !out.error && !out.restore_failed);
    assert(f.created == 1 && f.moves == 1 && f.moved_to == 1 && out.old_set == 11 && out.new_set == 1);
    assert(f.mask_set && !f.affinity_calls && !out.set_mask_error);
    assert(out.affinity == 0xea00 && out.set == 0xea00 && out.root == 0xffff);
    assert(out.after_affinity == 0x1fff && out.after_set == 0x1fff);
}

/* A refused set-wide mask: the move leaves the set at the root's width, then the calling thread narrows itself. */
static void moved_by_affinity_case(void)
{
    Fake f;
    PwCpuPlacement out;
    helper(&f);
    f.fail_set_mask = 1;
    f.thread_keeps_mask = 1;
    assert(!pw_cpu_placement_apply(&ops, 1, PW_CPU_PLACEMENT_TITLE_CPUS, &out));
    assert(out.status == PW_CPU_PLACEMENT_MOVED && !out.error && !out.restore_failed);
    assert(out.set_mask_error == EPERM && f.affinity_calls == 1 && f.affinity_mask == PW_CPU_PLACEMENT_TITLE_CPUS);
    assert(out.after_affinity == 0x1fff && out.after_set == 0xffff);
}

/* Already inside the set: plain affinity, no set made. */
static void affinity_only_case(void)
{
    Fake f;
    PwCpuPlacement out;
    helper(&f);
    f.level[PW_CPU_PLACEMENT_SET] = 0xffff;
    assert(!pw_cpu_placement_apply(&ops, 1, PW_CPU_PLACEMENT_TITLE_CPUS, &out));
    assert(out.status == PW_CPU_PLACEMENT_MOVED && !out.error && !f.created && !f.moves && f.affinity_calls == 1);
    assert(out.after_affinity == 0x1fff);
}

static void not_in_root_case(void)
{
    Fake f;
    PwCpuPlacement out;
    helper(&f);
    f.level[PW_CPU_PLACEMENT_ROOT] = 0x0fff;  /* a root set without CPU 12 */
    assert(pw_cpu_placement_apply(&ops, 1, PW_CPU_PLACEMENT_TITLE_CPUS, &out) == -1);
    assert(out.error == PW_CPU_PLACEMENT_ERR_NOT_IN_ROOT && !f.created && !f.moves);
}

static void create_failure_case(void)
{
    Fake f;
    PwCpuPlacement out;
    helper(&f);
    f.fail_create = 1;
    assert(pw_cpu_placement_apply(&ops, 1, PW_CPU_PLACEMENT_TITLE_CPUS, &out) == -1);
    assert(out.error == PW_CPU_PLACEMENT_ERR_SET_CREATE && out.errno_value == EPERM && !f.moves);
}

static void get_failure_case(void)
{
    Fake f;
    PwCpuPlacement out;
    helper(&f);
    f.fail_get = 1;
    assert(pw_cpu_placement_apply(&ops, 1, PW_CPU_PLACEMENT_TITLE_CPUS, &out) == -1);
    assert(out.error == PW_CPU_PLACEMENT_ERR_SET_ID && !f.created && !f.moves);
}

static void move_failure_case(void)
{
    Fake f;
    PwCpuPlacement out;
    helper(&f);
    f.fail_move = 1;
    assert(pw_cpu_placement_apply(&ops, 1, PW_CPU_PLACEMENT_TITLE_CPUS, &out) == -1);
    assert(out.error == PW_CPU_PLACEMENT_ERR_SET_MOVE && out.errno_value == EINVAL);
    assert(!f.moves && !f.affinity_calls && !out.restore_failed); /* the process never left its set */
}

/* The affinity refused after the move: the helper goes back to the set it came from. */
static void affinity_failure_restores_case(void)
{
    Fake f;
    PwCpuPlacement out;
    helper(&f);
    f.fail_set_mask = 1;   /* the move leaves the set wide and the thread keeps its mask, so affinity is needed */
    f.thread_keeps_mask = 1;
    f.fail_affinity = 1;
    assert(pw_cpu_placement_apply(&ops, 1, PW_CPU_PLACEMENT_TITLE_CPUS, &out) == -1);
    assert(out.error == PW_CPU_PLACEMENT_ERR_AFFINITY && !out.restore_failed);
    assert(f.moves == 2 && f.moved_to == 11); /* into the new set, then back to 11 */
}

/* A kernel that reports success but leaves the thread elsewhere: put the helper back, do not claim a move. */
static void verify_failure_restores_case(void)
{
    Fake f;
    PwCpuPlacement out;
    helper(&f);
    f.fail_set_mask = 1;
    f.thread_keeps_mask = 1;
    f.no_affinity_effect = 1;
    assert(pw_cpu_placement_apply(&ops, 1, PW_CPU_PLACEMENT_TITLE_CPUS, &out) == -1);
    assert(out.error == PW_CPU_PLACEMENT_ERR_VERIFY && !out.restore_failed);
    assert(f.moves == 2 && f.moved_to == 11);
    assert(out.after_affinity == 0xea00);  /* the thread kept the mask it had */
}

/* The kernel refuses the exact narrowing but the helper can still reach every target CPU: keep the wider set, say so,
 * and do not undo the move. */
static void narrow_refused_keeps_widened_case(void)
{
    Fake f;
    PwCpuPlacement out;
    helper(&f);
    f.fail_set_mask = 1;
    f.fail_affinity = 1;
    assert(!pw_cpu_placement_apply(&ops, 1, PW_CPU_PLACEMENT_TITLE_CPUS, &out));
    assert(out.status == PW_CPU_PLACEMENT_MOVED && !out.error && !out.restore_failed);
    assert(out.narrow_error == EINVAL);
    assert(f.moves == 1 && f.moved_to == 1);      /* no move back: the widened set is kept */
    assert(out.after_affinity == 0xffff && (out.after_affinity & out.target) == out.target);
}

static void restore_failure_is_typed(void)
{
    Fake f;
    PwCpuPlacement out;
    helper(&f);
    f.fail_set_mask = 1;
    f.thread_keeps_mask = 1;
    f.no_affinity_effect = 1;
    f.fail_restore = 1;
    assert(pw_cpu_placement_apply(&ops, 1, PW_CPU_PLACEMENT_TITLE_CPUS, &out) == -1);
    assert(out.restore_failed && out.error == PW_CPU_PLACEMENT_ERR_RESTORE);
}

static void args_cases(void)
{
    PwCpuPlacement out;
    assert(pw_cpu_placement_apply(NULL, 1, PW_CPU_PLACEMENT_TITLE_CPUS, &out) == -1);
    assert(out.error == PW_CPU_PLACEMENT_ERR_ARGS);
    {
        Fake f;
        helper(&f);
        assert(pw_cpu_placement_apply(&ops, 1, 0, &out) == -1);
        assert(out.error == PW_CPU_PLACEMENT_ERR_TARGET);
    }
    {
        /* set_mask is optional; everything else is not. */
        static const PwCpuPlacementOps no_mask = {read_op, create_op, get_op, NULL, move_op, affinity_op};
        Fake f;
        helper(&f);
        assert(!pw_cpu_placement_apply(&no_mask, 1, PW_CPU_PLACEMENT_TITLE_CPUS, &out));
        assert(out.status == PW_CPU_PLACEMENT_MOVED && out.set_mask_error == 0);
    }
}

int main(void)
{
    switch_cases();
    off_case();
    already_case();
    moved_by_set_mask_case();
    moved_by_affinity_case();
    affinity_only_case();
    not_in_root_case();
    create_failure_case();
    get_failure_case();
    move_failure_case();
    affinity_failure_restores_case();
    narrow_refused_keeps_widened_case();
    verify_failure_restores_case();
    restore_failure_is_typed();
    args_cases();
    puts("test_pw_cpu_placement: ok");
    return 0;
}
