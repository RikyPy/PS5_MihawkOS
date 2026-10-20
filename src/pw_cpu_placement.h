/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_CPU_PLACEMENT_H
#define PW_CPU_PLACEMENT_H
#include <stddef.h>
#include <stdint.h>
/* Where a helper may run, placed once, in its loader, before any game thread exists (docs/PLAN.md, "Ended helpers"
 * and the CPU census entry).
 *
 * Why: every helper's own CPU set is 0xea00 (CPUs 9, 11, 13-15) while the title runs on CPUs 0-12 (its affinity
 * 0x1fff, its set and the root set 0xffff); the title's CPUs 1, 3, 5 and 7-12 sat 99% idle while a program's threads
 * used about 1.5 CPUs (gate-d3d12-census-2026-10-17-01/-02: 77 title starts, 477 helper starts). Plain affinity only
 * narrows inside a process's own set, so widening means leaving the set: cpuset() creates one under the root set and
 * cpuset_setid() moves the process into it, both in libkernel_sys. One place, every game, before Wine runs.
 *
 * The switch is the title's WINE_PS5_HELPER_CPUS_WIDEN=1, forwarded to every helper in the bootstrap's settings block
 * (wine/ps5/pw_wine_process.c inherited(), native/proton_wine_child.c sched_entry's block). Unset: off, the loader
 * changes nothing, which is what the commit gate runs.
 *
 * Contract: the caller passes the kernel in an ops table (the console's calls are in native/proton_wine_child.c; a
 * host test fakes them). The target is a mask inside the root set. A move that does not end with the helper able to run
 * on every target CPU is undone by moving back to the set it came from, so a failure never narrows a helper. Nothing
 * here allocates, prints or aborts. */

/* The title's measured affinity, on every start: CPUs 0-12 (docs/PLAN.md; native/proton_wine.cpp "cpu:" line). */
enum { PW_CPU_PLACEMENT_TITLE_CPUS = 0x1fff };

/* What apply() did. */
enum {
    PW_CPU_PLACEMENT_OFF = 0,       /* the switch is off: nothing was read or changed */
    PW_CPU_PLACEMENT_ALREADY = 1,   /* its CPU set and affinity already are the target */
    PW_CPU_PLACEMENT_MOVED = 2,     /* now runs on every target CPU (a new set, or the affinity narrowed) */
};

/* Typed failures: which step refused, never a bare errno. */
enum {
    PW_CPU_PLACEMENT_ERR_ARGS = 1,       /* no ops table */
    PW_CPU_PLACEMENT_ERR_TARGET = 2,     /* the target mask is empty */
    PW_CPU_PLACEMENT_ERR_READ = 3,       /* the helper's CPU sets could not be read */
    PW_CPU_PLACEMENT_ERR_NOT_IN_ROOT = 4,/* the target names CPUs the root set above the helper lacks */
    PW_CPU_PLACEMENT_ERR_SET_ID = 5,     /* the current set's id could not be read (needed to move back) */
    PW_CPU_PLACEMENT_ERR_SET_CREATE = 6, /* cpuset() failed */
    PW_CPU_PLACEMENT_ERR_SET_MOVE = 7,   /* cpuset_setid() failed */
    PW_CPU_PLACEMENT_ERR_AFFINITY = 8,   /* cpuset_setaffinity() failed */
    PW_CPU_PLACEMENT_ERR_VERIFY = 9,     /* after the move the helper still cannot run on every target CPU */
    PW_CPU_PLACEMENT_ERR_RESTORE = 10,   /* a failed attempt could not be undone: the helper is left moved */
};

/* The three levels of cpuset_getaffinity, as apply() names them. */
enum { PW_CPU_PLACEMENT_WHICH = 0, PW_CPU_PLACEMENT_SET = 1, PW_CPU_PLACEMENT_ROOT = 2 };

/* The kernel, as the caller has it. Each returns 0, or -1 and *error set to the kernel's errno. */
typedef struct PwCpuPlacementOps {
    int (*read)(int level, uint64_t *mask, int *error);   /* CPU level of the process's CPU sets */
    int (*create_set)(int *set_id, int *error);           /* cpuset(): a new set under the root set */
    int (*get_set)(int *set_id, int *error);              /* cpuset_getid(CPU_LEVEL_CPUSET, PID, -1) */
    /* cpuset_setaffinity(CPU_LEVEL_CPUSET, CPU_WHICH_CPUSET, set_id): narrow the new set itself, so every thread that
     * inherits it is inside target. Optional: NULL means the caller has no such attempt. */
    int (*set_mask)(int set_id, uint64_t mask, int *error);
    int (*move)(int set_id, int *error);                  /* cpuset_setid(CPU_WHICH_PID, -1, set_id) */
    int (*set_affinity)(uint64_t mask, int *error);       /* cpuset_setaffinity(CPU_LEVEL_WHICH, -1, mask) */
} PwCpuPlacementOps;

typedef struct PwCpuPlacement {
    int status;             /* PW_CPU_PLACEMENT_OFF / ALREADY / MOVED */
    int error;              /* PW_CPU_PLACEMENT_ERR_* or 0 */
    int errno_value;        /* the errno the failing call reported (0 when none) */
    int restore_failed;     /* 1 when a failed attempt could not be moved back */
    int set_mask_error;     /* the errno of a refused set-wide mask (0 when asked or not needed) */
    int narrow_error;       /* the errno of a refused thread narrowing that still left every target CPU reachable */
    int old_set, new_set;   /* set ids; 0 before anything was made */
    uint64_t target;        /* the mask asked for */
    uint64_t affinity, set, root;              /* read before */
    uint64_t after_affinity, after_set, after_root;   /* read after a move or a narrowed affinity */
} PwCpuPlacement;

/* 1 when the settings block (NAME=VALUE, each NUL-terminated, the list by an empty one, as in the bootstrap) holds
 * WINE_PS5_HELPER_CPUS_WIDEN=1. Anything else, or no block, is 0: the switch is off. Never reads past bytes. */
int pw_cpu_placement_enabled(const char *block, size_t bytes);

/* Place this process on target, or report why it could not. Returns 0 for OFF, ALREADY and MOVED with out->status
 * saying which; -1 with a typed out->error otherwise. out is always written. */
int pw_cpu_placement_apply(const PwCpuPlacementOps *ops, int enabled, uint64_t target, PwCpuPlacement *out);

#endif
