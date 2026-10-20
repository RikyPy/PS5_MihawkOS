/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_FIRMWARE_H
#define PW_FIRMWARE_H
/*
 * The firmware that is INSTALLED on the console, as a version word (BCD: 0x10010000 is 10.01, 0x13400002 is 13.40).
 *
 * sceKernelGetSystemSwVersion is not that: a homebrew enabler spoofs it. The same release gates of 2026-10-20 logged
 * 0x12090001 on the PS5 Pro (installed 10.01) and 0x13590001 on the standard PS5 (installed 13.40), so a decision keyed on
 * it (the memory guard's limit, whether helpers load Wine's modules themselves) treated the Pro as firmware 12 and ran it
 * with the wrong limit and the wrong loader for the whole of alpha-2. kern.sdk_version is what the payload SDK (its crt and
 * the access daemon, which logs "firmware=10010000" on the Pro and "13400002" on the standard PS5) takes the installed
 * firmware from, and it is not spoofed.
 *
 * pw_firmware_choose is the portable rule; native/pw_firmware_ps5.c reads the two figures on the console.
 */
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The installed firmware: sdk_version (kern.sdk_version) when it reads as a firmware word, else reported (what
 * sceKernelGetSystemSwVersion says), else 0. A word reads as one when its two high bytes are BCD digits of a firmware
 * between 1.00 and 30.99. */
unsigned pw_firmware_choose(unsigned sdk_version, unsigned reported);

/* The word as "13.40" (its two high bytes are BCD digits). 0 for no version or a buffer under 6 bytes. */
int pw_firmware_text(unsigned word, char *text, size_t size);

/* The firmware this process runs on (console only: native/pw_firmware_ps5.c). reported, when not NULL, gets what
 * sceKernelGetSystemSwVersion said, for a line that shows both. */
unsigned pw_firmware_installed(unsigned *reported);

#ifdef __cplusplus
}
#endif

#endif
