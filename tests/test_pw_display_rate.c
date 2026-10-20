/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The display's kept rate and what Wine may offer (src/pw_display_rate.h): 120 Hz modes only when the display kept
 * 119.88 Hz; every fallback leaves Wine at 60 Hz alone. Periods as the console measured them: 8,341 us at 119.88 Hz
 * (2026-10-17), 16,683 us at 59.94 Hz. */
#include "../src/pw_display_rate.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    /* Kept: asked, supported, configured, vblanks at 8.34 ms. */
    assert(pw_display_kept_120(1, 1, 0, 8341000));
    assert(!strcmp(pw_display_refresh_env(120), "120"));
    /* Fallbacks, each to 59.94 Hz and no 120 Hz mode: not asked; the TV does not support it; configuring failed;
     * configured but the vblanks still at 16.7 ms; no measurement. */
    assert(!pw_display_kept_120(0, 1, 0, 8341000));
    assert(!pw_display_kept_120(1, 0, -1, 0));
    assert(!pw_display_kept_120(1, 1, -1, 8341000));
    assert(!pw_display_kept_120(1, 1, 0, 16683000));
    assert(!pw_display_kept_120(1, 1, 0, 0));
    assert(pw_display_refresh_env(60) == NULL && pw_display_refresh_env(0) == NULL);
    /* The 12.5 ms line itself: kept at it, not past it. */
    assert(pw_display_kept_120(1, 1, 0, 12500000) && !pw_display_kept_120(1, 1, 0, 12500001));
    puts("test_pw_display_rate: ok");
    return 0;
}
