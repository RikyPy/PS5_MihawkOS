/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pw_firmware.h"
#include <stdio.h>

static int bcd(unsigned byte)
{
    return (byte >> 4) <= 9 && (byte & 15) <= 9;
}

static int plausible(unsigned word)
{
    const unsigned major = (word >> 24) & 0xffu, minor = (word >> 16) & 0xffu;
    return bcd(major) && bcd(minor) && major >= 0x01 && major <= 0x30;
}

unsigned pw_firmware_choose(unsigned sdk_version, unsigned reported)
{
    if (plausible(sdk_version)) return sdk_version;
    return plausible(reported) ? reported : 0;
}

int pw_firmware_text(unsigned word, char *text, size_t size)
{
    if (!text || size < 6 || !word) return 0;
    return snprintf(text, size, "%02x.%02x", (word >> 24) & 0xffu, (word >> 16) & 0xffu) > 0;
}
