/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_DISPLAY_RATE_H
#define PW_DISPLAY_RATE_H
#include <stdint.h>
/* The rate the broker's display keeps, and the one Wine may offer (graphics patches 0023 and 0029,
 * native/proton_wine.cpp). Host test: tests/test_pw_display_rate.c.
 *
 * Kept: 119.88 Hz only when the title asked for it (PROTON_120HZ, on by default, src/pw_speed_defaults.h), VideoOut
 * said the output supports high frame rate, configuring it succeeded, and the vblanks measured after came within
 * 12.5 ms. Otherwise 59.94 Hz, restored.
 *
 * Offered: WINE_PS5_REFRESH=120 makes Wine's PS5 driver publish 120 Hz modes (patches 0924, 1188). The title sets it
 * only when the broker's display kept 119.88 Hz, read from the opened display before any helper starts (patch 0029).
 * On a TV that cannot take it, the variable stays unset and a program sees 60 Hz modes alone (36 modes, DXGI 12, all at
 * 60: modes-p3-2026-10-17-03), so no game picks a mode the display does not show. */
static inline int pw_display_kept_120(int asked, int supported, int configured, uint64_t period_ns)
{
    return asked && supported > 0 && configured == 0 && period_ns && period_ns <= 12500000u;
}
/* WINE_PS5_REFRESH's value for a display at hz, or NULL to leave it unset (Wine's 60 Hz). */
static inline const char *pw_display_refresh_env(unsigned hz)
{
    return hz == 120 ? "120" : 0;
}
#endif
