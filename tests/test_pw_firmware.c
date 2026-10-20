/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/pw_firmware.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    char text[16];

    /* The two consoles of 2026-10-20: the installed firmware wins over the word the enabler reports. */
    assert(pw_firmware_choose(0x10010000u, 0x12090001u) == 0x10010000u);   /* PS5 Pro, installed 10.01 */
    assert(pw_firmware_choose(0x13400002u, 0x13590001u) == 0x13400002u);   /* standard PS5, installed 13.40 */
    /* kern.sdk_version not readable, or not a firmware: what the system reports; nothing at all is 0. */
    assert(pw_firmware_choose(0, 0x13590001u) == 0x13590001u);
    assert(pw_firmware_choose(0xdeadbeefu, 0x10010000u) == 0x10010000u);
    assert(pw_firmware_choose(0x00000000u, 0) == 0);
    assert(pw_firmware_choose(0xdeadbeefu, 0xffffffffu) == 0);
    assert(pw_firmware_choose(0x31000000u, 0) == 0);

    assert(pw_firmware_text(0x10010000u, text, sizeof(text)) && !strcmp(text, "10.01"));
    assert(pw_firmware_text(0x13400002u, text, sizeof(text)) && !strcmp(text, "13.40"));
    assert(!pw_firmware_text(0, text, sizeof(text)) && !pw_firmware_text(0x10010000u, text, 5));
    puts("pw_firmware: ok");
    return 0;
}
