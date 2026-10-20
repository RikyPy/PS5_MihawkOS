/* SPDX-License-Identifier: GPL-3.0-or-later
 * src/pw_text_glyphs.h against Chromium: the taskbar clock's strings composed from the prototype's own glyphs
 * (.deps/windows11-theme/pw_shell_glyphs.h) equal the same strings as Chromium drew them, right-aligned at a whole pixel
 * (pw_shell_glyph_checks.h; both from themes/windows11/taskbar/clock-*.json by tools/build-shell-glyphs.py). Every
 * channel is equal except where two glyphs' boxes overlap, which is at most one level off. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pw_shell_glyphs.h"
#include "pw_shell_glyph_checks.h"

static int failures;

static int wide(const char *text, unsigned short *out)
{
    int length = 0;
    while (text[length]) { out[length] = (unsigned char)text[length]; ++length; }
    return length;
}

static void check(const char *theme, const PwGlyphFont *font, const struct pw_glyph_check *c)
{
    unsigned short text[64]; int length = wide(c->text, text), width = pw_text_width(font, text, length);
    unsigned *got = calloc((size_t)c->w * c->h, 4), *overlap = calloc((size_t)c->w * c->h, 4);
    int exact = 0, off = 0, worst = 0, x = c->x - c->x0 * 65536, right;
    if (width < 0) { printf("FAIL %s \"%s\": a character has no glyph\n", theme, c->text); ++failures; goto done; }
    /* Chromium right-aligned it at a whole pixel: its start is that pixel less the width in 1/64 px units. */
    right = (c->x + ((width + 1023) & ~1023)) >> 16;
    if (pw_text_right_aligned(width, right) != c->x)
    { printf("FAIL %s \"%s\": starts at %d/65536, right-aligned at %d gives %d\n", theme, c->text, c->x, right,
             pw_text_right_aligned(width, right)); ++failures; }
    if (!pw_text_draw(font, text, length, x, c->baseline - c->y0, got, c->w, c->h))
    { printf("FAIL %s \"%s\": not drawn\n", theme, c->text); ++failures; goto done; }
    /* Each glyph alone, counted per pixel: the pixels two glyphs share. */
    for (int i = 0, pen = x; i < length; pen += pw_text_glyph(font, text[i])->advance, ++i)
    {
        unsigned *one = calloc((size_t)c->w * c->h, 4);
        pw_text_draw(font, &text[i], 1, pen, c->baseline - c->y0, one, c->w, c->h);
        for (int p = 0; p < c->w * c->h; ++p) overlap[p] += one[p] != 0;
        free(one);
    }
    for (int p = 0; p < c->w * c->h; ++p)
        for (int shift = 0; shift < 32; shift += 8)
        {
            int d = abs((int)((got[p] >> shift) & 255) - (int)((c->bgra[p] >> shift) & 255));
            if (d > worst) worst = d;
            if (!d) ++exact;
            else if (d > 1 || overlap[p] < 2)
            {
                if (!off++) printf("FAIL %s \"%s\": pixel %d,%d channel %d is %u, Chromium %u (%u glyphs there)\n", theme,
                                   c->text, p % c->w, p / c->w, shift / 8, (got[p] >> shift) & 255,
                                   (c->bgra[p] >> shift) & 255, overlap[p]);
            }
        }
    if (off) ++failures;
    printf("%s \"%s\": %d of %d channels equal, worst %d\n", theme, c->text, exact, c->w * c->h * 4, worst);
done:
    free(got); free(overlap);
}

int main(void)
{
    static const struct { const char *theme; const struct pw_glyph_check *checks; size_t count; } themes[2] = {
        {"light", pw_glyph_checks_light, sizeof(pw_glyph_checks_light) / sizeof(*pw_glyph_checks_light)},
        {"dark", pw_glyph_checks_dark, sizeof(pw_glyph_checks_dark) / sizeof(*pw_glyph_checks_dark)},
    };
    unsigned short text[16]; unsigned pixel = 0;
    for (int t = 0; t < 2; ++t)
        for (size_t i = 0; i < themes[t].count; ++i) check(themes[t].theme, &pw_shell_clock_glyphs[t], &themes[t].checks[i]);
    /* A character without a glyph: no width, nothing drawn, so the caller keeps its own drawing. */
    if (pw_text_width(&pw_shell_clock_glyphs[0], text, wide("2:18 pm", text)) != -1 ||
        pw_text_draw(&pw_shell_clock_glyphs[0], text, wide("2:18 pm", text), 0, 13, &pixel, 1, 1) || pixel)
    { printf("FAIL an unmeasured character was drawn\n"); ++failures; }
    /* Clipped at every edge: a run far outside the buffer writes nothing. */
    if (!pw_text_draw(&pw_shell_clock_glyphs[1], text, wide("10/16/2026", text), -100 * 65536, -50, &pixel, 1, 1) || pixel)
    { printf("FAIL a run outside the buffer drew into it\n"); ++failures; }
    printf(failures ? "%d failures\n" : "text glyphs: ok\n", failures);
    return failures != 0;
}
