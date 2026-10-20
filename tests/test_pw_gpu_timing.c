/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The broker's frame-time split (src/pw_gpu_timing.h): its timestamp packets against RADV's own encoding, the GPU's
 * busy time against intervals worked out here by hand, the clock's rate, and the report line. */
#include "../src/pw_gpu_timing.h"
#include <assert.h>
#include <string.h>

/* PS5_Mesa 1ba9874 src/amd/common/sid.h, and the fields radv_write_timestamp passes (src/amd/vulkan/radv_query.c,
 * ac_emit_cp_copy_data and ac_emit_cp_release_mem in src/amd/common/ac_cmdbuf_cp.c). */
#define PKT3(op, count) (3u << 30 | ((count) & 0x3fffu) << 16 | ((op) & 0xffu) << 8)
#define PKT3_COPY_DATA 0x40
#define PKT3_RELEASE_MEM 0x49
#define COPY_DATA_SRC_SEL(x) ((x) & 0xf)
#define COPY_DATA_DST_SEL(x) (((unsigned)(x) & 0xf) << 8)
#define COPY_DATA_TIMESTAMP 9
#define COPY_DATA_DST_MEM 5
#define COPY_DATA_COUNT_SEL (1 << 16)
#define COPY_DATA_WR_CONFIRM (1 << 20)
#define EVENT_TYPE(x) ((x) & 0x3f)
#define EVENT_INDEX(x) (((x) & 0xf) << 8)
#define V_028A90_BOTTOM_OF_PIPE_TS 40
#define EOP_DST_SEL(x) ((x) << 16)
#define EOP_INT_SEL(x) ((x) << 24)
#define EOP_DATA_SEL(x) ((unsigned)(x) << 29)

static double field(const char *line, const char *name)
{
    char key[40];
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
    {   /* The packets, word for word, at an address above 4 GiB. */
        const uint64_t va = 0x123456789abcull;
        uint32_t start[PW_GPU_TS_START_WORDS], end[PW_GPU_TS_END_WORDS];
        pw_gpu_ts_start(start, va);
        pw_gpu_ts_end(end, va + 8);
        const uint32_t want_start[] = {PKT3(PKT3_COPY_DATA, 4),
            COPY_DATA_SRC_SEL(COPY_DATA_TIMESTAMP) | COPY_DATA_DST_SEL(COPY_DATA_DST_MEM) | COPY_DATA_WR_CONFIRM |
                COPY_DATA_COUNT_SEL,
            0, 0, 0x56789abcu, 0x1234u};
        const uint32_t want_end[] = {PKT3(PKT3_RELEASE_MEM, 6), EVENT_TYPE(V_028A90_BOTTOM_OF_PIPE_TS) | EVENT_INDEX(5),
            EOP_DST_SEL(0) | EOP_INT_SEL(3) | EOP_DATA_SEL(3), 0x56789ac4u, 0x1234u, 0, 0, 0};
        assert(!memcmp(start, want_start, sizeof(start)) && !memcmp(end, want_end, sizeof(end)));
        /* The completion packet the broker already appends (completion.h) has the same header. */
        assert(end[0] == 0xc0064900u);
    }
    {   /* Busy time on one in-order ring: [10,20] alone, [15,30] overlaps it by 5, [40,50] after a gap, [45,48] inside
         * [40,50], then a slot the GPU never wrote and a backwards pair. */
        uint64_t last = 0;
        assert(pw_gpu_busy(&last, 10, 20) == 10 && last == 20);
        assert(pw_gpu_busy(&last, 15, 30) == 10 && last == 30);
        assert(pw_gpu_busy(&last, 40, 50) == 10 && last == 50);
        assert(pw_gpu_busy(&last, 45, 48) == 0 && last == 50);
        assert(pw_gpu_busy(&last, 0, 60) == 0 && pw_gpu_busy(&last, 60, 0) == 0 && pw_gpu_busy(&last, 70, 65) == 0);
        assert(last == 50);
        assert(pw_gpu_busy(&last, 49, 55) == 5 && last == 55);
    }
    {   /* A 100 MHz clock (0.1 tick/ns), noted 2.5 s apart, with readings that arrive late or out of order. */
        PwGpuClock clock = {0};
        pw_gpu_clock_note(&clock, 5000000000ull, 0);
        assert(!clock.first_ns && pw_gpu_clock_rate(&clock) == 0);
        pw_gpu_clock_note(&clock, 5000000000ull, 700000000ull);
        pw_gpu_clock_note(&clock, 5500000000ull, 750000000ull);
        assert(pw_gpu_clock_rate(&clock) == 0);   /* under a second */
        pw_gpu_clock_note(&clock, 7500000000ull, 950000000ull);
        pw_gpu_clock_note(&clock, 7600000000ull, 940000000ull);   /* an older reading: ignored */
        const double rate = pw_gpu_clock_rate(&clock);
        assert(rate > 0.0999 && rate < 0.1001);
    }
    {   /* A 10 s window of 300 frames: 6 s of GPU work at 100 MHz, 2,700 submissions, 5.8 ms presents with 4.5 ms
         * copies; the client's 290 presents with 12 ms fence waits, 0.5 ms flushes, 6 ms round trips. */
        PwGpuTiming t = {0};
        t.gpu_ticks = 600000000ull;
        t.submissions = 2700;
        t.copy_ns = 300 * 4500000ull;
        t.broker_submit_ns = 300 * 400000ull;
        t.client_presents = 290;
        t.fence_ns = 290 * 12000000ull;
        t.flush_ns = 290 * 500000ull;
        t.present_rpc_ns = 290 * 6000000ull;
        t.submit_rpc_ns = 290 * 1500000ull;
        char line[512];
        const int n = pw_gpu_timing_format(&t, 300, 10000000000ull, 300 * 5800000ull, 0.1, line, sizeof(line));
        assert(n > 0 && n < (int)sizeof(line));
        assert(field(line, "frames") == 300 && field(line, "gpu_ms") == 20.0 && field(line, "gpu_busy") == 60);
        assert(field(line, "gpu_mhz") == 100.0 && field(line, "subs") == 9.0 && field(line, "present_ms") == 5.8);
        assert(field(line, "copy_ms") == 4.5 && field(line, "broker_submit_ms") == 0.4 && field(line, "presents") == 290);
        assert(field(line, "fence_ms") == 12.0 && field(line, "flush_ms") == 0.5 && field(line, "present_rpc_ms") == 6.0);
        assert(field(line, "submit_rpc_ms") == 1.5);
        /* Without timestamps, the GPU fields are 0 and the rest stand. */
        pw_gpu_timing_format(&t, 300, 10000000000ull, 300 * 5800000ull, 0, line, sizeof(line));
        assert(field(line, "gpu_ms") == 0 && field(line, "gpu_busy") == 0 && field(line, "gpu_mhz") == 0);
        assert(field(line, "copy_ms") == 4.5);
        /* An empty window divides by nothing. */
        const PwGpuTiming empty = {0};
        pw_gpu_timing_format(&empty, 0, 0, 0, 0.1, line, sizeof(line));
        assert(field(line, "frames") == 0 && field(line, "gpu_ms") == 0 && field(line, "fence_ms") == 0);
    }
    puts("test_pw_gpu_timing: ok");
    return 0;
}
