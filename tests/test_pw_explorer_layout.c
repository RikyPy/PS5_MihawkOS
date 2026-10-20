/* SPDX-License-Identifier: GPL-3.0-or-later
 * src/pw_explorer_layout.h against the prototype's own boxes (tools/measure-prototype-surface.mjs explorer '.window',
 * prototypes/windows11-shell ?fresh&light&open=explorer at 1920 x 1080: the window at 410,166, 1100 x 700), in client
 * coordinates (the window's less 411,167), to a hundredth of a pixel. */
#include <math.h>
#include <stdio.h>
#include "../src/pw_explorer_layout.h"

static int failures;
static void same(const char *what, PwExplorerBox b, double x, double y, double w, double h)
{
    if (fabs(b.x - x) < 0.06 && fabs(b.y - y) < 0.06 && fabs(b.w - w) < 0.06 && fabs(b.h - h) < 0.06) return;
    printf("FAIL %s: %.2f,%.2f %.2fx%.2f, prototype %.2f,%.2f %.2fx%.2f\n", what, b.x, b.y, b.w, b.h, x, y, w, h);
    ++failures;
}

int main(void)
{
    static const double commands[][2] = {{8, 83.9}, {100.9, 40}, {140.9, 40}, {180.9, 40}, {220.9, 40}, {260.9, 40},
                                         {300.9, 40}, {349.9, 81.7}, {431.6, 85.3}, {516.9, 0}, {525.9, 40}, {1006.2, 79.8}};
    static const double rows[] = {148, 180, 229, 261, 293, 325, 357, 389, 438, 470};
    static const double tiles[][2] = {{16, 44}, {243.2, 44}, {470.5, 44}, {697.8, 44}, {16, 120}, {243.2, 120}};
    PwExplorerPlace p;
    PwExplorerHome home;
    int i;
    pw_explorer_place(&p, 1098, 698, 0, 0);
    {
        /* The tabs (?fresh&light&open=explorer:documents, then + clicked): one tab, two, three, six and nine. */
        PwExplorerBox tab[PW_EXPLORER_TABS_MAX], close[PW_EXPLORER_TABS_MAX], add;
        pw_explorer_tab_strip(tab, close, &add, 1, 1098);
        same("tab", tab[0], 8, 8, 240, 40);
        same("tab close", close[0], 211, 16.5, 32, 24);
        same("tab add", add, 252, 14, 32, 28);
        pw_explorer_tab_strip(tab, close, &add, 3, 1098);
        same("third tab", tab[2], 488, 8, 240, 40);
        same("third tab's close", close[2], 691, 16.5, 32, 24);
        same("tab add after three", add, 732, 14, 32, 28);
        pw_explorer_tab_strip(tab, close, &add, 6, 1098);
        same("sixth of six tabs", tab[5], 738, 8, 146, 40);
        same("first of six tabs' close", close[0], 117, 16.5, 32, 24);
        same("tab add after six", add, 888, 14, 32, 28);
        pw_explorer_tab_strip(tab, close, &add, 9, 1098);
        same("second of nine tabs", tab[1], 105.34, 8, 97.33, 40);
        same("first of nine tabs' close", close[0], 68.34, 16.5, 32, 24);
        same("tab add after nine", add, 888, 14, 32, 28);
        pw_explorer_tab_strip(tab, close, &add, 20, 1098);
        same("tab add when the strip clips", add, 888, 14, 32, 28);
    }
    for (i = 0; i < 3; ++i) same("caption button", p.caption[i], 960 + 46 * i, 0, 46, 48);
    for (i = 0; i < 4; ++i) same("navigation button", p.navigation[i], 8 + 36 * i, 56, 32, 32);
    same("address", p.address, 156, 56, 642, 32);
    same("search", p.search, 806, 56, 280, 32);
    for (i = 0; i < PW_EXPLORER_COMMANDS; ++i) same("command", p.command[i], commands[i][0], 103.5, commands[i][1], 32);
    same("separator 1", p.separator[0], 95.9, 107.5, 1, 24);
    same("separator 2", p.separator[1], 344.9, 107.5, 1, 24);
    same("separator 3", p.separator[2], 520.9, 107.5, 1, 24);
    for (i = 0; i < PW_EXPLORER_NAV_ROWS; ++i) same("navigation row", p.nav_row[i], 4, rows[i], 147, 32);
    same("navigation separator 1", p.nav_separator[0], 12, 220, 131, 1);
    same("navigation separator 2", p.nav_separator[1], 12, 429, 131, 1);
    same("items", p.content, 165, 144, 933, 528);
    same("status", p.status, 0, 672, 1098, 26);
    same("status details", p.status_details, 1038, 674, 28, 22);
    same("status large", p.status_large, 1066, 674, 28, 22);
    /* A folder (?open=explorer:documents): Filter after View, then the separator and See more move right. */
    pw_explorer_place(&p, 1098, 698, 1, 0);
    same("Filter", p.command[PW_EXPLORER_CMD_FILTER], 516.9, 103.5, 86.19, 32);
    same("separator 3 in a folder", p.separator[2], 607.09, 107.5, 1, 24);
    same("See more in a folder", p.command[PW_EXPLORER_CMD_MORE], 612.09, 103.5, 40, 32);
    same("Details in a folder", p.command[PW_EXPLORER_CMD_DETAILS], 1006.2, 103.5, 79.8, 32);
    /* Home, relative to the items area (165,144): sections at 152, 348 and 432 on the window. */
    pw_explorer_home(&home, 933, 0, 10); /* the prototype lists its ten files */
    same("Quick access", home.head[0], 12, 8, 105.2, 32);
    same("Favorites", home.head[1], 12, 204, 85.3, 32);
    same("Recent", home.head[2], 12, 288, 74.1, 32);
    for (i = 0; i < PW_EXPLORER_HOME_TILES; ++i) same("tile", home.tile[i], tiles[i][0], tiles[i][1], 223.25, 72);
    same("favorites note", home.favorites_note, 16, 240, 905, 36);
    same("recent head", home.recent_head, 16, 324, 670, 32);
    if (fabs(home.recent_rows_y - 358) > 0.06) { printf("FAIL recent rows at %.2f, prototype 358\n", home.recent_rows_y); ++failures; }
    /* The page is the prototype's 702 px .ex-page with its 8 px of bottom padding and the 12 px of the items area. */
    if (fabs(home.height - (702 + 12)) > 0.06) { printf("FAIL home height %.2f, prototype 714\n", home.height); ++failures; }
    /* A collapsed Quick access leaves its head: Favorites moves up by the tiles' body. */
    pw_explorer_home(&home, 933, 1, 10);
    same("Favorites under a collapsed Quick access", home.head[1], 12, 44, 85.3, 32);
    /* A narrow window: three columns of tiles no narrower than 196 px. */
    pw_explorer_home(&home, 700, 0, 0);
    if (home.tile[3].x != home.tile[0].x || home.tile[2].w < 196)
    { printf("FAIL narrow tiles: %.2f wide, fourth at %.2f\n", home.tile[2].w, home.tile[3].x); ++failures; }
    /* Documents in each icon view (?fresh&light&open=explorer:documents, Ctrl+Shift+2, 4, 5), relative to the items
     * area's content box (588,311 on the screen): its four items, each name on one line. */
    {
        static const int one[4] = {1, 1, 1, 1}, two[4] = {1, 2, 1, 1};
        PwExplorerBox t[4];
        pw_explorer_tiles(t, 4, PW_EXPLORER_VIEW_LARGE, 913, 516, one);
        for (i = 0; i < 4; ++i) same("large tile", t[i], 118 * i, 8, 116, 128);
        pw_explorer_tiles(t, 4, PW_EXPLORER_VIEW_SMALL, 913, 516, NULL);
        for (i = 0; i < 3; ++i) same("small tile", t[i], 244 * i, 8, 240, 32);
        same("small tile on the second row", t[3], 0, 40, 240, 32);
        pw_explorer_tiles(t, 4, PW_EXPLORER_VIEW_LIST, 913, 516, NULL);
        for (i = 0; i < 4; ++i) same("list tile", t[i], 0, 4 + 32 * i, 225.25, 32);
        /* A name on two lines makes its row 16 px taller; fifteen 32 px rows fill a 516 px list's column. */
        pw_explorer_tiles(t, 4, PW_EXPLORER_VIEW_LARGE, 300, 516, two);
        same("large tile in a two-line row", t[0], 0, 8, 116, 144);
        same("large tile below it", t[2], 0, 8 + 144 + 2, 116, 128);
        pw_explorer_tiles(t, 4, PW_EXPLORER_VIEW_LIST, 913, 12 + 32 * 2 + 31, NULL);
        same("list tile in the second column", t[2], 229.25, 4, 225.25, 32);
    }
    {
        /* The details pane (Alt+Shift+P) on Documents, relative to the pane at 818,144: nothing selected (the hint on
         * two lines), the folder "Save backups" (Contains, Date modified, Path on two lines), two files selected. */
        static const int folder[3] = {1, 1, 2}, two[1] = {1};
        PwExplorerDetails d;
        pw_explorer_place(&p, 1098, 698, 1, 1);
        same("items with the details pane", p.content, 165, 144, 653, 528);
        same("details pane", p.details, 818, 144, 280, 528);
        pw_explorer_details(&d, 1, 2, 1, 0, NULL, 0);
        same("pane icon", d.icon, 910.5 - 818, 20, 96, 96);
        same("pane name", d.name, 835 - 818, 272 - 144, 247, 20);
        same("pane hint", d.below, 835 - 818, 310 - 144, 247, 32);
        pw_explorer_details(&d, 1, 1, 0, 3, folder, 75.5);
        same("pane type", d.below, 835 - 818, 298 - 144, 247, 16);
        same("pane title", d.title, 835 - 818, 330 - 144, 247, 41);
        same("pane key", d.key[0], 835 - 818, 371 - 144, 75.5, 16);
        same("pane value", d.value[1], 922.5 - 818, 395 - 144, 159.5, 16);
        same("pane path", d.value[2], 922.5 - 818, 419 - 144, 159.5, 32);
        pw_explorer_details(&d, 1, 0, 0, 1, two, 68.52);
        same("pane title for two items", d.title, 835 - 818, 308 - 144, 247, 41);
        same("pane value for two items", d.value[0], 915.52 - 818, 349 - 144, 166.48, 16);
    }
    printf("%s: test_pw_explorer_layout\n", failures ? "FAIL" : "PASS");
    return failures != 0;
}
