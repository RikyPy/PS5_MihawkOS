/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The GPU broker's spare staging (graphics patch 0018, the foundation's tooling/gpu-broker/staging.h), driven the way
 * its server.cpp drives it: submit takes a spare of its rounded size or makes one, reap gives back what the fence passed
 * (freeing what the budget refuses), teardown frees the spares. The memory is fake (addresses only); what is checked is
 * the contract: no buffer is taken while in flight, a buffer is made only when every one of its size is in flight (so
 * per size never more than were once in flight together), each is its submission's rounded size, and the spares never
 * hold more than the budget. Best fit, 0018's first form, held 262 MiB of spares at the end of this model. */
#include "../.deps/graphics-foundation/tooling/gpu-broker/staging.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static struct ps5_gpu_staging pool;
static unsigned long long made, freed, reuses, refused;
static uint64_t held, held_peak, in_flight_bytes;

/* Per size (bytes / 64 KiB, to 18 MiB): buffers made and not freed, those in flight, and the most in flight at once. */
enum { SIZES = 18 * 16 + 1 };
static unsigned live[SIZES], flying[SIZES], flying_peak[SIZES];
static unsigned size_of(uint64_t bytes)
{
    assert(bytes % PS5_GPU_STAGING_MIN == 0 && bytes / PS5_GPU_STAGING_MIN < SIZES);
    return (unsigned)(bytes / PS5_GPU_STAGING_MIN);
}

static struct radv_ps5_memory make(uint64_t bytes)
{
    struct radv_ps5_memory m = {0};
    m.cpu = (uint8_t *)(uintptr_t)(UINT64_C(0x4000000000) + (uint64_t)++made * UINT64_C(0x4000000));
    m.bytes = bytes;
    return m;
}

/* The fence ring, as server.cpp's: in submission order, reaped from the head once the fence passes. */
enum { RING = 512 };
static struct radv_ps5_memory ring[RING];
static unsigned head, count, peak;

/* server.cpp submit: take a spare of the rounded size, else make one of that size. */
static void submit(uint64_t need)
{
    struct radv_ps5_memory staging;
    assert(count < RING);
    if (ps5_gpu_staging_take(&pool, need, &staging))
        ++reuses;
    else {
        assert(!staging.cpu);
        staging = make(ps5_gpu_staging_round(need));
        /* Made only when every buffer of its size is in flight. */
        assert(live[size_of(staging.bytes)] == flying[size_of(staging.bytes)]);
        ++live[size_of(staging.bytes)];
        held += staging.bytes;
        if (held > held_peak) held_peak = held;
    }
    assert(staging.cpu && staging.bytes == ps5_gpu_staging_round(need));
    for (unsigned i = 0; i < count; ++i) assert(ring[(head + i) % RING].cpu != staging.cpu);
    for (unsigned i = 0; i < pool.count; ++i) assert(pool.spare[i].cpu != staging.cpu);
    ring[(head + count++) % RING] = staging;
    in_flight_bytes += staging.bytes;
    if (count > peak) peak = count;
    const unsigned size = size_of(staging.bytes);
    if (++flying[size] > flying_peak[size]) flying_peak[size] = flying[size];
    assert(live[size] <= flying_peak[size]);
    /* Only what is in flight or spare exists, and the spares stay within the budget. */
    assert(made - freed == count + pool.count && held == in_flight_bytes + pool.bytes);
    assert(pool.bytes <= PS5_GPU_STAGING_BUDGET);
}

/* server.cpp reap: the fence has passed the oldest n; what the spares refuse is freed. */
static void reap(unsigned n)
{
    for (; n && count; --n, --count) {
        const struct radv_ps5_memory m = ring[head];
        --flying[size_of(m.bytes)];
        in_flight_bytes -= m.bytes;
        if (!ps5_gpu_staging_give(&pool, m)) {
            assert(pool.bytes + m.bytes > PS5_GPU_STAGING_BUDGET);
            ++freed, ++refused, --live[size_of(m.bytes)];
            held -= m.bytes;
        }
        ring[head] = (struct radv_ps5_memory){0};
        head = (head + 1) % RING;
    }
    assert(pool.bytes <= PS5_GPU_STAGING_BUDGET);
}

