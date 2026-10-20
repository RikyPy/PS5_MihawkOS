/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/proton_touchpad.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    ProtonTouchpad state = {0};
    PwWineInput events[2];
    assert(!proton_touchpad_update(&state, 1, 1, 7, 500, 400, 0, 0, events));
    assert(proton_touchpad_update(&state, 1, 1, 7, 520, 390, 1, 0, events) == 2);
    assert(events[0].type == PW_WINE_INPUT_MOUSE_MOVE && events[0].x == 20 && events[0].y == -10);
    assert(events[1].type == PW_WINE_INPUT_MOUSE_BUTTON && events[1].code == 0 && events[1].down);
    assert(!proton_touchpad_update(&state, 1, 1, 8, 10, 10, 1, 0, events));
    assert(proton_touchpad_update(&state, 0, 1, 8, 10, 10, 1, 0, events) == 1);
    assert(!events[0].down);
    assert(!proton_touchpad_update(&state, 1, 1, 8, 1000, 900, 0, 0, events));
    assert(!proton_touchpad_update(&state, 1, 0, 8, 1000, 900, 0, 0, events));
    assert(!proton_touchpad_update(&state, 1, 1, 8, 50, 50, 0, 0, events));

    /* A double click with the finger squashing a few units each press: the pointer stays where the first click
     * was, so Windows sees a double click (two clicks within 2 pixels). */
    ProtonTouchpad pad = {0};
    assert(!proton_touchpad_update(&pad, 1, 1, 3, 900, 500, 0, 0, events));
    assert(proton_touchpad_update(&pad, 1, 1, 3, 900, 500, 1, 0, events) == 1 && events[0].code == 0 && events[0].down);
    assert(!proton_touchpad_update(&pad, 1, 1, 3, 906, 497, 1, 0, events));      /* squash while pressed: held */
    assert(proton_touchpad_update(&pad, 1, 1, 3, 903, 499, 0, 0, events) == 1 && !events[0].down);
    assert(proton_touchpad_update(&pad, 1, 1, 3, 908, 503, 1, 0, events) == 1 && events[0].down); /* no motion */
    assert(proton_touchpad_update(&pad, 1, 1, 3, 902, 501, 0, 0, events) == 1 && !events[0].down);
    /* Moving on: the pin counts from the latest press (908, 503); once the held motion passes it, all of it is
     * sent, then motion is direct. */
    assert(!proton_touchpad_update(&pad, 1, 1, 3, 930, 501, 0, 0, events));
    assert(proton_touchpad_update(&pad, 1, 1, 3, 935, 501, 0, 0, events) == 1);
    assert(events[0].type == PW_WINE_INPUT_MOUSE_MOVE && events[0].x == 27 && events[0].y == -2);
    assert(proton_touchpad_update(&pad, 1, 1, 3, 940, 501, 0, 0, events) == 1 && events[0].x == 5);
    /* A drag: press, then move past the pin while pressed. */
    assert(proton_touchpad_update(&pad, 1, 1, 3, 940, 501, 1, 0, events) == 1 && events[0].down);
    assert(!proton_touchpad_update(&pad, 1, 1, 3, 950, 501, 1, 0, events));
    assert(proton_touchpad_update(&pad, 1, 1, 3, 965, 511, 1, 0, events) == 1 && events[0].x == 25 && events[0].y == 10);
    assert(proton_touchpad_update(&pad, 1, 1, 3, 975, 511, 1, 0, events) == 1 && events[0].x == 10);
    assert(proton_touchpad_update(&pad, 1, 1, 3, 975, 511, 0, 0, events) == 1 && events[0].code == 0 && !events[0].down);

    /* Two fingers at the press: the right button, released as the right button even if one finger lifts first. */
    assert(proton_touchpad_update(&pad, 1, 1, 3, 975, 511, 1, 1, events) == 1);
    assert(events[0].type == PW_WINE_INPUT_MOUSE_BUTTON && events[0].code == 1 && events[0].down);
    assert(proton_touchpad_update(&pad, 1, 1, 3, 975, 511, 0, 0, events) == 1 && events[0].code == 1 && !events[0].down);
    assert(proton_touchpad_update(&pad, 1, 1, 3, 975, 511, 1, 0, events) == 1 && events[0].code == 0);
    /* A disconnect releases the button it went down as and ends the pin. */
    assert(proton_touchpad_update(&pad, 0, 1, 3, 975, 511, 1, 0, events) == 1 && events[0].code == 0 && !events[0].down);
    assert(!pad.pinned);
    puts("touchpad: relative motion, left press/release, new contact, disconnect, pinned double click, drag, "
         "two-finger right click passed");
}
