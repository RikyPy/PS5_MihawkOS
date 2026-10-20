/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The broker's line copy into the display's tiled buffer (src/pw_display_copy.h) against display.cpp's pixel loop,
 * pixel for pixel, in both channel orders, from a source with a wider pitch and no 16-byte alignment. */
#include "../src/pw_display_copy.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { TILED_BYTES = PW_DISPLAY_BLOCKS * 9 << 16 };

/* display.cpp's loop for an unscaled frame: each pixel to its tiled offset, the channels swapped unless bgra. */
static void pixel_loop(uint8_t *buffer, const uint8_t *pixels, size_t pitch, int bgra)
{
    for (unsigned y = 0; y < PW_DISPLAY_HEIGHT; ++y)
        for (unsigned x = 0; x < PW_DISPLAY_WIDTH; ++x) {
            uint32_t rgba;
            memcpy(&rgba, pixels + (size_t)y * pitch + (size_t)x * 4, 4);
            const uint32_t out = bgra ? rgba : (rgba & 0xff00ff00u) | ((rgba & 0xff) << 16) | ((rgba >> 16) & 0xff);
            memcpy(buffer + pw_display_tiled(x, y), &out, 4);
        }
}

int main(void)
{
    /* The layout this relies on: each 4x4 block at a multiple of 4 is one aligned line, its rows in order. */
    for (unsigned y = 0; y < PW_DISPLAY_HEIGHT; y += 4)
        for (unsigned x = 0; x < PW_DISPLAY_WIDTH; x += 4) {
            assert(pw_display_tiled(x, y) % 64 == 0);
            for (unsigned j = 0; j < 16; ++j) assert(pw_display_tiled(x + (j & 3), y + (j >> 2)) == pw_display_tiled(x, y) + 4 * j);
        }
    const size_t pitch = PW_DISPLAY_WIDTH * 4 + 196;
    uint8_t *source = malloc(pitch * PW_DISPLAY_HEIGHT + 4), *pixels = source + 4;
    uint8_t *expected = aligned_alloc(65536, TILED_BYTES), *actual = aligned_alloc(65536, TILED_BYTES);
    assert(source && expected && actual);
    for (size_t i = 0; i < pitch * PW_DISPLAY_HEIGHT; ++i) pixels[i] = (uint8_t)(i * 2654435761u >> 13);
    for (int bgra = 0; bgra <= 1; ++bgra) {
        memset(expected, 0x5a, TILED_BYTES);
        memset(actual, 0x5a, TILED_BYTES);
        pixel_loop(expected, pixels, pitch, bgra);
        pw_display_copy_lines(actual, pixels, pitch, bgra);
        /* Every byte, the rows below the frame in the last tile row untouched by both. */
        assert(!memcmp(expected, actual, TILED_BYTES));
    }
    free(source);
    free(expected);
    free(actual);
    puts("test_pw_display_copy: ok");
    return 0;
}
