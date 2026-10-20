/* SPDX-License-Identifier: GPL-3.0-or-later
 * src/pw_taskbar_layout.h against the prototype's own boxes (tools/measure-prototype-surface.mjs on
 * prototypes/windows11-shell, ?fresh&light, 1920 x 1080, taskbar at y 1032), rounded to whole pixels. */
#include <stdio.h>
#include "../src/pw_taskbar_layout.h"

static int failures;
static void same(const char *what, PwTaskbarRect r, int x, int y, int w, int h)
{
    if (r.left == x && r.top == y && r.right - r.left == w && r.bottom - r.top == h) return;
    printf("FAIL %s: %d,%d %dx%d, prototype %d,%d %dx%d\n", what, r.left, r.top, r.right - r.left, r.bottom - r.top, x, y, w, h);
    ++failures;
}

int main(void)
{
    PwTaskbarPlace p;
    /* the prototype's fresh state: four pinned apps, "10/16/2026" 61.1 px wide, the "2" badge 18 px */
    pw_taskbar_place(&p, 1920, 4, 61, 18);
    same("start", p.start, 718, 4, 40, 40);
    same("search", p.search, 762, 8, 220, 32);
    same("taskview", p.taskview, 986, 4, 40, 40);
    same("app 0", p.apps[0], 1030, 4, 40, 40);
    same("app 3", p.apps[3], 1162, 4, 40, 40);
    same("chevron", p.chevron, 1687, 4, 24, 40);   /* 1686.9 */
    same("status", p.status, 1715, 4, 80, 40);     /* 1714.9 */
    same("clock", p.clock, 1799, 4, 107, 40);      /* 1798.9, 107.1 wide */
    same("peek", p.peek, 1912, 0, 8, 48);
    if (p.text_right != 1870 || p.time_top != 7 || p.date_top != 24 || p.apps_shown != 4)
    { printf("FAIL clock text: right %d, lines at %d and %d\n", p.text_right, p.time_top, p.date_top); ++failures; }
    /* no badge: the clock narrows by the badge and its gap, the text keeps its 9 px padding */
    pw_taskbar_place(&p, 1920, 4, 61, 0);
    same("clock without a badge", p.clock, 1825, 4, 81, 40);
    if (p.text_right != 1896) { printf("FAIL text without a badge: %d\n", p.text_right); ++failures; }
    /* the browser branch's five pins (MihawkBrowser after Terminal): themes/windows11/taskbar/rest-*.json, 2026-10-17 */
    pw_taskbar_place(&p, 1920, 5, 61, 18);
    same("five: start", p.start, 696, 4, 40, 40);
    same("five: search", p.search, 740, 8, 220, 32);
    same("five: taskview", p.taskview, 964, 4, 40, 40);
    same("five: app 0", p.apps[0], 1008, 4, 40, 40);
    same("five: app 4", p.apps[4], 1184, 4, 40, 40);
    same("five: chevron", p.chevron, 1687, 4, 24, 40);
    if (p.apps_shown != 5) { printf("FAIL five: %u apps shown\n", p.apps_shown); ++failures; }

    /* too many apps for a 1280 bar: the group moves left of the tray, then drops apps rather than overlap it */
    pw_taskbar_place(&p, 1280, 40, 61, 0);
    if (p.apps_shown >= 40 || p.start.left < 0 ||
        p.apps[p.apps_shown - 1].right + 2 > p.chevron.left - 2)
    { printf("FAIL narrow bar: %u apps shown, last ends %d, chevron %d\n", p.apps_shown,
             p.apps[p.apps_shown - 1].right, p.chevron.left); ++failures; }
    printf("%s: test_pw_taskbar_layout\n", failures ? "FAIL" : "PASS");
    return failures != 0;
}
