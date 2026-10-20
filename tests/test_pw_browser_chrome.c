/* SPDX-License-Identifier: GPL-3.0-or-later
 * src/pw_browser_chrome.h against the approved prototype's measurement (docs/windows11-reference/mihawk-browser.json):
 * its plates' pixels where the reference sampled them, over the window's surface or the flyout's acrylic, its type
 * colours, its file sizes and its Downloads rows' boxes. Pixels match to the unit except where noted. */
#include "../src/pw_browser_chrome.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void expect_rgb(const char *what, uint32_t pixel, int r, int g, int b, int tolerance)
{
    int pr = pixel >> 16 & 255, pg = pixel >> 8 & 255, pb = pixel & 255;
    if (abs(pr - r) > tolerance || abs(pg - g) > tolerance || abs(pb - b) > tolerance) {
        fprintf(stderr, "%s: %d,%d,%d, expected %d,%d,%d\n", what, pr, pg, pb, r, g, b);
        ++failures;
    }
}

/* A plate of w x h over under, sampled at (x, y) inside it. */
static uint32_t plate_at(int w, int h, float radius, PwRgba fill, PwRgba stroke, PwRgba bottom, int bottom_width,
                         PwRgba under, int x, int y)
{
    uint32_t *pixels = malloc((size_t)w * h * 4);
    pw_chrome_plate(pixels, w, h, radius, fill, stroke, bottom, bottom_width, &under);
    uint32_t pixel = pixels[y * w + x];
    free(pixels);
    return pixel;
}

static void plates(int dark)
{
    const PwChromeTheme *t = pw_chrome_theme(dark);
    char name[64];
    /* launcher-0 (200 x 40 at 331,160): fill at 335,180, top at 431,160, bottom at 431,199; hover and press */
    static const int launcher[2][3] = {{252, 228, 204}, {45, 53, 47}};
    for (int i = 0; i < 3; ++i) {
        static const int at[3][2] = {{4, 20}, {100, 0}, {100, 39}};
        snprintf(name, sizeof name, "%s launcher %d", dark ? "dark" : "light", i);
        expect_rgb(name, plate_at(200, 40, 4, t->control, t->control_stroke, t->control_stroke_bottom, 1, t->surface,
                                  at[i][0], at[i][1]), launcher[dark][i], launcher[dark][i], launcher[dark][i], 0);
    }
    int hover = dark ? 50 : 246, press = dark ? 39 : 244;
    expect_rgb("launcher hover", plate_at(200, 40, 4, t->control_hover, t->control_stroke, t->control_stroke_bottom, 1,
                                          t->surface, 3, 20), hover, hover, hover, 0);
    /* Chromium rounds 244.8 down; so does nothing here */
    expect_rgb("launcher press", plate_at(200, 40, 4, t->control_pressed, t->control_stroke, t->control_stroke_bottom,
                                          1, t->surface, 3, 20), press, press, press, 1);
    /* the address box (1126 x 32 at 112,4): fill at 1230,20, top at 675,4, bottom at 675,35; unfocused, then focused */
    static const int address[2][3] = {{252, 228, 134}, {45, 47, 153}};
    for (int i = 0; i < 3; ++i) {
        static const int at[3][2] = {{1118, 16}, {563, 0}, {563, 31}};
        snprintf(name, sizeof name, "%s address %d", dark ? "dark" : "light", i);
        expect_rgb(name, plate_at(1126, 32, 4, t->control, t->input_stroke, t->input_stroke_bottom, 1, t->surface,
                                  at[i][0], at[i][1]), address[dark][i], address[dark][i], address[dark][i], 0);
    }
    int focused = dark ? 30 : 255;
    expect_rgb("address focused fill", plate_at(1126, 32, 4, t->input_focused, t->input_stroke, t->accent, 2,
                                                t->surface, 1118, 16), focused, focused, focused, 0);
    expect_rgb("address focused line", plate_at(1126, 32, 4, t->input_focused, t->input_stroke, t->accent, 2,
                                                t->surface, 563, 30), t->accent.r, t->accent.g, t->accent.b, 0);
    /* command buttons: reload hover and press at 79,20 */
    PwRgba hover_fill = pw_chrome_blend(t->subtle_hover, t->surface), press_fill = pw_chrome_blend(t->subtle_pressed, t->surface);
    expect_rgb("reload hover", (uint32_t)hover_fill.r << 16 | hover_fill.g << 8 | hover_fill.b,
               dark ? 45 : 234, dark ? 45 : 234, dark ? 45 : 234, 0);
    expect_rgb("reload press", (uint32_t)press_fill.r << 16 | press_fill.g << 8 | press_fill.b,
               dark ? 40 : 237, dark ? 40 : 237, dark ? 40 : 237, 0);
    /* over the flyout: Cancel (fill at 979,169 of 975,153; top, bottom), the accent Open, and the row progress */
    static const int button[2][3] = {{254, 237, 212}, {55, 63, 59}};
    for (int i = 0; i < 3; ++i) {
        static const int at[3][2] = {{4, 16}, {32, 0}, {32, 31}};
        snprintf(name, sizeof name, "%s flyout button %d", dark ? "dark" : "light", i);
        /* the acrylic under it is not quite uniform: within 2 in dark */
        expect_rgb(name, plate_at(65, 32, 4, t->control, t->control_stroke, t->control_stroke_bottom, 1, t->flyout,
                                  at[i][0], at[i][1]), button[dark][i], button[dark][i], button[dark][i], dark ? 2 : 0);
    }
    expect_rgb("accent fill", plate_at(58, 32, 4, t->accent, t->accent_stroke, t->accent_stroke_bottom, 1, t->flyout,
                                       4, 16), t->accent.r, t->accent.g, t->accent.b, 0);
    int accent_bottom = dark ? 38 : 153;
    expect_rgb("accent bottom", plate_at(58, 32, 4, t->accent, t->accent_stroke, t->accent_stroke_bottom, 1, t->flyout,
                                         29, 31), accent_bottom, accent_bottom, accent_bottom, 3);
    uint32_t bar[282 * 3];
    pw_chrome_progress(bar, 282, 42, t->progress_track, t->accent, &t->flyout);
    int track = dark ? 159 : 139;
    expect_rgb("progress track", bar[282 + 280], track, track, track, 0);
    expect_rgb("progress fill", bar[282 + 2], t->accent.r, t->accent.g, t->accent.b, 0);
    expect_rgb("progress above the track", bar[280], t->flyout.r, t->flyout.g, t->flyout.b, 0);
    PwRgba divider = pw_chrome_blend(t->divider, t->flyout);
    expect_rgb("divider", (uint32_t)divider.r << 16 | divider.g << 8 | divider.b, dark ? 62 : 237, dark ? 62 : 237,
               dark ? 62 : 237, 1);
    /* type: primary and secondary over the surface */
    PwRgba text = pw_chrome_blend(t->text, t->surface), secondary = pw_chrome_blend(t->text_secondary, t->surface);
    assert(text.r == (dark ? 255 : 26) && secondary.r == (dark ? 204 : 92));
}

