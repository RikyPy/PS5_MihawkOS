/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include "../src/proton_dualsense.h"
int main(void)
{
    struct pad p = {.connected=true, .raw_axes={128,128,128,128,0,0}};
    uint8_t r[64];
    proton_dualsense_report(&p, 17, r);
    assert(r[0] == 1 && r[7] == 17 && r[8] == 8 && r[33] == 128 && r[37] == 128);
    p.held = PAD_UP | PAD_RIGHT | PAD_CROSS | PAD_OPTIONS | PAD_TOUCH_PAD;
    p.touch_count = 1; p.touch_id = 23; p.touch_x = 1919; p.touch_y = 1079;
    p.raw_axes[4] = 200;
    proton_dualsense_report(&p, 18, r);
    assert(r[8] == 0x21 && r[9] == 0x24 && r[10] == 2 && r[5] == 200);
    assert(r[33] == 23 && (r[34] | ((r[35]&15)<<8)) == 1919);
    assert(((r[35]>>4) | (r[36]<<4)) == 1079);
    p.held = PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT;
    proton_dualsense_report(&p, 19, r);
    assert(r[8] == 8);
    return 0;
}
