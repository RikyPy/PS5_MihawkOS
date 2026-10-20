/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_AUDIO_RING_H
#define PW_AUDIO_RING_H
#include "../include/prospero_win.h"

/* Sound from a helper process to the title (docs/GRAPHICS_PROCESS_BROKER.md, "Audio for helpers"): a ring of
 * grains in memory both processes map, with one writer (the helper's audio driver, through pw_wine_audio_output)
 * and one reader (the title's thread that owns the AudioOut port). A grain is the console's: PW_AUDIO_RING_GRAIN
 * stereo 16-bit frames at 48 kHz, as wine/ps5/pw_wine_sink.h's PW_WINE_AUDIO_GRAIN.
 *
 * The two counters only grow, and their difference is the fill. The writer waits a bounded time while the ring is
 * full, and counts a drop when it gives up, so a reader that stalls never holds a game's audio thread. The reader
 * takes what is there and never waits. A counter the other side corrupted can make the reader skip, never read
 * outside the ring.
 *
 * Portable: no allocation and no locking. The counters are C11 atomics, which hold across processes on memory both
 * map. */

enum {
    PW_AUDIO_RING_MAGIC = 0x50574152u,  /* "PWAR" */
    PW_AUDIO_RING_VERSION = 1,
    PW_AUDIO_RING_GRAIN = 256,          /* frames in a grain, 5.3 ms */
    PW_AUDIO_RING_GRAINS = 8,           /* grains in the ring, 43 ms */
};

typedef struct PwAudioRing {
    uint32_t magic, version, grain_frames, grains;
    uint32_t written;   /* grains the writer has published */
    uint32_t read;      /* grains the reader has taken */
    uint32_t dropped;   /* grains the writer gave up on */
    uint32_t reserved;
    int16_t frames[PW_AUDIO_RING_GRAINS][PW_AUDIO_RING_GRAIN * 2];
} PwAudioRing;

/* The bytes a ring takes: the shared object the title makes for each helper. */
size_t pw_audio_ring_bytes(void);
/* Title side: an empty ring in memory of at least pw_audio_ring_bytes(). */
int pw_audio_ring_init(void *memory, size_t bytes);
/* Helper side: PW_OK when memory holds a ring of this version and shape, PW_ERR_MALFORMED otherwise. */
int pw_audio_ring_check(const void *memory, size_t bytes);
/* Writer: one grain into the next slot. While the ring is full, wait() is called (it should sleep a millisecond or
 * so) until the reader makes room, or until patience_ms have passed by now_ms(); then the grain is dropped and
 * counted. Without now_ms or wait, a full ring drops at once. PW_OK, or PW_ERR_LIMIT when dropped. */
int pw_audio_ring_write(PwAudioRing *ring, const int16_t *frames, unsigned patience_ms, uint64_t (*now_ms)(void),
                        void (*wait)(void));
/* Reader: the oldest grain into out. 1 when there was one, 0 when the ring was empty. */
int pw_audio_ring_read(PwAudioRing *ring, int16_t *out);
/* The grains waiting, and the grains dropped so far. */
uint32_t pw_audio_ring_fill(const PwAudioRing *ring);
uint32_t pw_audio_ring_dropped(const PwAudioRing *ring);

#endif
