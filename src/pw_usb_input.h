/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_USB_INPUT_H
#define PW_USB_INPUT_H
/* A USB keyboard and mouse plugged into the PS5, as Windows input.
 *
 * The console reads them through the system's libSceKeyboard and libSceMouse (native/pw_usb_input_ps5.c); this
 * part turns what they report into the title's input events (wine/ps5/pw_wine_sink.h), which the in-process server
 * delivers as a real device's (patch 0931, scan codes and the wheel since patch 1001).
 *
 * The layouts are the ones public PS4/PS5 reimplementations agree on (shadPS4, fpPS4, OpenOrbis; prospero-win reads
 * them the same way). They are not measured on this firmware yet: the console part logs the first records that carry
 * data in hex, so a layout the firmware does not share shows in the first run.
 *
 *   keyboard state, 96 bytes: u64 timestamp; 8 bytes; u8 connected (+3); s32 key count; u32 locks;
 *                             u32 modifiers (the USB boot report's bits); u16 USB HID usages[16]; 32 bytes
 *   mouse record, 40 bytes:   u64 timestamp; u8 connected (+3); u32 buttons; s32 x, y (motion since the last
 *                             record), wheel, tilt; u8 reserve[8]
 *
 * Keys: each USB usage of the keyboard page becomes its Windows virtual key and its set-1 scan code, with the
 * extended flag where a PC keyboard sends E0 (arrows, the editing block, right Ctrl and Alt, keypad Enter and /).
 * Games that read raw input or DirectInput take the scan code, programs the virtual key. Ctrl, Shift, Alt and the
 * Windows keys are sent per side (the generic key with the side's scan code), as a PC keyboard driver does; the
 * server keeps the left and right states. The keypad sends its number keys (Num Lock on). Releases go first, so a
 * key that moved to another slot of the report is never released after its new press. */
#include "../wine/ps5/pw_wine_sink.h"
#include <stddef.h>
#include <stdint.h>

enum {
    PW_USB_KEYS = 16,                      /* usages a keyboard state carries */
    PW_USB_KEYBOARD_STATE = 96,
    PW_USB_MOUSE_RECORD = 40,
    PW_USB_MOUSE_BUTTONS = 5,              /* left, right, middle, back (X1), forward (X2) */
    PW_USB_WHEEL_DELTA = 120,              /* Windows' WHEEL_DELTA, one notch */
    /* At most every key and modifier released and pressed again in one poll. */
    PW_USB_KEYBOARD_EVENTS = 2 * (PW_USB_KEYS + 8),
    /* Motion, both wheels and every button. */
    PW_USB_MOUSE_EVENTS = 3 + PW_USB_MOUSE_BUTTONS,
};

typedef struct PwUsbKeyboard {
    uint8_t modifiers;                     /* bit 0 left Ctrl, 1 left Shift, 2 left Alt, 3 left GUI, 4..7 right */
    uint16_t keys[PW_USB_KEYS];            /* pressed usages; 0 none */
} PwUsbKeyboard;

typedef struct PwUsbMouse {
    int32_t dx, dy, wheel, tilt;           /* summed over the records read */
    uint32_t buttons;                      /* the newest record's: bit 0 left, 1 right, 2 middle, 3 X1, 4 X2 */
    uint64_t newest;                       /* newest timestamp seen */
} PwUsbMouse;

#ifdef __cplusplus
extern "C" {
#endif
/* The PwWineInput KEY code of a keyboard-page usage: virtual key | scan code << 8 | extended << 16; 0 when the
 * usage has none (no key, errors, keys a PC keyboard has not). */
uint32_t pw_usb_key_code(uint16_t usage);
/* The code of a modifier bit (0..7). */
uint32_t pw_usb_modifier_code(unsigned bit);
/* A keyboard state: 1 and *out when connected, 0 (and *out cleared) when not. */
int pw_usb_keyboard_decode(const uint8_t state[PW_USB_KEYBOARD_STATE], PwUsbKeyboard *out);
/* count mouse records added into *mouse (motion and wheels summed, the buttons of the newest); how many were
 * connected. Records older than *mouse's newest still add their motion: their order is not established. */
int pw_usb_mouse_decode(const uint8_t *records, int count, PwUsbMouse *mouse);
/* Adds one keyboard's state into merged, the state of every keyboard read: its modifiers, and its keys in merged's
 * free slots while there are any (a key held on two keyboards is pressed once, pw_usb_keyboard_events). */
void pw_usb_keyboard_merge(PwUsbKeyboard *merged, const PwUsbKeyboard *one);
/* The key events from previous to current, releases first; at most PW_USB_KEYBOARD_EVENTS. */
size_t pw_usb_keyboard_events(const PwUsbKeyboard *previous, const PwUsbKeyboard *current, PwWineInput *out,
                              size_t capacity);
/* The mouse's events after a read: its motion, its wheels (in WHEEL_DELTA units), and the changes of its buttons
 * from previous_buttons; at most PW_USB_MOUSE_EVENTS. */
size_t pw_usb_mouse_events(uint32_t previous_buttons, const PwUsbMouse *mouse, PwWineInput *out, size_t capacity);
#ifdef __cplusplus
}
#endif
#endif
