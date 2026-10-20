/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pw_frame_pacing.h"
#include <stdio.h>
#include <string.h>

void pw_frame_pacing_shown(PwFramePacing *pacing, uint64_t begin_ns, uint64_t end_ns)
{
    if (end_ns < begin_ns) end_ns = begin_ns;
    const uint64_t spent = end_ns - begin_ns;
    if (!pacing->window_ns) pacing->window_ns = end_ns;
    if (pacing->last_ns && end_ns >= pacing->last_ns) {
        const uint64_t interval = end_ns - pacing->last_ns;
        const uint64_t bucket = interval / PW_PACING_BUCKET_NS;
        ++pacing->interval[bucket < PW_PACING_BUCKETS ? bucket : PW_PACING_BUCKETS];
        ++pacing->intervals;
        pacing->interval_sum_ns += interval;
        if (interval > pacing->interval_max_ns) pacing->interval_max_ns = interval;
        pacing->over_20ms += interval > 20000000ull;
        pacing->over_34ms += interval > 34000000ull;
    }
    pacing->last_ns = end_ns;
    ++pacing->frames;
    pacing->present_ns += spent;
    if (spent > pacing->present_max_ns) pacing->present_max_ns = spent;
}

void pw_frame_pacing_hidden(PwFramePacing *pacing)
{
    ++pacing->hidden;
}

uint32_t pw_frame_pacing_percentile_us(const PwFramePacing *pacing, unsigned percent)
{
    if (!pacing->intervals || !percent) return 0;
    if (percent > 100) percent = 100;
    /* The smallest bucket holding at least percent% of the intervals. */
    const uint64_t wanted = (pacing->intervals * percent + 99) / 100;
    uint64_t seen = 0;
    for (unsigned bucket = 0; bucket <= PW_PACING_BUCKETS; ++bucket) {
        seen += pacing->interval[bucket];
        if (seen >= wanted)
            return bucket < PW_PACING_BUCKETS ? (bucket + 1) * (PW_PACING_BUCKET_NS / 1000)
                                              : (uint32_t)(pacing->interval_max_ns / 1000);
    }
    return (uint32_t)(pacing->interval_max_ns / 1000);
}

int pw_frame_pacing_due(const PwFramePacing *pacing, uint64_t now_ns)
{
    return pacing->window_ns && (pacing->frames || pacing->hidden) && now_ns >= pacing->window_ns &&
           now_ns - pacing->window_ns >= PW_PACING_REPORT_NS;
}

int pw_frame_pacing_format(const PwFramePacing *pacing, uint64_t now_ns, char *line, size_t size)
{
    const uint64_t span = pacing->window_ns && now_ns > pacing->window_ns ? now_ns - pacing->window_ns : 0;
    const double seconds = span / 1e9;
    return snprintf(line, size,
        "seconds=%.1f frames=%llu hidden=%llu fps=%.1f mean=%.2f p50=%.2f p95=%.2f p99=%.2f max=%.2f ms "
        "over20=%u over34=%u present_avg=%.2f present_max=%.2f ms",
        seconds, (unsigned long long)pacing->frames, (unsigned long long)pacing->hidden,
        seconds > 0 ? pacing->frames / seconds : 0.0,
        pacing->intervals ? pacing->interval_sum_ns / 1e6 / pacing->intervals : 0.0,
        pw_frame_pacing_percentile_us(pacing, 50) / 1e3, pw_frame_pacing_percentile_us(pacing, 95) / 1e3,
        pw_frame_pacing_percentile_us(pacing, 99) / 1e3, pacing->interval_max_ns / 1e6,
        pacing->over_20ms, pacing->over_34ms,
        pacing->frames ? pacing->present_ns / 1e6 / pacing->frames : 0.0, pacing->present_max_ns / 1e6);
}

void pw_frame_pacing_next(PwFramePacing *pacing, uint64_t now_ns)
{
    const uint64_t last = pacing->last_ns;
    memset(pacing, 0, sizeof(*pacing));
    pacing->last_ns = last;
    pacing->window_ns = now_ns;
}

void pw_frame_pacing_skip(PwFramePacing *pacing, uint64_t end_ns)
{
    pacing->last_ns = 0;
    if (!pacing->frames && !pacing->hidden) pacing->window_ns = end_ns;
}

unsigned pw_frame_pacing_slow(PwPacingSlow *slow, uint64_t interval_ns, unsigned idle_looks, uint64_t compose_ns,
                              uint64_t copy_ns, uint64_t wait_ns)
{
    if (interval_ns <= 20000000ull) return PW_PACING_SLOW_CAUSES;
    const uint64_t busy = compose_ns + copy_ns + wait_ns, other = interval_ns > busy ? interval_ns - busy : 0;
    unsigned cause = PW_PACING_SLOW_CONTENT;
    if (!idle_looks) {
        const uint64_t part[4] = {compose_ns, copy_ns, wait_ns, other};
        unsigned longest = 0;
        for (unsigned i = 1; i < 4; ++i) if (part[i] > part[longest]) longest = i;
        cause = PW_PACING_SLOW_COMPOSE + longest;
    }
    ++slow->count[cause];
    slow->interval_ns += interval_ns; slow->compose_ns += compose_ns; slow->copy_ns += copy_ns; slow->wait_ns += wait_ns;
    slow->idle_looks += idle_looks;
    return cause;
}

int pw_frame_pacing_slow_format(const PwPacingSlow *slow, char *line, size_t size)
{
    uint64_t n = 0;
    for (unsigned i = 0; i < PW_PACING_SLOW_CAUSES; ++i) n += slow->count[i];
    const double d = n ? (double)n : 1.0;
    return snprintf(line, size,
        "slow content=%u compose=%u copy=%u wait=%u other=%u mean interval=%.2f compose_ms=%.2f copy_ms=%.2f wait_ms=%.2f "
        "looks=%.1f",
        slow->count[PW_PACING_SLOW_CONTENT], slow->count[PW_PACING_SLOW_COMPOSE], slow->count[PW_PACING_SLOW_COPY],
        slow->count[PW_PACING_SLOW_WAIT], slow->count[PW_PACING_SLOW_OTHER], slow->interval_ns / 1e6 / d,
        slow->compose_ns / 1e6 / d, slow->copy_ns / 1e6 / d, slow->wait_ns / 1e6 / d, slow->idle_looks / d);
}
