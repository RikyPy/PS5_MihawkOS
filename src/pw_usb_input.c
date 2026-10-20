/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pw_usb_input.h"
#include <string.h>

/* Windows virtual keys and set-1 scan codes of keyboard-page usages 0x04..0x65 (USB HID Usage Tables, section 10;
 * Microsoft's "Keyboard Scan Code Specification", annex). E: the scan code is extended (E0). */
#define E 0x100
static const struct { uint8_t vk; uint16_t scan; } keys[0x66] = {
    [0x04] = {'A', 0x1e}, [0x05] = {'B', 0x30}, [0x06] = {'C', 0x2e}, [0x07] = {'D', 0x20}, [0x08] = {'E', 0x12},
    [0x09] = {'F', 0x21}, [0x0a] = {'G', 0x22}, [0x0b] = {'H', 0x23}, [0x0c] = {'I', 0x17}, [0x0d] = {'J', 0x24},
    [0x0e] = {'K', 0x25}, [0x0f] = {'L', 0x26}, [0x10] = {'M', 0x32}, [0x11] = {'N', 0x31}, [0x12] = {'O', 0x18},
    [0x13] = {'P', 0x19}, [0x14] = {'Q', 0x10}, [0x15] = {'R', 0x13}, [0x16] = {'S', 0x1f}, [0x17] = {'T', 0x14},
    [0x18] = {'U', 0x16}, [0x19] = {'V', 0x2f}, [0x1a] = {'W', 0x11}, [0x1b] = {'X', 0x2d}, [0x1c] = {'Y', 0x15},
    [0x1d] = {'Z', 0x2c},
    [0x1e] = {'1', 0x02}, [0x1f] = {'2', 0x03}, [0x20] = {'3', 0x04}, [0x21] = {'4', 0x05}, [0x22] = {'5', 0x06},
    [0x23] = {'6', 0x07}, [0x24] = {'7', 0x08}, [0x25] = {'8', 0x09}, [0x26] = {'9', 0x0a}, [0x27] = {'0', 0x0b},
    [0x28] = {0x0d, 0x1c},         /* Enter */          [0x29] = {0x1b, 0x01},  /* Escape */
    [0x2a] = {0x08, 0x0e},         /* Backspace */      [0x2b] = {0x09, 0x0f},  /* Tab */
    [0x2c] = {0x20, 0x39},         /* Space */          [0x2d] = {0xbd, 0x0c},  /* - */
    [0x2e] = {0xbb, 0x0d},         /* = */              [0x2f] = {0xdb, 0x1a},  /* [ */
    [0x30] = {0xdd, 0x1b},         /* ] */              [0x31] = {0xdc, 0x2b},  /* \ */
    [0x32] = {0xdc, 0x2b},         /* non-US # */       [0x33] = {0xba, 0x27},  /* ; */
    [0x34] = {0xde, 0x28},         /* ' */              [0x35] = {0xc0, 0x29},  /* ` */
    [0x36] = {0xbc, 0x33},         /* , */              [0x37] = {0xbe, 0x34},  /* . */
    [0x38] = {0xbf, 0x35},         /* / */              [0x39] = {0x14, 0x3a},  /* Caps Lock */
    [0x3a] = {0x70, 0x3b}, [0x3b] = {0x71, 0x3c}, [0x3c] = {0x72, 0x3d}, [0x3d] = {0x73, 0x3e},   /* F1..F4 */
    [0x3e] = {0x74, 0x3f}, [0x3f] = {0x75, 0x40}, [0x40] = {0x76, 0x41}, [0x41] = {0x77, 0x42},   /* F5..F8 */
    [0x42] = {0x78, 0x43}, [0x43] = {0x79, 0x44}, [0x44] = {0x7a, 0x57}, [0x45] = {0x7b, 0x58},   /* F9..F12 */
    [0x46] = {0x2c, E | 0x37},     /* Print Screen */   [0x47] = {0x91, 0x46},  /* Scroll Lock */
    [0x48] = {0x13, 0x45},         /* Pause */          [0x49] = {0x2d, E | 0x52},  /* Insert */
    [0x4a] = {0x24, E | 0x47},     /* Home */           [0x4b] = {0x21, E | 0x49},  /* Page Up */
    [0x4c] = {0x2e, E | 0x53},     /* Delete */         [0x4d] = {0x23, E | 0x4f},  /* End */
    [0x4e] = {0x22, E | 0x51},     /* Page Down */      [0x4f] = {0x27, E | 0x4d},  /* Right */
    [0x50] = {0x25, E | 0x4b},     /* Left */           [0x51] = {0x28, E | 0x50},  /* Down */
    [0x52] = {0x26, E | 0x48},     /* Up */             [0x53] = {0x90, E | 0x45},  /* Num Lock */
    [0x54] = {0x6f, E | 0x35},     /* keypad / */       [0x55] = {0x6a, 0x37},  /* keypad * */
    [0x56] = {0x6d, 0x4a},         /* keypad - */       [0x57] = {0x6b, 0x4e},  /* keypad + */
    [0x58] = {0x0d, E | 0x1c},     /* keypad Enter */
    [0x59] = {0x61, 0x4f}, [0x5a] = {0x62, 0x50}, [0x5b] = {0x63, 0x51}, [0x5c] = {0x64, 0x4b},   /* keypad 1..4 */
    [0x5d] = {0x65, 0x4c}, [0x5e] = {0x66, 0x4d}, [0x5f] = {0x67, 0x47}, [0x60] = {0x68, 0x48},   /* keypad 5..8 */
    [0x61] = {0x69, 0x49}, [0x62] = {0x60, 0x52},                                                 /* keypad 9, 0 */
    [0x63] = {0x6e, 0x53},         /* keypad . */       [0x64] = {0xe2, 0x56},  /* non-US \ */
    [0x65] = {0x5d, E | 0x5d},     /* Application */
};
/* Modifier bits: left Ctrl, Shift, Alt, Windows, then the right ones. */
static const struct { uint8_t vk; uint16_t scan; } modifiers[8] = {
    {0x11, 0x1d}, {0x10, 0x2a}, {0x12, 0x38}, {0x5b, E | 0x5b},
    {0x11, E | 0x1d}, {0x10, 0x36}, {0x12, E | 0x38}, {0x5c, E | 0x5c},
};
#undef E

