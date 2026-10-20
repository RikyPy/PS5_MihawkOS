/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A timed script of pad input for a game run that nobody holds a controller for: buttons held for a
 * while, stick and trigger positions, frame captures and a stop request, at times counted from the
 * start of the run or from the game's first presented frame. The title reads the script once,
 * applies its state over the real pad's every tick (a person can still press buttons), and fires its
 * events as their times pass. One line per step, '#' starts a comment:
 *
 *   anchor frame          times count from the first presented frame (the default: from the start)
 *   12.5 cross            press CROSS for the default 0.15 s
 *   14 down 0.6           hold the D-pad down for 0.6 s
 *   20 lx -1 0.8          the left stick fully left for 0.8 s (y is down-positive, as the pad's)
 *   21 touch 960 540 0.2  a finger on the touchpad at (960, 540) for 0.2 s (the pad's units, 1920x1080);
 *                         a finger that moves between steps without lifting moves the pointer
 *   22 capture            ask for a picture of the next presented frame
 *   90 stop               ask the game to close, as stop-game does
 *
 * Buttons: cross circle square triangle up down left right options l1 r1 l3 r3 touchpad.
 * Axes: lx ly rx ry (-1..1) and l2 r2 (0..1).
 */
#ifndef PW_PAD_SCRIPT_H
#define PW_PAD_SCRIPT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PW_PAD_SCRIPT_MAX 512
#define PW_PAD_SCRIPT_TAP_MS 150

/* The console's button numbering (ps5/src/platform.h); the title checks that they agree. */
enum {
    PW_PAD_SCRIPT_L3 = 0x000002, PW_PAD_SCRIPT_R3 = 0x000004, PW_PAD_SCRIPT_OPTIONS = 0x000008,
    PW_PAD_SCRIPT_UP = 0x000010, PW_PAD_SCRIPT_RIGHT = 0x000020, PW_PAD_SCRIPT_DOWN = 0x000040,
    PW_PAD_SCRIPT_LEFT = 0x000080, PW_PAD_SCRIPT_L1 = 0x000400, PW_PAD_SCRIPT_R1 = 0x000800,
    PW_PAD_SCRIPT_TRIANGLE = 0x001000, PW_PAD_SCRIPT_CIRCLE = 0x002000, PW_PAD_SCRIPT_CROSS = 0x004000,
    PW_PAD_SCRIPT_SQUARE = 0x008000, PW_PAD_SCRIPT_TOUCH_PAD = 0x100000,
};

enum PwPadScriptAxis { PW_PAD_SCRIPT_LX, PW_PAD_SCRIPT_LY, PW_PAD_SCRIPT_RX, PW_PAD_SCRIPT_RY,
                       PW_PAD_SCRIPT_L2, PW_PAD_SCRIPT_R2, PW_PAD_SCRIPT_AXES };

enum PwPadScriptKind { PW_PAD_SCRIPT_BUTTON, PW_PAD_SCRIPT_AXIS, PW_PAD_SCRIPT_CAPTURE, PW_PAD_SCRIPT_STOP,
                       PW_PAD_SCRIPT_TOUCH };

struct PwPadScriptStep {
    unsigned start_ms, duration_ms;
    enum PwPadScriptKind kind;
    uint32_t button;          /* PW_PAD_SCRIPT_BUTTON */
    int axis;                 /* PW_PAD_SCRIPT_AXIS */
    float value;
    float x, y;               /* PW_PAD_SCRIPT_TOUCH */
};

struct PwPadScript {
    bool from_frame;          /* "anchor frame": times count from the first presented frame */
    unsigned count;
    struct PwPadScriptStep steps[PW_PAD_SCRIPT_MAX];
};

struct PwPadScriptState {
    uint32_t buttons;                    /* held by the script now */
    bool axis_set[PW_PAD_SCRIPT_AXES];   /* the script holds this axis now */
    float axis[PW_PAD_SCRIPT_AXES];
    bool touching;                       /* a finger on the touchpad, at touch_x, touch_y */
    uint16_t touch_x, touch_y;
};

/* Parse a whole script. 0 when every line was understood; otherwise the number of the first line
 * that was not (from 1), and *out holds the steps before it. */
unsigned pw_pad_script_parse(const char *text, struct PwPadScript *out);

/* What the script holds at time now_ms. A later step overrides an earlier one for the same axis. */
void pw_pad_script_state(const struct PwPadScript *script, unsigned now_ms, struct PwPadScriptState *out);

/* The capture and stop steps whose time is in [from_ms, to_ms), in script order, at most max of them;
 * returns how many were written to out. Calling it with each tick's time as the next call's from_ms
 * fires every event once. */
unsigned pw_pad_script_events(const struct PwPadScript *script, unsigned from_ms, unsigned to_ms,
                              const struct PwPadScriptStep **out, unsigned max);

/* The raw 0..255 reading for an axis value, as the pad reports it (128 at rest for a stick). */
uint8_t pw_pad_script_raw(int axis, float value);

#ifdef __cplusplus
}
#endif

#endif
