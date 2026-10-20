/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_WINE_SPIN_H
#define PW_WINE_SPIN_H
#include <sched.h>
#include <time.h>

/* The spin locks of the direct-memory engine (pw_wine_dmem.c) and of its alias windows (pw_wine_alias.c), taken in
 * signal handlers too (every fault classify takes the engine's): a waiter spins PW_WINE_SPIN_PAUSES pause loops, then
 * yields once, then sleeps 50 us per look, all async-signal-safe. In the Unity game stalls of 2026-10-16 one thread
 * held the engine's lock 15 to 62 s in NtProtectVirtualMemory (sceKernelMprotect where the record names the call) and
 * the helper burned about 2 CPU-seconds a second meanwhile (CPU Top in a 2026-10-16 run, record removed from the tree; in git history: 95%, 40% and 40% at
 * priority 700; docs/PLAN.md "Kernel mprotect stall", H-B); a holder in the kernel now costs its waiters no core.
 * Holds shorter than the pauses wait as before. Not measured on the console: how long the pauses take there, and what
 * the sleep adds to a contended fault. Host: tests/test_pw_wine_dmem_regions.c (check_lock_waits). */
enum { PW_WINE_SPIN_PAUSES = 1000 };
static inline void pw_wine_spin_lock(int *lock)
{
    unsigned looks = 0;
    while (__atomic_exchange_n(lock, 1, __ATOMIC_ACQUIRE))
        while (__atomic_load_n(lock, __ATOMIC_RELAXED)) {
            if (looks < PW_WINE_SPIN_PAUSES) {
#if defined(__x86_64__) || defined(__i386__)
                __builtin_ia32_pause();
#endif
            } else if (looks == PW_WINE_SPIN_PAUSES) {
                sched_yield();
            } else {
                const struct timespec wait = {0, 50000};
                nanosleep(&wait, NULL);
            }
            if (looks <= PW_WINE_SPIN_PAUSES) looks++;
        }
}
#endif
