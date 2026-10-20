/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "proton_touchpad.h"
#include "../ps5/src/platform.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One global mapping, read by the launcher and each owner before game startup.
 * Stable button names persist atomically. A button owns at most one mouse
 * action; assigning it again removes its previous assignment. Mapped buttons
 * are removed from XInput/HID, while touch motion remains available.
 * Disconnect/shell interception releases every held mouse button.
 * File functions return errno values; failed reads leave the defaults and
 * failed saves leave the previous file. The caller owns the parent folder. */
enum { PW_CONTROLS_BUTTONS = 17, PW_CONTROLS_MOUSE_BUTTONS = 3 };
typedef struct pw_controls { unsigned button[3]; } pw_controls;
typedef struct pw_controls_mouse {
    ProtonTouchpad pointer;
    int right, middle;
} pw_controls_mouse;

const char *pw_controls_button_id(unsigned index);
const char *pw_controls_button_label(unsigned index);
void pw_controls_defaults(pw_controls *controls);
int pw_controls_assign(pw_controls *controls, unsigned mouse, unsigned button);
int pw_controls_load(pw_controls *controls, const char *path);
int pw_controls_save(const pw_controls *controls, const char *path);
unsigned pw_controls_update(const pw_controls *controls, pw_controls_mouse *state,
                            const struct pad *pad, PwWineInput events[4]);
void pw_controls_gamepad(const pw_controls *controls, const struct pad *input, struct pad *output);

#ifdef __cplusplus
}
#endif
