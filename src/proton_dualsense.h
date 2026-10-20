/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>
#include <string.h>
#include "../ps5/src/platform.h"

/* DualSense USB report 0x01. Keep native axis ranges and touch coordinates. */
static inline void proton_dualsense_report(const struct pad *pad, uint8_t sequence, uint8_t report[64])
{
    uint32_t b = pad->held;
    int x = !!(b & PAD_RIGHT) - !!(b & PAD_LEFT);
    int y = !!(b & PAD_DOWN) - !!(b & PAD_UP);
    static const uint8_t hats[3][3] = {{7,0,1},{6,8,2},{5,4,3}};
    memset(report, 0, 64);
    report[0] = 1;
    memcpy(report + 1, pad->raw_axes, 6);
    report[7] = sequence;
    report[8] = hats[y+1][x+1] | (!!(b & PAD_SQUARE) << 4) | (!!(b & PAD_CROSS) << 5) |
        (!!(b & PAD_CIRCLE) << 6) | (!!(b & PAD_TRIANGLE) << 7);
    report[9] = (!!(b & PAD_L1)) | (!!(b & PAD_R1) << 1) | ((pad->raw_axes[4] > 0) << 2) |
        ((pad->raw_axes[5] > 0) << 3) | (!!(b & PAD_OPTIONS) << 5) |
        (!!(b & PAD_L3) << 6) | (!!(b & PAD_R3) << 7);
    report[10] = !!(b & PAD_TOUCH_PAD) << 1;
    report[33] = pad->touch_count ? pad->touch_id & 0x7f : 0x80;
    report[34] = pad->touch_x;
    report[35] = ((pad->touch_x >> 8) & 15) | ((pad->touch_y & 15) << 4);
    report[36] = pad->touch_y >> 4;
    report[37] = 0x80;
    report[54] = 0x08; /* USB transport */
}
