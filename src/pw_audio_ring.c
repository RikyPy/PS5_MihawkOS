/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pw_audio_ring.h"
#include <string.h>

size_t pw_audio_ring_bytes(void) { return sizeof(PwAudioRing); }

int pw_audio_ring_init(void *memory, size_t bytes)
{
    if (!memory || bytes < sizeof(PwAudioRing)) return PW_ERR_PRECONDITION;
    PwAudioRing *ring = memory;
    memset(ring, 0, sizeof(*ring));
    ring->version = PW_AUDIO_RING_VERSION;
    ring->grain_frames = PW_AUDIO_RING_GRAIN;
    ring->grains = PW_AUDIO_RING_GRAINS;
    __atomic_store_n(&ring->magic, PW_AUDIO_RING_MAGIC, __ATOMIC_RELEASE);
    return PW_OK;
}

int pw_audio_ring_check(const void *memory, size_t bytes)
{
    const PwAudioRing *ring = memory;
    if (!memory || bytes < sizeof(PwAudioRing)) return PW_ERR_PRECONDITION;
    if (__atomic_load_n(&ring->magic, __ATOMIC_ACQUIRE) != PW_AUDIO_RING_MAGIC ||
        ring->version != PW_AUDIO_RING_VERSION || ring->grain_frames != PW_AUDIO_RING_GRAIN ||
        ring->grains != PW_AUDIO_RING_GRAINS)
        return PW_ERR_MALFORMED;
    return PW_OK;
}

uint32_t pw_audio_ring_fill(const PwAudioRing *ring)
{
    return __atomic_load_n(&ring->written, __ATOMIC_ACQUIRE) - __atomic_load_n(&ring->read, __ATOMIC_ACQUIRE);
}

uint32_t pw_audio_ring_dropped(const PwAudioRing *ring)
{
    return __atomic_load_n(&ring->dropped, __ATOMIC_RELAXED);
}

int pw_audio_ring_write(PwAudioRing *ring, const int16_t *frames, unsigned patience_ms, uint64_t (*now_ms)(void),
                        void (*wait)(void))
{
    if (!ring || !frames) return PW_ERR_PRECONDITION;
    /* Only the writer moves written; the reader's count is taken with acquire, so a slot is overwritten only after
     * the reader has copied it out. */
    const uint32_t written = __atomic_load_n(&ring->written, __ATOMIC_RELAXED);
    if (written - __atomic_load_n(&ring->read, __ATOMIC_ACQUIRE) >= PW_AUDIO_RING_GRAINS) {
        const uint64_t deadline = now_ms && wait ? now_ms() + patience_ms : 0;
        do {
            if (!now_ms || !wait || now_ms() >= deadline) {
                __atomic_add_fetch(&ring->dropped, 1, __ATOMIC_RELAXED);
                return PW_ERR_LIMIT;
            }
            wait();
        } while (written - __atomic_load_n(&ring->read, __ATOMIC_ACQUIRE) >= PW_AUDIO_RING_GRAINS);
    }
    memcpy(ring->frames[written % PW_AUDIO_RING_GRAINS], frames, sizeof(ring->frames[0]));
    __atomic_store_n(&ring->written, written + 1, __ATOMIC_RELEASE);
    return PW_OK;
}

int pw_audio_ring_read(PwAudioRing *ring, int16_t *out)
{
    if (!ring || !out) return 0;
    const uint32_t read = __atomic_load_n(&ring->read, __ATOMIC_RELAXED);
    const uint32_t written = __atomic_load_n(&ring->written, __ATOMIC_ACQUIRE);
    if (written == read) return 0;
    if (written - read > PW_AUDIO_RING_GRAINS) {
        /* More than the ring holds: not a writer that follows the rules. Start again from its count. */
        __atomic_store_n(&ring->read, written, __ATOMIC_RELEASE);
        return 0;
    }
    memcpy(out, ring->frames[read % PW_AUDIO_RING_GRAINS], sizeof(ring->frames[0]));
    __atomic_store_n(&ring->read, read + 1, __ATOMIC_RELEASE);
    return 1;
}
