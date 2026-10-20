/* SPDX-License-Identifier: GPL-3.0-or-later
 * src/pw_tooltip.h against Chromium: three tooltips (one line, three lines from newlines, a line broken at 302 px) laid out
 * and composed from the prototype's own glyphs (.deps/windows11-theme/pw_tooltip_glyphs.h) beside their text as Chromium
 * drew it (pw_tooltip_glyph_checks.h; both from themes/windows11/tooltip by tools/build-tooltip-glyphs.py), light and
 * dark: the box is Chromium's to the pixel and the text within a level everywhere (exact but where glyph boxes overlap).
 * Then pre-line's white space, a character with no glyph, and the placement the prototype's ui.position gives. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pw_tooltip.h"
#include "pw_tooltip_glyphs.h"
#include "pw_tooltip_glyph_checks.h"

static int failures;
#define CHECK(condition, ...) do { if (!(condition)) { printf("FAIL " __VA_ARGS__); printf("\n"); ++failures; } } while (0)

static int wide(const char *text, unsigned short *out)
{
    int length = 0;
    while (text[length]) { out[length] = (unsigned char)text[length]; ++length; }
    return length;
}

static int channel_error(unsigned a, unsigned b)
{
    int worst = 0;
    for (int shift = 0; shift < 32; shift += 8)
    {
        int d = (int)((a >> shift) & 255) - (int)((b >> shift) & 255);
        if (d < 0) d = -d;
        if (d > worst) worst = d;
    }
    return worst;
}

static void check_tip(const struct pw_tooltip_check *c)
{
    static PwTooltipLayout layout;
    unsigned short text[400];
    int length = wide(c->text, text), exact = 0, ink = 0, worst = 0;
    unsigned *got;

    if (!pw_tooltip_layout(&pw_tooltip_fonts[c->dark], text, length, &layout))
    {
        CHECK(0, "\"%.20s...\": a character has no glyph", c->text);
        return;
    }
    CHECK(layout.width == c->w && layout.height == c->h, "\"%.20s...\" (%s): a %d x %d box, Chromium's is %d x %d", c->text,
          c->dark ? "dark" : "light", layout.width, layout.height, c->w, c->h);
    got = calloc((size_t)c->w * c->h, 4);
    pw_tooltip_draw(&pw_tooltip_fonts[c->dark], &layout, 0, 0, got, c->w, c->h);
    for (int i = 0; i < c->w * c->h; ++i)
    {
        int error = channel_error(got[i], c->bgra[i]);
        if (!got[i] && !c->bgra[i]) continue;
        ++ink;
        exact += !error;
        if (error > worst) worst = error;
    }
    printf("\"%.24s...\" (%s): %d lines, %d x %d, %d ink pixels, %.1f%% exact, worst %d\n", c->text, c->dark ? "dark" : "light",
           layout.lines, layout.width, layout.height, ink, 100.0 * exact / ink, worst);
    CHECK(worst <= 1, "\"%.20s...\" (%s): the text is not Chromium's", c->text, c->dark ? "dark" : "light");
    free(got);
}

static void check_white_space(void)
{
    static PwTooltipLayout layout;
    unsigned short text[64], tip[2] = {'a', 0x4e00};
    int length = wide("  two   spaces \n\n  next ", text);

    CHECK(pw_tooltip_layout(&pw_tooltip_fonts[0], text, length, &layout) && layout.lines == 3 && layout.line[0].length == 10 &&
          layout.line[1].length == 0 && layout.line[2].length == 4 && !memcmp(layout.text + layout.line[2].start, u"next", 8),
          "pre-line white space is not collapsed as CSS collapses it");
    CHECK(!pw_tooltip_layout(&pw_tooltip_fonts[0], tip, 2, &layout), "a character with no glyph is laid out");
}

static void check_place(void)
{
    /* the prototype's work area at 1920 x 1080: the taskbar's 48 px below it */
    static const struct { int px, py, w, h, x, y; } cases[] = {
        {37, 129, 293, 30, 37, 149},       /* the Recycle Bin's infotip, as the reference capture has it */
        {1900, 500, 293, 30, 1607, 520},   /* no room on the right: the box ends at the pointer */
        {1915, 500, 320, 62, 1592, 520},   /* ... and stays 8 px inside the screen */
        {500, 1020, 200, 62, 500, 958},    /* no room below the pointer: above it */
        {2, 3, 200, 30, 8, 23},            /* 8 px inside the left edge */
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); ++i)
    {
        int x, y;
        pw_tooltip_place(cases[i].px, cases[i].py, cases[i].w, cases[i].h, 0, 0, 1920, 1032, &x, &y);
        CHECK(x == cases[i].x && y == cases[i].y, "pointer %d,%d: the tip at %d,%d, the prototype's at %d,%d", cases[i].px,
              cases[i].py, x, y, cases[i].x, cases[i].y);
    }
}

int main(void)
{
    for (size_t i = 0; i < sizeof(pw_tooltip_checks) / sizeof(*pw_tooltip_checks); ++i) check_tip(&pw_tooltip_checks[i]);
    check_white_space();
    check_place();
    if (failures) { printf("%d failures\n", failures); return 1; }
    printf("ok\n");
    return 0;
}