int main(void)
{
    /* Rounding: 64 KiB at least, powers of two to 2 MiB, 2 MiB steps above. */
    assert(ps5_gpu_staging_round(32) == 0x10000 && ps5_gpu_staging_round(4128) == 0x10000);
    assert(ps5_gpu_staging_round(0x10000) == 0x10000 && ps5_gpu_staging_round(0x10001) == 0x20000);
    assert(ps5_gpu_staging_round(0x180000) == 0x200000 && ps5_gpu_staging_round(0x200000) == 0x200000);
    assert(ps5_gpu_staging_round(0x200001) == 0x400000 && ps5_gpu_staging_round(0x600001) == 0x800000);
    assert(ps5_gpu_staging_round(16 * 1024 * 1024 + 32) == 18 * 1024 * 1024);

    {   /* Empty: nothing taken. Only the rounded size is taken, never a bigger spare. The budget refuses, the slots
         * never do first (each buffer is at least 64 KiB, and 512 of those are the budget). */
        struct radv_ps5_memory out;
        assert(!ps5_gpu_staging_take(&pool, 1, &out) && !out.cpu);
        const struct radv_ps5_memory a = make(0x10000), b = make(0x40000), c = make(0x20000);
        assert(ps5_gpu_staging_give(&pool, a) && ps5_gpu_staging_give(&pool, b) && ps5_gpu_staging_give(&pool, c));
        assert(pool.bytes == 0x70000);
        assert(ps5_gpu_staging_take(&pool, 0x18000, &out) && out.cpu == c.cpu && pool.count == 2 && pool.bytes == 0x50000);
        assert(!ps5_gpu_staging_take(&pool, 0x18000, &out) && !out.cpu && pool.count == 2);   /* b is bigger: not taken */
        assert(!ps5_gpu_staging_take(&pool, 0x50000, &out) && !out.cpu && pool.count == 2);
        assert(ps5_gpu_staging_take(&pool, 0x30000, &out) && out.cpu == b.cpu && pool.count == 1);
        assert(pool.spare[0].cpu == a.cpu && !pool.spare[1].cpu);
        assert(ps5_gpu_staging_take(&pool, 0x10000, &out) && out.cpu == a.cpu && !pool.count && !pool.bytes);
        const struct radv_ps5_memory big = make(18u << 20), mid = make(16u << 20), rest = make(14u << 20);
        assert(ps5_gpu_staging_give(&pool, big) && !ps5_gpu_staging_give(&pool, mid) && ps5_gpu_staging_give(&pool, rest));
        assert(pool.bytes == PS5_GPU_STAGING_BUDGET && !ps5_gpu_staging_give(&pool, a) && pool.count == 2);
        pool = (struct ps5_gpu_staging){0};
        for (unsigned i = 0; i < PS5_GPU_STAGING_SPARES; ++i) assert(ps5_gpu_staging_give(&pool, a));
        assert(pool.bytes == PS5_GPU_STAGING_BUDGET && !ps5_gpu_staging_give(&pool, a));
        pool = (struct ps5_gpu_staging){0};
        made = 0;
    }

    /* Play: about 420 submissions a second for 10 minutes, mostly about 4 KiB (2026-10-16; record removed from the tree), sometimes up to
     * 1 MiB and rarely the 16 MiB limit, with 1 to 40 in flight as the GPU falls behind and catches up. */
    srand(18);
    const unsigned long long submissions = 420ull * 600;
    for (unsigned long long s = 0; s < submissions; ++s) {
        const unsigned roll = (unsigned)rand() % 10000;
        const uint64_t words = roll < 9900 ? 256 + (unsigned)rand() % 1792 : roll < 9999 ? (unsigned)rand() % 262144 + 1
                                                                                         : 4u * 1024 * 1024;
        if (count == RING) reap(count);   /* server.cpp drains a full ring */
        submit(words * 4 + 32);
        const unsigned lag = 1 + (unsigned)(s / 5000 % 40);
        if (count > lag) reap(count - lag);
    }
    reap(count);
    unsigned largest = 0;
    for (unsigned i = 0; i < pool.count; ++i) largest += pool.spare[i].bytes > PS5_GPU_STAGING_LARGE;
    printf("staging: %llu submissions made=%llu reuses=%llu refused=%llu peak_in_flight=%u spares=%u spare_bytes=%llu "
           "peak_bytes=%llu above_2MiB=%u\n", submissions, made, reuses, refused, peak, pool.count,
           (unsigned long long)pool.bytes, (unsigned long long)held_peak, largest);
    assert(reuses + made == submissions);
    assert(made <= 100 && !refused);   /* was 2 kernel calls a submission; now none after the first second */
    assert(held_peak <= PS5_GPU_STAGING_BUDGET && largest <= 1);

    /* Teardown (fences_reset): every spare freed; made equals freed. */
    while (pool.count) {
        held -= pool.spare[--pool.count].bytes;
        ++freed;
    }
    assert(made == freed && !held);
    puts("GPU broker staging reuse, budget and teardown contracts passed");
    return 0;
}