static uint32_t code_of(uint8_t vk, uint16_t scan)
{
    return vk ? (uint32_t)vk | (uint32_t)(scan & 0xff) << 8 | (uint32_t)(scan >> 8) << 16 : 0;
}

uint32_t pw_usb_key_code(uint16_t usage)
{
    return usage < sizeof(keys) / sizeof(keys[0]) ? code_of(keys[usage].vk, keys[usage].scan) : 0;
}

uint32_t pw_usb_modifier_code(unsigned bit)
{
    return bit < 8 ? code_of(modifiers[bit].vk, modifiers[bit].scan) : 0;
}

static uint32_t u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static int32_t s32(const uint8_t *p) { int32_t v; memcpy(&v, p, 4); return v; }
static uint64_t u64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

int pw_usb_keyboard_decode(const uint8_t state[PW_USB_KEYBOARD_STATE], PwUsbKeyboard *out)
{
    int32_t count;

    memset(out, 0, sizeof(*out));
    if (!state[16]) return 0;
    count = s32(state + 20);
    if (count < 0) count = 0;
    if (count > PW_USB_KEYS) count = PW_USB_KEYS;
    out->modifiers = (uint8_t)u32(state + 28);
    for (int32_t i = 0; i < count; i++) memcpy(&out->keys[i], state + 32 + 2 * i, 2);
    return 1;
}

int pw_usb_mouse_decode(const uint8_t *records, int count, PwUsbMouse *mouse)
{
    int connected = 0;

    for (int i = 0; i < count; i++) {
        const uint8_t *r = records + (size_t)i * PW_USB_MOUSE_RECORD;
        if (!r[8]) continue;
        mouse->dx += s32(r + 16);
        mouse->dy += s32(r + 20);
        mouse->wheel += s32(r + 24);
        mouse->tilt += s32(r + 28);
        if (u64(r) >= mouse->newest) {
            mouse->newest = u64(r);
            mouse->buttons = u32(r + 12) & ((1u << PW_USB_MOUSE_BUTTONS) - 1);
        }
        connected++;
    }
    return connected;
}

