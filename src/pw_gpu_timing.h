/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_GPU_TIMING_H
#define PW_GPU_TIMING_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
/* Where a GPU client's frame time goes, as the broker sees it: the GPU's work on the client's submissions, what its
 * presents cost on both sides of the broker, and so what is left for the game's and Wine's CPU (docs/PLAN.md,
 * a Direct3D 12 game above 120 fps). One "gpu-split" line per client beside each "gpu-pacing" line (graphics patch
 * 0021), over the same window.
 *
 * GPU time, only with PROTON_GPU_TIMESTAMPS=1 in the title (off, no submission changes): the broker wraps each
 * submission in the two timestamps RADV writes for vkCmdWriteTimestamp (radv_write_timestamp, PS5_Mesa 1ba9874): a
 * COPY_DATA of the GPU clock when the command processor reaches the submission's start, and a bottom-of-pipe
 * RELEASE_MEM once its work is done. One ring runs every client's work in order, so the part of [start, end] after
 * the previous submission's end belongs to this one alone; summed, that is the time the GPU worked, never counted
 * twice. The clock's rate is measured against CLOCK_MONOTONIC across the run, not assumed.
 *
 * Present, always: the broker's copy of the frame into the display's buffer (display.cpp) and the rest of its present
 * (the wait for a free buffer and the flip), and the client's share, which it sends about once a second
 * (PW_GPU_TIMING): its wait for the frame's fence before presenting, its cache flush of the frame, the present's round
 * trip, and its submissions' round trips. Nothing here locks or allocates: the broker calls it under its own lock. */
enum { PW_GPU_TS_START_WORDS = 6, PW_GPU_TS_END_WORDS = 8, PW_GPU_TS_SLOT_BYTES = 16 };

/* PKT3(COPY_DATA, 4): source the GPU clock (9), destination memory (5), 64 bits, write confirmed. */
static inline void pw_gpu_ts_start(uint32_t words[PW_GPU_TS_START_WORDS], uint64_t va)
{
    words[0] = 0xc0044000u;
    words[1] = 9u | 5u << 8 | 1u << 16 | 1u << 20;
    words[2] = words[3] = 0;
    words[4] = (uint32_t)va;
    words[5] = (uint32_t)(va >> 32);
}
/* PKT3(RELEASE_MEM, 6): event BOTTOM_OF_PIPE_TS (40), event index 5, data the GPU clock (3), to memory, sent after
 * the write is confirmed (3). */
static inline void pw_gpu_ts_end(uint32_t words[PW_GPU_TS_END_WORDS], uint64_t va)
{
    words[0] = 0xc0064900u;
    words[1] = 40u | 5u << 8;
    words[2] = 3u << 29 | 3u << 24;
    words[3] = (uint32_t)va;
    words[4] = (uint32_t)(va >> 32);
    words[5] = words[6] = words[7] = 0;
}

/* The GPU time [start, end] adds that no earlier submission covered; *last_end is the newest end seen so far. A slot
 * the GPU did not write (0) or a backwards pair adds nothing and moves nothing. */
static inline uint64_t pw_gpu_busy(uint64_t *last_end, uint64_t start, uint64_t end)
{
    if (!start || !end || end < start) return 0;
    const uint64_t from = start > *last_end ? start : *last_end;
    if (end > *last_end) *last_end = end;
    return end > from ? end - from : 0;
}

/* The GPU clock against CLOCK_MONOTONIC: its first and newest readings, each noted when the broker saw it. */
typedef struct PwGpuClock { uint64_t first_ns, first_ticks, last_ns, last_ticks; } PwGpuClock;
static inline void pw_gpu_clock_note(PwGpuClock *clock, uint64_t now_ns, uint64_t ticks)
{
    if (!ticks) return;
    if (!clock->first_ns) { clock->first_ns = now_ns; clock->first_ticks = ticks; }
    if (ticks > clock->last_ticks) { clock->last_ns = now_ns; clock->last_ticks = ticks; }
}
/* Ticks per nanosecond, once a second or more lies between the readings; 0 before. */
static inline double pw_gpu_clock_rate(const PwGpuClock *clock)
{
    return clock->last_ns >= clock->first_ns + 1000000000ull && clock->last_ticks > clock->first_ticks
               ? (double)(clock->last_ticks - clock->first_ticks) / (double)(clock->last_ns - clock->first_ns)
               : 0;
}

/* One client's sums over a report window. */
typedef struct PwGpuTiming {
    uint64_t gpu_ticks;                   /* GPU busy on its submissions completed in the window */
    uint64_t submissions;                 /* its submissions in the window */
    uint64_t copy_ns;                     /* the broker's copies of its frames into the display's buffer */
    uint64_t broker_submit_ns;            /* the broker handling its submissions: ring waits, staging, the submit */
    uint64_t client_presents;             /* its own (PW_GPU_TIMING): presents, */
    uint64_t fence_ns, flush_ns, present_rpc_ns;   /* fence waits, frame flushes, present round trips, */
    uint64_t submit_rpc_ns;               /* and submission round trips */
} PwGpuTiming;

/* "frames=... gpu_ms=..." without a newline, per shown frame (the client's own fields per present it counted):
 * frames; gpu_ms the GPU's busy time, gpu_busy the share of the window it was busy, gpu_mhz the clock (all 0 without
 * timestamps); subs; present_ms the broker's whole present, copy_ms its copy; broker_submit_ms; then presents and the
 * client's fence_ms, flush_ms, present_rpc_ms and submit_rpc_ms. The number of characters as snprintf counts them. */
static inline int pw_gpu_timing_format(const PwGpuTiming *t, uint64_t frames, uint64_t window_ns, uint64_t present_ns,
                                       double ticks_per_ns, char *line, size_t size)
{
    const double ms = 1e6, f = frames ? (double)frames : 1, p = t->client_presents ? (double)t->client_presents : 1;
    const double gpu_ns = ticks_per_ns > 0 ? (double)t->gpu_ticks / ticks_per_ns : 0;
    return snprintf(line, size,
                    "frames=%llu gpu_ms=%.2f gpu_busy=%.0f%% gpu_mhz=%.2f subs=%.1f present_ms=%.2f copy_ms=%.2f "
                    "broker_submit_ms=%.2f presents=%llu fence_ms=%.2f flush_ms=%.2f present_rpc_ms=%.2f "
                    "submit_rpc_ms=%.2f",
                    (unsigned long long)frames, gpu_ns / f / ms, window_ns ? 100.0 * gpu_ns / (double)window_ns : 0.0,
                    ticks_per_ns * 1e3, (double)t->submissions / f, (double)present_ns / f / ms,
                    (double)t->copy_ns / f / ms, (double)t->broker_submit_ns / f / ms,
                    (unsigned long long)t->client_presents, (double)t->fence_ns / p / ms, (double)t->flush_ns / p / ms,
                    (double)t->present_rpc_ns / p / ms, (double)t->submit_rpc_ns / p / ms);
}
#endif
