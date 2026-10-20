/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_FRAME_PACING_H
#define PW_FRAME_PACING_H
#include <stddef.h>
#include <stdint.h>
/* How a GPU client's frames reached the screen, as the broker saw them: one record per client and route, reported
 * every PW_PACING_REPORT_NS of activity and at close.
 *
 * The question it answers (docs/PLAN.md, priority 1): is a game slower or less even on the Mihawk desktop, where the
 * broker copies each completed GPU frame and composes the desktop on the CPU, than in the launcher, where the
 * broker flips the game's own buffer? In both routes the broker replies to a present only after the display flip,
 * so the interval between two shown presents' ends is the time between two new game frames on the screen, and the
 * time inside a present is what the route costs the game (copy, composition and flip wait; or the flip wait alone).
 * A present the broker did not show (another process held the screen) is counted as hidden, never as an interval.
 *
 * Intervals go into 250 us buckets up to 100 ms and one overflow bucket; a percentile is its bucket's upper edge,
 * the maximum is exact. Nothing here locks or allocates: the broker calls it under its own lock. */
enum { PW_PACING_BUCKET_NS = 250000, PW_PACING_BUCKETS = 400 };
#define PW_PACING_REPORT_NS 10000000000ull
typedef struct PwFramePacing {
    uint64_t window_ns;                       /* start of the current report window (first shown present's end) */
    uint64_t last_ns;                         /* end of the newest shown present; 0 before the first */
    uint64_t frames, hidden, intervals;
    uint32_t interval[PW_PACING_BUCKETS + 1];
    uint64_t interval_max_ns, interval_sum_ns;
    uint64_t present_ns, present_max_ns;      /* time inside shown presents */
    uint32_t over_20ms, over_34ms;            /* intervals longer than a 50 Hz and a 30 Hz frame */
} PwFramePacing;
#ifdef __cplusplus
extern "C" {
#endif
/* A present handled from begin_ns to end_ns (monotonic) that the display showed. */
void pw_frame_pacing_shown(PwFramePacing *pacing, uint64_t begin_ns, uint64_t end_ns);
void pw_frame_pacing_hidden(PwFramePacing *pacing);
/* Percentile (1..100) of the window's intervals in microseconds, the upper edge of its bucket; 0 without any. */
uint32_t pw_frame_pacing_percentile_us(const PwFramePacing *pacing, unsigned percent);
/* Whether the window has run PW_PACING_REPORT_NS by now_ns and has something to say. */
int pw_frame_pacing_due(const PwFramePacing *pacing, uint64_t now_ns);
/* One line, without a newline: "frames=... ms" fields, for the caller to prefix with its identity. The number of
 * characters written as snprintf counts them. */
int pw_frame_pacing_format(const PwFramePacing *pacing, uint64_t now_ns, char *line, size_t size);
/* Start the next window; the newest present's end stays, so the next interval is measured across the boundary. */
void pw_frame_pacing_next(PwFramePacing *pacing, uint64_t now_ns);
/* A present that is not the client's pacing (a probe's display capture: the broker writes its picture before the flip,
 * 630 ms in frame-pacing-2026-10-17-01): nothing is counted, nor the interval from its end to the next present, which
 * is the probe waiting for its picture (the one interval over 20 ms of a 60 Hz drag, 56 ms, in
 * frame-pacing-2026-10-17-shell-batch-01); a window that has nothing in it yet starts at its end. */
void pw_frame_pacing_skip(PwFramePacing *pacing, uint64_t end_ns);

/* Why an interval between two of the composed desktop's shown presents ran over 20 ms (graphics patch 0020). The
 * broker looks at the windows every 4 ms: if one of its looks since the last present found nothing new to show, the
 * windows changed no sooner (the content's own cadence: an application's moves, a probe's); otherwise the broker was
 * busy the whole interval, and the cause is what took longest of composing, copying the frame into the display's
 * buffer, waiting in the present for a free buffer (the display behind), and the rest (its 4 ms sleep, the snapshot,
 * the scene, its locks). Kept in memory and written once per phase: no write under the display lock. */
enum { PW_PACING_SLOW_CONTENT, PW_PACING_SLOW_COMPOSE, PW_PACING_SLOW_COPY, PW_PACING_SLOW_WAIT, PW_PACING_SLOW_OTHER,
       PW_PACING_SLOW_CAUSES };
typedef struct PwPacingSlow {
    uint32_t count[PW_PACING_SLOW_CAUSES];
    uint64_t interval_ns, compose_ns, copy_ns, wait_ns, idle_looks;  /* sums over the slow intervals */
} PwPacingSlow;
/* One shown present, interval_ns after the previous one (0 for the first), after idle_looks looks that found nothing
 * new; compose_ns, copy_ns and wait_ns are its own. Returns the cause it was counted under, or PW_PACING_SLOW_CAUSES
 * when the interval was not over 20 ms. */
unsigned pw_frame_pacing_slow(PwPacingSlow *slow, uint64_t interval_ns, unsigned idle_looks, uint64_t compose_ns,
                              uint64_t copy_ns, uint64_t wait_ns);
/* "slow content=N compose=N copy=N wait=N other=N mean interval=.. compose_ms=.. copy_ms=.. wait_ms=.. looks=..":
 * the counts by cause, then the slow intervals' means (milliseconds, and idle looks), without a newline; the number
 * of characters as snprintf counts them. */
int pw_frame_pacing_slow_format(const PwPacingSlow *slow, char *line, size_t size);
#ifdef __cplusplus
}
#endif
#endif
