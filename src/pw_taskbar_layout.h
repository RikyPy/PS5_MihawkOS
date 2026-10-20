/* SPDX-License-Identifier: GPL-3.0-or-later
 * Windows 11's taskbar laid out as the approved prototype lays it out (prototypes/windows11-shell/css/taskbar.css), in
 * 96 DPI pixels of a 48 px bar: every slot is a 40 x 40 button 4 px from the bar's top with 2 px margins, so slots are
 * 44 px apart; the centred group is Start, the 220 x 32 search pill, Task View, then the apps; on the right, from the
 * screen edge in, the 8 px show-desktop sliver (4 px margin), the clock (9 px padding, the text block right-aligned,
 * the notification badge 8 px after it when shown), the 80 px status button and the 24 px chevron.
 *
 * At 1920 wide with four apps and a 61 px clock (the prototype's 10/16/2026) and an 18 px badge, every rectangle equals
 * the prototype's measured box rounded to whole pixels (tests/test_pw_taskbar_layout.c). Explorer's taskbar (Wine patch
 * 1140) places its windows here and draws the prototype's own pixels in them. */
#ifndef PW_TASKBAR_LAYOUT_H
#define PW_TASKBAR_LAYOUT_H

#define PW_TASKBAR_APP_SLOTS 64

typedef struct { int left, top, right, bottom; } PwTaskbarRect;

typedef struct
{
    PwTaskbarRect start, search, taskview, apps[PW_TASKBAR_APP_SLOTS], chevron, status, clock, peek;
    unsigned apps_shown;                 /* slots that fit beside the tray; fewer than asked means an overflow */
    int text_right, time_top, date_top;  /* the clock's two 17 px lines, right-aligned at text_right */
} PwTaskbarPlace;

static inline PwTaskbarRect pw_taskbar_rect(int left, int top, int width, int height)
{
    PwTaskbarRect r = {left, top, left + width, top + height};
    return r;
}

/* width: the bar's width; apps: app slots wanted; clock_text: the wider of the time and date lines; badge: the
 * badge's width, 0 when it is hidden. */
static inline void pw_taskbar_place(PwTaskbarPlace *p, int width, unsigned apps, int clock_text, int badge)
{
    int clock_width = 2 + 9 + clock_text + (badge ? 8 + badge : 0) + 9, group, x;
    if (apps > PW_TASKBAR_APP_SLOTS) apps = PW_TASKBAR_APP_SLOTS;
    p->peek = pw_taskbar_rect(width - 8, 0, 8, 48);
    p->clock = pw_taskbar_rect(p->peek.left - 4 - 2 - clock_width, 4, clock_width, 40);
    p->status = pw_taskbar_rect(p->clock.left - 4 - 80, 4, 80, 40);
    p->chevron = pw_taskbar_rect(p->status.left - 4 - 24, 4, 24, 40);
    p->text_right = p->clock.right - 1 - 9 - (badge ? badge + 8 : 0);
    p->time_top = 4 + (40 - 34) / 2;
    p->date_top = p->time_top + 17;
    /* Centred on the bar; pushed left of the tray when it would reach it; apps dropped when even that is too narrow. */
    p->apps_shown = apps;
    for (;;)
    {
        group = 44 + 224 + 44 + 44 * (int)p->apps_shown;
        x = (width - group) / 2;
        if (x + group > p->chevron.left - 2) x = p->chevron.left - 2 - group;
        if (x >= 0 || !p->apps_shown) break;
        --p->apps_shown;
    }
    if (x < 0) x = 0;
    p->start = pw_taskbar_rect(x + 2, 4, 40, 40);
    p->search = pw_taskbar_rect(x + 44 + 2, 8, 220, 32);
    p->taskview = pw_taskbar_rect(x + 268 + 2, 4, 40, 40);
    for (unsigned i = 0; i < p->apps_shown; ++i) p->apps[i] = pw_taskbar_rect(x + 312 + 44 * (int)i + 2, 4, 40, 40);
}

#endif