static void size(long long bytes, const char *expected)
{
    char out[32];
    pw_chrome_size(bytes, out, sizeof out);
    if (strcmp(out, expected)) { fprintf(stderr, "size %lld: \"%s\", expected \"%s\"\n", bytes, out, expected); ++failures; }
}

static void box(const char *what, PwBox b, int x, int y, int w, int h)
{
    /* the reference's boxes are in client coordinates; the flyout's top left is at 914,40 there */
    if (b.x + 914 != x || b.y + 40 != y || b.w != w || b.h != h) {
        fprintf(stderr, "%s: %d,%d %dx%d, expected %d,%d %dx%d\n", what, b.x + 914, b.y + 40, b.w, b.h, x, y, w, h);
        ++failures;
    }
}

static void rows(void)
{
    /* downloads-light: an active EpicGamesLauncherInstaller.msi, then a finished ExampleSetup.exe */
    PwRowState states[2] = {PW_ROW_ACTIVE, PW_ROW_DONE};
    assert(pw_chrome_flyout_height(states, 2, 718) == 265);
    assert(pw_chrome_flyout_height(states, 0, 718) == 82);
    assert(pw_chrome_flyout_height(states, 2, 200) == 200);
    int cancel[1] = {41}, done[2] = {34, 89};  /* "Cancel", "Open", "Show in folder" in Body, rounded */
    PwRowBoxes r;
    pw_chrome_row(PW_ROW_ACTIVE, 1 + PW_FLYOUT_HEAD, cancel, &r);
    box("row-0", r.row, 915, 81, 358, 116);
    box("row-0-icon", r.icon, 931, 92, 32, 32);
    box("row-0-name", r.name, 975, 92, 282, 20);
    box("row-0-progress", r.progress, 975, 120, 282, 3);
    box("row-0-status", r.status, 975, 129, 282, 16);
    box("row-0-action-0", r.action[0], 975, 153, 65, 32);
    assert(r.actions == 1);
    pw_chrome_row(PW_ROW_DONE, 1 + PW_FLYOUT_HEAD + 116, done, &r);
    box("row-1", r.row, 915, 197, 358, 103);
    box("row-1-icon", r.icon, 931, 208, 32, 32);
    box("row-1-status", r.status, 975, 232, 282, 16);
    box("row-1-action-0", r.action[0], 975, 256, 58, 32);
    box("row-1-action-1", r.action[1], 1041, 256, 113, 32);
    assert(r.actions == 2 && !r.progress.w);
    pw_chrome_row(PW_ROW_CANCELED, 0, NULL, &r);
    assert(r.actions == 0 && r.row.h == 63);
}

int main(void)
{
    plates(0);
    plates(1);
    size(512, "512 bytes");
    size(1536, "1.5 KB");
    size(2380800, "2.3 MB");           /* ExampleSetup.exe, row-1-status */
    size(80ll << 20, "80 MB");          /* row-0-status: "80 MB of 191 MB" */
    size(191ll << 20, "191 MB");
    size(10ll << 30, "10 GB");
    rows();
    if (failures) { fprintf(stderr, "test_pw_browser_chrome: %d failures\n", failures); return 1; }
    puts("test_pw_browser_chrome: ok");
    return 0;
}
