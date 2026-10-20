/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/pw_audio_ring.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { SAMPLES = PW_AUDIO_RING_GRAIN * 2 };

static void grain(int16_t *frames, int16_t value)
{
    for (int i = 0; i < SAMPLES; i++) frames[i] = value;
}

static int uniform(const int16_t *frames, int16_t value)
{
    for (int i = 0; i < SAMPLES; i++) if (frames[i] != value) return 0;
    return 1;
}

/* A clock that moves a millisecond on each wait, and a wait that can play the reader. */
static uint64_t fake_ms;
static unsigned waits;
static PwAudioRing *reader_ring;
static uint64_t fake_now(void) { return fake_ms; }
static void fake_wait(void) { fake_ms++; waits++; }
static void reading_wait(void)
{
    int16_t out[SAMPLES];
    fake_ms++;
    waits++;
    if (waits == 3) assert(pw_audio_ring_read(reader_ring, out) == 1);
}

static void test_init_and_check(void)
{
    static PwAudioRing ring;
    assert(pw_audio_ring_bytes() == sizeof(PwAudioRing));
    assert(pw_audio_ring_init(NULL, sizeof(ring)) == PW_ERR_PRECONDITION);
    assert(pw_audio_ring_init(&ring, sizeof(ring) - 1) == PW_ERR_PRECONDITION);
    assert(pw_audio_ring_check(&ring, sizeof(ring)) == PW_ERR_MALFORMED);  /* never initialized */
    assert(pw_audio_ring_init(&ring, sizeof(ring)) == PW_OK);
    assert(pw_audio_ring_check(&ring, sizeof(ring)) == PW_OK);
    assert(pw_audio_ring_check(&ring, sizeof(ring) - 1) == PW_ERR_PRECONDITION);
    ring.version = 2;
    assert(pw_audio_ring_check(&ring, sizeof(ring)) == PW_ERR_MALFORMED);
    assert(pw_audio_ring_init(&ring, sizeof(ring)) == PW_OK);
    ring.grains = 4;
    assert(pw_audio_ring_check(&ring, sizeof(ring)) == PW_ERR_MALFORMED);
}

static void test_order_and_empty(void)
{
    static PwAudioRing ring;
    int16_t in[SAMPLES], out[SAMPLES];
    assert(pw_audio_ring_init(&ring, sizeof(ring)) == PW_OK);
    assert(pw_audio_ring_read(&ring, out) == 0);
    for (int16_t v = 1; v <= 3; v++) {
        grain(in, v);
        assert(pw_audio_ring_write(&ring, in, 0, NULL, NULL) == PW_OK);
    }
    assert(pw_audio_ring_fill(&ring) == 3);
    for (int16_t v = 1; v <= 3; v++) {
        assert(pw_audio_ring_read(&ring, out) == 1);
        assert(uniform(out, v));
    }
    assert(pw_audio_ring_read(&ring, out) == 0);
    assert(pw_audio_ring_fill(&ring) == 0 && pw_audio_ring_dropped(&ring) == 0);
    assert(pw_audio_ring_write(NULL, in, 0, NULL, NULL) == PW_ERR_PRECONDITION);
    assert(pw_audio_ring_write(&ring, NULL, 0, NULL, NULL) == PW_ERR_PRECONDITION);
    assert(pw_audio_ring_read(NULL, out) == 0 && pw_audio_ring_read(&ring, NULL) == 0);
}

static void test_full_drops_after_patience(void)
{
    static PwAudioRing ring;
    int16_t in[SAMPLES], out[SAMPLES];
    assert(pw_audio_ring_init(&ring, sizeof(ring)) == PW_OK);
    for (int i = 0; i < PW_AUDIO_RING_GRAINS; i++) {
        grain(in, (int16_t)(10 + i));
        assert(pw_audio_ring_write(&ring, in, 5, fake_now, fake_wait) == PW_OK);
    }
    assert(pw_audio_ring_fill(&ring) == PW_AUDIO_RING_GRAINS);
    fake_ms = 100;
    waits = 0;
    grain(in, 99);
    assert(pw_audio_ring_write(&ring, in, 5, fake_now, fake_wait) == PW_ERR_LIMIT);
    assert(waits == 5 && pw_audio_ring_dropped(&ring) == 1);
    /* Without a clock, a full ring drops at once. */
    assert(pw_audio_ring_write(&ring, in, 1000, NULL, NULL) == PW_ERR_LIMIT && pw_audio_ring_dropped(&ring) == 2);
    /* The dropped grains never reached the ring: the reader sees the eight that did, in order. */
    for (int i = 0; i < PW_AUDIO_RING_GRAINS; i++) {
        assert(pw_audio_ring_read(&ring, out) == 1);
        assert(uniform(out, (int16_t)(10 + i)));
    }
    assert(pw_audio_ring_read(&ring, out) == 0);
}

