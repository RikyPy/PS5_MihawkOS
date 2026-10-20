/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The broker's frame pacing record (src/pw_frame_pacing.h), against intervals computed here by hand. */
#include "../src/pw_frame_pacing.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const uint64_t ms = 1000000ull;

static double field(const char *line, const char *name)
{
    char key[32];
    snprintf(key, sizeof(key), " %s=", name);
    const char *at = strstr(line, key);
    if (!at && !strncmp(line, key + 1, strlen(key + 1))) at = line - 1;
    assert(at);
    double value = 0;
    assert(sscanf(at + strlen(key), "%lf", &value) == 1);
    return value;
}

int main(void)
{
    {   /* A steady 60 Hz client: 120 shown presents, 2 ms each, 16.667 ms apart. */
        PwFramePacing p = {0};
        const uint64_t step = 16666667ull;
        for (unsigned i = 0; i < 120; ++i) pw_frame_pacing_shown(&p, 1000 * ms + i * step - 2 * ms, 1000 * ms + i * step);
        assert(p.frames == 120 && p.intervals == 119 && !p.hidden);
        /* 16.667 ms is in bucket 66 (16.50..16.75 ms): every percentile is that bucket's upper edge. */
        assert(pw_frame_pacing_percentile_us(&p, 50) == 16750 && pw_frame_pacing_percentile_us(&p, 99) == 16750);
        assert(p.interval_max_ns == step && !p.over_20ms && !p.over_34ms);
        assert(p.present_ns == 120 * 2 * ms && p.present_max_ns == 2 * ms);
        char line[256];
        const uint64_t now = 1000 * ms + 119 * step;
        pw_frame_pacing_format(&p, now, line, sizeof(line));
        assert(field(line, "frames") == 120 && field(line, "p50") == 16.75 && field(line, "max") == 16.67);
        assert(field(line, "present_avg") == 2.0 && field(line, "over20") == 0);
        assert(!pw_frame_pacing_due(&p, now));
        assert(pw_frame_pacing_due(&p, 1000 * ms + PW_PACING_REPORT_NS));
    }
    {   /* Stalls: one 25 ms and one 40 ms gap among 16 ms frames; hidden presents are no interval. */
        PwFramePacing p = {0};
        uint64_t t = 50 * ms;
        pw_frame_pacing_shown(&p, t, t);
        for (unsigned i = 0; i < 98; ++i) { t += 16 * ms; pw_frame_pacing_shown(&p, t, t); }
        t += 25 * ms; pw_frame_pacing_shown(&p, t, t);
        pw_frame_pacing_hidden(&p);
        pw_frame_pacing_hidden(&p);
        t += 40 * ms; pw_frame_pacing_shown(&p, t - 3 * ms, t);
        assert(p.frames == 101 && p.hidden == 2 && p.intervals == 100);
        assert(p.over_20ms == 2 && p.over_34ms == 1 && p.interval_max_ns == 40 * ms);
        /* 98 of 100 intervals are 16 ms, the bucket ending at 16.25 ms; the 99th percentile reaches the 25 ms one. */
        assert(pw_frame_pacing_percentile_us(&p, 98) == 16250);
        assert(pw_frame_pacing_percentile_us(&p, 99) == 25250 && pw_frame_pacing_percentile_us(&p, 100) == 40250);
        assert(p.present_max_ns == 3 * ms);
    }
    {   /* Beyond the buckets: the overflow bucket answers with the exact maximum. */
        PwFramePacing p = {0};
        pw_frame_pacing_shown(&p, 0, 10 * ms);
        pw_frame_pacing_shown(&p, 10 * ms, 160 * ms);
        assert(p.interval[PW_PACING_BUCKETS] == 1 && pw_frame_pacing_percentile_us(&p, 50) == 150000);
        /* A reply that ended before it began counts no time. */
        pw_frame_pacing_shown(&p, 200 * ms, 170 * ms);
        assert(p.frames == 3 && p.present_max_ns == 150 * ms);
    }
    {   /* The next window keeps the newest end, so the interval across the boundary is not lost. */
        PwFramePacing p = {0};
        pw_frame_pacing_shown(&p, 0, 5 * ms);
        pw_frame_pacing_shown(&p, 5 * ms, 21 * ms);
        pw_frame_pacing_next(&p, 30 * ms);
        assert(!p.frames && !p.intervals && p.last_ns == 21 * ms && p.window_ns == 30 * ms);
        assert(!pw_frame_pacing_due(&p, 30 * ms + PW_PACING_REPORT_NS));
        pw_frame_pacing_shown(&p, 33 * ms, 37 * ms);
        assert(p.intervals == 1 && p.interval_max_ns == 16 * ms && pw_frame_pacing_due(&p, 30 * ms + PW_PACING_REPORT_NS));
        assert(!pw_frame_pacing_percentile_us(&(PwFramePacing){0}, 50));
    }
    {   /* A capture's present between two windows: neither its time nor an interval across it or from it counts, and the
         * new window starts at its end; a skip inside a window keeps that window's start. */
        PwFramePacing p = {0};
        pw_frame_pacing_shown(&p, 0, 16 * ms);
        pw_frame_pacing_next(&p, 20 * ms);
        pw_frame_pacing_skip(&p, 650 * ms);
        assert(!p.frames && !p.intervals && !p.last_ns && p.window_ns == 650 * ms);
        pw_frame_pacing_shown(&p, 706 * ms, 712 * ms);
        assert(p.frames == 1 && !p.intervals && p.present_max_ns == 6 * ms && p.last_ns == 712 * ms);
        pw_frame_pacing_shown(&p, 722 * ms, 729 * ms);
        assert(p.frames == 2 && p.intervals == 1 && p.interval_max_ns == 17 * ms && p.present_max_ns == 7 * ms);
        pw_frame_pacing_skip(&p, 800 * ms);
        assert(p.window_ns == 650 * ms && !p.last_ns && p.frames == 2);
    }
    {   /* Slow intervals by cause: an idle look means the content came no sooner; a busy broker is blamed on its longest
         * part (compose, copy, buffer wait, or the rest of the interval); 20 ms itself is not slow. */
        PwPacingSlow s = {0};
        assert(pw_frame_pacing_slow(&s, 20 * ms, 3, 1 * ms, 7 * ms, 0) == PW_PACING_SLOW_CAUSES);
        assert(pw_frame_pacing_slow(&s, 0, 0, 0, 0, 0) == PW_PACING_SLOW_CAUSES);
        assert(pw_frame_pacing_slow(&s, 24 * ms, 2, 2 * ms, 7 * ms, 1 * ms) == PW_PACING_SLOW_CONTENT);
        assert(pw_frame_pacing_slow(&s, 25 * ms, 0, 12 * ms, 7 * ms, 1 * ms) == PW_PACING_SLOW_COMPOSE);
        assert(pw_frame_pacing_slow(&s, 21 * ms, 0, 2 * ms, 9 * ms, 1 * ms) == PW_PACING_SLOW_COPY);
        assert(pw_frame_pacing_slow(&s, 30 * ms, 0, 2 * ms, 7 * ms, 16 * ms) == PW_PACING_SLOW_WAIT);
        assert(pw_frame_pacing_slow(&s, 40 * ms, 0, 2 * ms, 7 * ms, 1 * ms) == PW_PACING_SLOW_OTHER);
        assert(s.count[PW_PACING_SLOW_CONTENT] == 1 && s.count[PW_PACING_SLOW_COMPOSE] == 1 && s.count[PW_PACING_SLOW_COPY] == 1 &&
               s.count[PW_PACING_SLOW_WAIT] == 1 && s.count[PW_PACING_SLOW_OTHER] == 1);
        char line[256];
        pw_frame_pacing_slow_format(&s, line, sizeof(line));
        assert(field(line, "content") == 1 && field(line, "other") == 1 && field(line, "interval") == 28.0);
        assert(field(line, "copy") == 1 && field(line, "copy_ms") == 7.4 && field(line, "looks") == 0.4);
        PwPacingSlow none = {0};
        pw_frame_pacing_slow_format(&none, line, sizeof(line));
        assert(field(line, "content") == 0 && field(line, "interval") == 0);
    }
    puts("frame pacing: intervals, percentiles, stalls, hidden presents, windows, skipped captures and slow intervals' causes "
         "as computed by hand");
    return 0;
}
