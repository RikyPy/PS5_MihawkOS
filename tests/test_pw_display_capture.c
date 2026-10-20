/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The broker's captures off the present path (src/pw_display_capture.h).
 * - Every picture handed over is written once drained, more than the queue holds included, and its pixels are
 *   display.cpp's old capture loop's (R, G, B from each tiled B8G8R8A8 word).
 * - No PATH.part is left, and a failed write is counted and logged. */
#include "../src/pw_display_capture.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static uint32_t pattern(unsigned x, unsigned y, unsigned seed) { return 0xff000000u | (x * 7919u + y * 104729u + seed * 31u) % 0xffffffu; }

static uint8_t *tiled_frame(unsigned seed)
{
    uint8_t *frame = aligned_alloc(64, PW_DISPLAY_TILED_BYTES);
    assert(frame);
    memset(frame, 0, PW_DISPLAY_TILED_BYTES);
    for (unsigned y = 0; y < PW_DISPLAY_HEIGHT; ++y)
        for (unsigned x = 0; x < PW_DISPLAY_WIDTH; ++x) {
            const uint32_t p = pattern(x, y, seed);
            memcpy(frame + pw_display_tiled(x, y), &p, 4);
        }
    return frame;
}

static void check_picture(const char *path, unsigned seed)
{
    FILE *f = fopen(path, "rb");
    assert(f);
    char header[32];
    const char expected[] = "P6\n1920 1080\n255\n";
    assert(fread(header, 1, strlen(expected), f) == strlen(expected) && !memcmp(header, expected, strlen(expected)));
    static uint8_t row[PW_DISPLAY_WIDTH * 3];
    for (unsigned y = 0; y < PW_DISPLAY_HEIGHT; ++y) {
        assert(fread(row, 1, sizeof(row), f) == sizeof(row));
        for (unsigned x = 0; x < PW_DISPLAY_WIDTH; ++x) {
            const uint32_t p = pattern(x, y, seed);
            assert(row[x * 3] == (uint8_t)(p >> 16) && row[x * 3 + 1] == (uint8_t)(p >> 8) && row[x * 3 + 2] == (uint8_t)p);
        }
    }
    assert(fgetc(f) == EOF);
    fclose(f);
    char part[300];
    struct stat status;
    snprintf(part, sizeof(part), "%s.part", path);
    assert(stat(part, &status) != 0);
}

int main(void)
{
    char folder[] = "/tmp/pw-display-capture-XXXXXX";
    assert(mkdtemp(folder));

    /* Five pictures through a queue of two: the presents that find it full wait, none is lost. */
    PwDisplayCaptures captures = PW_DISPLAY_CAPTURES_INIT;
    FILE *log = tmpfile();
    assert(log);
    enum { PICTURES = 5 };
    char paths[PICTURES][256];
    for (unsigned i = 0; i < PICTURES; ++i) {
        uint8_t *picture = tiled_frame(i + 2);
        snprintf(paths[i], sizeof(paths[i]), "%s/frame-%02u.ppm", folder, i);
        assert(pw_display_capture_take(&captures, picture, paths[i], 100 + i, 1000, log) == 0);
    }
    pw_display_capture_drain(&captures);
    assert(captures.written == PICTURES && captures.failed == 0 && captures.count == 0 && !captures.started);
    for (unsigned i = 0; i < PICTURES; ++i) check_picture(paths[i], i + 2);

    /* A write that cannot happen (no such folder): counted, logged with its errno, nothing left behind. The thread
     * starts again after a drain. */
    char missing[300];
    snprintf(missing, sizeof(missing), "%s/no-such-folder/frame.ppm", folder);
    uint8_t *picture = malloc(PW_DISPLAY_TILED_BYTES);
    assert(picture);
    memset(picture, 0, PW_DISPLAY_TILED_BYTES);
    assert(pw_display_capture_take(&captures, picture, missing, 200, 0, log) == 0);
    pw_display_capture_drain(&captures);
    assert(captures.written == PICTURES && captures.failed == 1);

    /* A path that does not fit: refused, the frame stays the caller's. */
    char long_path[400];
    memset(long_path, 'a', sizeof(long_path) - 1);
    long_path[sizeof(long_path) - 1] = 0;
    picture = malloc(64);
    assert(picture && pw_display_capture_take(&captures, picture, long_path, 0, 0, log) == EINVAL);
    free(picture);
    pw_display_capture_drain(&captures);   /* never started: returns */

    /* One line per picture, the failed one with a non-zero status. */
    rewind(log);
    char line[512];
    unsigned lines = 0, failed_lines = 0;
    while (fgets(line, sizeof(line), log)) {
        assert(!strncmp(line, "gpu-display capture flip=", 25));
        ++lines;
        if (!strstr(line, " status=0 ")) ++failed_lines;
    }
    assert(lines == PICTURES + 1 && failed_lines == 1);
    fclose(log);

    for (unsigned i = 0; i < PICTURES; ++i) remove(paths[i]);
    rmdir(folder);
    puts("test_pw_display_capture: ok");
    return 0;
}
