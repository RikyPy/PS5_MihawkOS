/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/pw_controls.h"
#include "../src/proton_dualsense.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    pw_controls controls, loaded;
    pw_controls_mouse mouse = {0};
    PwWineInput events[4];
    struct pad pad = {.connected = 1, .touch_count = 1, .touch_id = 7, .touch_x = 100, .touch_y = 100};
    pw_controls_defaults(&controls);
    assert(controls.button[0] == 1 && !controls.button[1] && !controls.button[2]);
    assert(!pw_controls_assign(&controls, 0, 7)); /* R1 */
    assert(!pw_controls_assign(&controls, 1, 6)); /* L1 */
    assert(!pw_controls_assign(&controls, 2, 9)); /* R2 */
    assert(pw_controls_assign(&controls, 3, 1) == EINVAL);
    assert(pw_controls_assign(&controls, 0, PW_CONTROLS_BUTTONS) == EINVAL);
    assert(!pw_controls_update(&controls, &mouse, &pad, events));
    pad.held = PAD_R1 | PAD_L1 | PAD_CROSS;
    pad.touch_x += 10;
    pad.r2 = 0.6f;
    pad.raw_axes[5] = 200;
    assert(pw_controls_update(&controls, &mouse, &pad, events) == 4);
    assert(events[0].type == PW_WINE_INPUT_MOUSE_MOVE && events[0].x == 10);
    for (unsigned i = 0; i < 3; ++i) assert(events[i+1].code == i && events[i+1].down);
    assert(!pw_controls_update(&controls, &mouse, &pad, events));
    struct pad gamepad;
    uint8_t report[64];
    pw_controls_gamepad(&controls, &pad, &gamepad);
    assert(gamepad.held == PAD_CROSS && !gamepad.r2 && !gamepad.raw_axes[5]);
    assert(gamepad.touch_x == pad.touch_x);
    proton_dualsense_report(&gamepad, 0, report);
    assert(!(report[9] & 0x0a)); /* R1 and R2 removed from HID too. */
    pad.held |= PAD_INTERCEPTED;
    assert(pw_controls_update(&controls, &mouse, &pad, events) == 3);
    for (unsigned i = 0; i < 3; ++i) assert(events[i].code == i && !events[i].down);
    pad.held = PAD_R1;
    pad.connected = 0;
    assert(!pw_controls_update(&controls, &mouse, &pad, events));
    assert(!pw_controls_assign(&controls, 1, 7));
    assert(!controls.button[0] && controls.button[1] == 7); /* conflict is unassigned */

    /* Defaults: a touchpad click with two fingers on the pad is the right button; with one, the left. */
    pw_controls defaults;
    pw_controls_mouse trackpad = {0};
    pw_controls_defaults(&defaults);
    struct pad touch = {.connected = 1, .touch_count = 2, .touch_id = 4, .touch_x = 300, .touch_y = 300};
    assert(!pw_controls_update(&defaults, &trackpad, &touch, events));
    touch.held = PAD_TOUCH_PAD;
    assert(pw_controls_update(&defaults, &trackpad, &touch, events) == 1 && events[0].code == 1 && events[0].down);
    touch.held = 0;
    assert(pw_controls_update(&defaults, &trackpad, &touch, events) == 1 && events[0].code == 1 && !events[0].down);
    touch.touch_count = 1;
    touch.held = PAD_TOUCH_PAD;
    assert(pw_controls_update(&defaults, &trackpad, &touch, events) == 1 && events[0].code == 0 && events[0].down);

    char folder[] = "/tmp/proton-controls-XXXXXX";
    assert(mkdtemp(folder));
    char path[256];
    snprintf(path, sizeof(path), "%s/controls.txt", folder);
    assert(pw_controls_load(&loaded, path) == ENOENT && loaded.button[0] == 1);
    assert(!pw_controls_save(&controls, path));
    assert(!pw_controls_load(&loaded, path) && !memcmp(&loaded, &controls, sizeof(controls)));
    controls.button[0] = 7; /* invalid duplicate must not damage the last saved file */
    assert(pw_controls_save(&controls, path) == EINVAL);
    assert(!pw_controls_load(&controls, path) && !memcmp(&loaded, &controls, sizeof(controls)));
    FILE *file = fopen(path, "w");
    assert(file);
    fputs("left cross\nright cross\nmiddle none\n", file);
    fclose(file);
    assert(pw_controls_load(&loaded, path) == EINVAL && loaded.button[0] == 1);
    file = fopen(path, "w");
    assert(file);
    fputs("left no-such-button\n", file);
    fclose(file);
    assert(pw_controls_load(&loaded, path) == EINVAL && loaded.button[0] == 1);
    unlink(path);
    rmdir(folder);
    puts("controls: persistence, conflicts, motion, three clicks, HID consumption and disconnect PASS");
}
