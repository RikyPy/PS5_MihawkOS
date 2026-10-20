/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The title's guard over the console's system memory, which every helper draws on and the title does not
 * (src/pw_memory_helper.h has the census it reads). Past about 850 MiB the console swaps. A game whose helpers keep
 * it there swaps it until it stops answering: a launcher's held it between 865 and 970 MiB for about three minutes, and
 * the console had to be restarted (2026-10-07; record removed from the tree, in git history). A game that only passes
 * through is fine: a .NET (MonoGame) game's reached 857 MiB while it loaded, then settled near 810 MiB and played (2026-10-06).
 *
 * So the guard asks the game to close as a user would once the census has stood at the limit (870 MiB unless the
 * title says otherwise) for 20 of the last 30 s, and to be ended at once when it stands 70 MiB past it for 3 s. Each
 * is said once. It reads time from the samples, so a census that stops coming in decides nothing.
 *
 * And a climb that is too steep for either: the console's own limit for the system's resident memory is about
 * 989 MiB ("RSS x / 989" in SceShellCore's VM Stats line), and past it the console swaps and stops answering.
 * 2026-10-11 (record removed from the tree; in git history): a launcher's GPU process went from 60 to 331 MiB and the system from 670 to 919 MiB
 * (the census) in 10 s, and the kernel read 929 then 1026 MiB, the console froze and had to be restarted: the 3 s
 * past 940 MiB came too late. So a census that is within 10 MiB of the limit or past it, and has risen by 120 MiB in
 * the last 8 s, ends the game at once. A slow arrival in the same band (the launcher's 865 to 948 MiB for minutes, the
 * .NET game's 857) is not touched. It applies to the default end mark only: a run that sets its own end mark has
 * chosen to go past the limit.
 */
#ifndef PW_MEMORY_GUARD_H
#define PW_MEMORY_GUARD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PW_MEMORY_GUARD_LIMIT_MIB 870u      /* the default limit */
#define PW_MEMORY_GUARD_END_MARGIN_MIB 70u   /* how far past it a game is ended at once */
#define PW_MEMORY_GUARD_WINDOW_MS 30000u     /* the window the limit is counted over */
#define PW_MEMORY_GUARD_CLOSE_MS 20000u      /* how long at the limit, within it, asks the game to close */
#define PW_MEMORY_GUARD_END_MS 3000u         /* how long past the end mark ends it */
#define PW_MEMORY_GUARD_STEEP_RISE_MIB 120u  /* a rise this big within the steep window, at the limit or near it... */
#define PW_MEMORY_GUARD_STEEP_WINDOW_MS 8000u
#define PW_MEMORY_GUARD_STEEP_NEAR_MIB 10u   /* ...counts from this far below the limit, and ends the game at once */

enum pw_memory_verdict {
    PW_MEMORY_FINE,     /* nothing to do */
    PW_MEMORY_CLOSE,    /* ask the game to close (said once) */
    PW_MEMORY_END,      /* end it now (said once) */
};

struct pw_memory_guard {
    uint64_t limit, end;               /* bytes; limit 0 is no guard */
    uint64_t last_ms;                  /* the previous sample's time, 0 before the first */
    uint64_t end_since_ms;             /* when the census went past the end mark and stayed, 0 when it is not */
    uint32_t spans[64];                /* the time each recent sample stood at the limit, ms, oldest first */
    uint64_t times[64];                /* and when each was taken */
    unsigned count;                    /* how many of them are kept */
    int closed, ended;                 /* each verdict is said once */
    uint64_t rise_times[32];           /* recent censuses for the steep-climb rule */
    uint64_t rise_bytes[32];
    unsigned rise_count;
};

/* limit_mib 0 makes a guard that never acts; end_mib 0 puts the end mark PW_MEMORY_GUARD_END_MARGIN_MIB past the
 * limit. */
void pw_memory_guard_init(struct pw_memory_guard *guard, unsigned limit_mib, unsigned end_mib);
/* A census of system bytes taken at now_ms (the census's own time; one not newer than the last is ignored). */
enum pw_memory_verdict pw_memory_guard_sample(struct pw_memory_guard *guard, uint64_t system, uint64_t now_ms);
/* After the guard acted on one process in the desktop session (the title ends the process that holds the memory, not
 * the session): it starts over, so the next process that holds the memory is acted on too, after the same times. */
void pw_memory_guard_rearm(struct pw_memory_guard *guard);
/* How long the census stood at the limit within the window, ms, as of the last sample. */
uint64_t pw_memory_guard_pressure_ms(const struct pw_memory_guard *guard);

#ifdef __cplusplus
}
#endif

#endif
