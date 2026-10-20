/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_USB_INPUT_PS5_H
#define PW_USB_INPUT_PS5_H
/* A USB keyboard and mouse on the console, read for the title's input (src/pw_usb_input.h turns what they report into
 * Windows input).
 *
 * The system's libSceKeyboard and libSceMouse are loaded as its modules (sceSysmoduleLoadModule, ids 0x0106 and
 * 0x00a9): a title may not load them by path (0x80020063, measured by prospero-win), and the payload SDK has no
 * libSceMouse stub, so their functions are looked up by name (sceKernelDlsym). They are opened for the user the pad
 * is opened for. One mouse handle takes every mouse (the open parameter's first byte 1, "merged"); one opened
 * without it is tried next. A device that is not there, or is plugged in later, is looked for again every
 * PW_USB_INPUT_RETRY_TICKS; a failure is logged once.
 *
 * Observability: each open is logged with the module handle and the init and open results; the first
 * PW_USB_INPUT_DUMPS keyboard states and mouse records that carry data are logged in hex, so the layouts in
 * src/pw_usb_input.h are checked by the first run that touches a key. */
#include "../src/pw_usb_input.h"
#include <stdint.h>

enum { PW_USB_INPUT_RETRY_TICKS = 360, PW_USB_INPUT_DUMPS = 3, PW_USB_INPUT_MOUSE_READ = 64 };

typedef struct PwUsbInputPs5 {
    int32_t user;
    int32_t keyboard, mouse;                         /* handles, -1 when not open */
    int keyboard_logged, mouse_logged;               /* a failed open was logged */
    int (*keyboard_read_state)(int32_t handle, void *state);
    int (*mouse_read)(int32_t handle, void *records, int32_t count);
    PwUsbKeyboard keys;
    uint32_t buttons;
    uint64_t mouse_time;
    unsigned keyboard_dumps, mouse_dumps;
    uint64_t keys_sent, mouse_sent;                  /* events, for the title's log */
    void (*log)(const char *line);
} PwUsbInputPs5;

#ifdef __cplusplus
extern "C" {
#endif
/* Loads both modules; call once, early in the title. */
void pw_usb_input_ps5_preload(void (*log)(const char *line));
/* Opens whichever devices the system has for user; the state is ready either way. */
void pw_usb_input_ps5_open(PwUsbInputPs5 *usb, int32_t user, void (*log)(const char *line));
/* One poll: the events since the last one into out (at most capacity), and a retry of a closed device every
 * PW_USB_INPUT_RETRY_TICKS ticks. Returns how many events were written. */
unsigned pw_usb_input_ps5_poll(PwUsbInputPs5 *usb, unsigned tick, PwWineInput *out, unsigned capacity);
#ifdef __cplusplus
}
#endif
#endif
