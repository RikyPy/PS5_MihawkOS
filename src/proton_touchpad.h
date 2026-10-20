/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "../wine/ps5/pw_wine_sink.h"

/* The DualSense touchpad as a laptop's trackpad.
 *
 * Pinning: pressing the pad moves the finger a few units, and Windows takes two clicks as a double click only
 * within 2 pixels of each other (SM_CXDOUBLECLK 4, win32u message.c), so desktop shortcuts, which open on a
 * double click, did not open. A click pins the pointer: motion from the press on is held back until it adds up to
 * more than PROTON_TOUCHPAD_PIN units on either axis, then all of it is sent and motion is direct again (a drag).
 * The pin outlasts the release, so the second click of a double click lands where the first one did.
 *
 * Right click: with left on the touchpad click (the default), a click made with two fingers on the pad is the right
 * button, as on a trackpad; the button is chosen at the press and released as the same one. */
enum { PROTON_TOUCHPAD_PIN = 24 };

typedef struct ProtonTouchpad {
    int active, clicked;
    unsigned finger;
    int x, y;
    unsigned button;      /* the button the current press went down as */
    int pinned, held_x, held_y;
} ProtonTouchpad;

/* Relative motion for one continuous contact; press/release is the left mouse button, or the right one when
 * two_finger_right and a second finger is on the pad at the press. A new finger, disconnect or shell
 * interception never jumps the pointer. */
static inline unsigned proton_touchpad_update(ProtonTouchpad *state, int connected,
    int touching, unsigned finger, int x, int y, int clicked, int two_finger_right, PwWineInput events[2])
{
    unsigned count = 0;
    touching = connected && touching;
    clicked = connected && clicked;
    if (touching && state->active && finger == state->finger && (x != state->x || y != state->y)) {
        int dx = x - state->x, dy = y - state->y;
        if (state->pinned) {
            state->held_x += dx;
            state->held_y += dy;
            dx = dy = 0;
            if (state->held_x > PROTON_TOUCHPAD_PIN || state->held_x < -PROTON_TOUCHPAD_PIN ||
                state->held_y > PROTON_TOUCHPAD_PIN || state->held_y < -PROTON_TOUCHPAD_PIN) {
                dx = state->held_x;
                dy = state->held_y;
                state->pinned = 0;
            }
        }
        if (dx || dy) events[count++] = (PwWineInput){PW_WINE_INPUT_MOUSE_MOVE, 0, dx, dy, 0};
    }
    if (clicked != state->clicked) {
        if (clicked) {
            state->button = two_finger_right ? 1 : 0;
            state->pinned = 1;
            state->held_x = state->held_y = 0;
        }
        events[count++] = (PwWineInput){PW_WINE_INPUT_MOUSE_BUTTON, state->button, 0, 0, (unsigned)clicked};
    }
    if (!connected) state->pinned = 0;
    state->active = touching;
    state->clicked = clicked;
    state->finger = finger;
    state->x = x;
    state->y = y;
    return count;
}