static void test_room_made_while_waiting(void)
{
    static PwAudioRing ring;
    int16_t in[SAMPLES], out[SAMPLES];
    assert(pw_audio_ring_init(&ring, sizeof(ring)) == PW_OK);
    for (int i = 0; i < PW_AUDIO_RING_GRAINS; i++) {
        grain(in, (int16_t)i);
        assert(pw_audio_ring_write(&ring, in, 0, NULL, NULL) == PW_OK);
    }
    reader_ring = &ring;
    fake_ms = 0;
    waits = 0;
    grain(in, 42);
    assert(pw_audio_ring_write(&ring, in, 50, fake_now, reading_wait) == PW_OK);
    assert(waits == 3 && pw_audio_ring_dropped(&ring) == 0 && pw_audio_ring_fill(&ring) == PW_AUDIO_RING_GRAINS);
    for (int i = 1; i < PW_AUDIO_RING_GRAINS; i++) {
        assert(pw_audio_ring_read(&ring, out) == 1);
        assert(uniform(out, (int16_t)i));
    }
    assert(pw_audio_ring_read(&ring, out) == 1 && uniform(out, 42));
}

static void test_counters_wrap(void)
{
    static PwAudioRing ring;
    int16_t in[SAMPLES], out[SAMPLES];
    assert(pw_audio_ring_init(&ring, sizeof(ring)) == PW_OK);
    ring.written = ring.read = 0xfffffffdu;
    for (int16_t v = 0; v < 6; v++) {
        grain(in, v);
        assert(pw_audio_ring_write(&ring, in, 0, NULL, NULL) == PW_OK);
    }
    assert(pw_audio_ring_fill(&ring) == 6 && ring.written == 3);
    for (int16_t v = 0; v < 6; v++) {
        assert(pw_audio_ring_read(&ring, out) == 1);
        assert(uniform(out, v));
    }
    assert(pw_audio_ring_read(&ring, out) == 0);
}

static void test_corrupt_writer(void)
{
    static PwAudioRing ring;
    int16_t out[SAMPLES];
    assert(pw_audio_ring_init(&ring, sizeof(ring)) == PW_OK);
    ring.written = 100;   /* more than the ring holds */
    assert(pw_audio_ring_read(&ring, out) == 0);
    assert(ring.read == 100 && pw_audio_ring_fill(&ring) == 0);
}

/* Two threads, as the helper and the title: every grain arrives, in order, with a reader slower than the writer. */
static PwAudioRing shared;
enum { GRAINS_SENT = 20000 };
static uint64_t real_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}
static void real_wait(void) { usleep(100); }
static void *writer(void *unused)
{
    (void)unused;
    int16_t in[SAMPLES];
    for (int i = 0; i < GRAINS_SENT; i++) {
        grain(in, (int16_t)(i & 0x7fff));
        assert(pw_audio_ring_write(&shared, in, 5000, real_ms, real_wait) == PW_OK);
    }
    return NULL;
}

static void test_two_threads(void)
{
    assert(pw_audio_ring_init(&shared, sizeof(shared)) == PW_OK);
    pthread_t thread;
    assert(pthread_create(&thread, NULL, writer, NULL) == 0);
    int16_t out[SAMPLES];
    int next = 0;
    while (next < GRAINS_SENT) {
        if (pw_audio_ring_read(&shared, out) == 1) {
            assert(uniform(out, (int16_t)(next & 0x7fff)));
            next++;
            if (next % 64 == 0) usleep(50);
        }
    }
    assert(pthread_join(thread, NULL) == 0);
    assert(pw_audio_ring_dropped(&shared) == 0 && pw_audio_ring_fill(&shared) == 0);
}

int main(void)
{
    test_init_and_check();
    test_order_and_empty();
    test_full_drops_after_patience();
    test_room_made_while_waiting();
    test_counters_wrap();
    test_corrupt_writer();
    test_two_threads();
    puts("pw_audio_ring: ok");
    return 0;
}