void pw_usb_keyboard_merge(PwUsbKeyboard *merged, const PwUsbKeyboard *one)
{
    size_t slot = 0;

    merged->modifiers |= one->modifiers;
    for (size_t i = 0; i < PW_USB_KEYS; i++) {
        if (!one->keys[i]) continue;
        while (slot < PW_USB_KEYS && merged->keys[slot]) slot++;
        if (slot == PW_USB_KEYS) return;
        merged->keys[slot] = one->keys[i];
    }
}

static int holds(const PwUsbKeyboard *k, uint16_t usage)
{
    for (size_t i = 0; i < PW_USB_KEYS; i++)
        if (k->keys[i] == usage) return 1;
    return 0;
}

static size_t put(PwWineInput *out, size_t count, size_t capacity, uint32_t type, uint32_t code, int32_t x,
                  int32_t y, uint32_t down)
{
    if (count < capacity) out[count++] = (PwWineInput){type, code, x, y, down};
    return count;
}

size_t pw_usb_keyboard_events(const PwUsbKeyboard *previous, const PwUsbKeyboard *current, PwWineInput *out,
                              size_t capacity)
{
    size_t count = 0;

    if (!previous || !current || !out) return 0;
    for (size_t i = 0; i < PW_USB_KEYS; i++) {
        uint16_t usage = previous->keys[i];
        if (pw_usb_key_code(usage) && !holds(current, usage))
            count = put(out, count, capacity, PW_WINE_INPUT_KEY, pw_usb_key_code(usage), 0, 0, 0);
    }
    for (unsigned bit = 0; bit < 8; bit++)
        if ((previous->modifiers & ~current->modifiers) >> bit & 1u)
            count = put(out, count, capacity, PW_WINE_INPUT_KEY, pw_usb_modifier_code(bit), 0, 0, 0);
    for (unsigned bit = 0; bit < 8; bit++)
        if ((current->modifiers & ~previous->modifiers) >> bit & 1u)
            count = put(out, count, capacity, PW_WINE_INPUT_KEY, pw_usb_modifier_code(bit), 0, 0, 1);
    for (size_t i = 0; i < PW_USB_KEYS; i++) {
        uint16_t usage = current->keys[i];
        int earlier = 0;
        for (size_t j = 0; j < i; j++) earlier |= current->keys[j] == usage;
        if (pw_usb_key_code(usage) && !earlier && !holds(previous, usage))
            count = put(out, count, capacity, PW_WINE_INPUT_KEY, pw_usb_key_code(usage), 0, 0, 1);
    }
    return count;
}

size_t pw_usb_mouse_events(uint32_t previous_buttons, const PwUsbMouse *mouse, PwWineInput *out, size_t capacity)
{
    size_t count = 0;
    uint32_t changed;

    if (!mouse || !out) return 0;
    if (mouse->dx || mouse->dy)
        count = put(out, count, capacity, PW_WINE_INPUT_MOUSE_MOVE, 0, mouse->dx, mouse->dy, 0);
    if (mouse->wheel || mouse->tilt)
        count = put(out, count, capacity, PW_WINE_INPUT_MOUSE_WHEEL, 0, mouse->tilt * PW_USB_WHEEL_DELTA,
                    mouse->wheel * PW_USB_WHEEL_DELTA, 0);
    changed = (previous_buttons ^ mouse->buttons) & ((1u << PW_USB_MOUSE_BUTTONS) - 1);
    for (uint32_t i = 0; i < PW_USB_MOUSE_BUTTONS; i++)
        if (changed >> i & 1u)
            count = put(out, count, capacity, PW_WINE_INPUT_MOUSE_BUTTON, i, 0, 0, mouse->buttons >> i & 1u);
    return count;
}
