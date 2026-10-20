/* SPDX-License-Identifier: GPL-3.0-or-later
 * Text drawn with the approved prototype's own glyph pixels, placed where Chromium places them.
 *
 * Chromium 154 on the host draws the prototype's web font (Proton UI = Selawik) with Fontations: skrifa's autohinter in
 * light mode (vertical only), Skia's analytic path coverage, then a gamma 1.1875 / contrast 0.2 preblend chosen by the
 * paint colour's luminance. GDI places glyphs on whole-pixel advances with FreeType's own hinting and gamma ("10/16/2026"
 * at 12 px: 58 px wide natively, 61.1 in the prototype), and FreeType 2.14's light autohinter on the same file matches
 * only 48% of Chromium's ink pixels. So the glyphs are measured, not re-rendered (tools/measure-prototype-glyphs.mjs):
 * each character alone, in one element's font and colour, at the four pen phases Skia draws at, as premultiplied BGRA.
 *
 * Skia's grid, measured: a glyph's pen is x + 1/8 floored to a quarter pixel (quarter phases 0..3, horizontal only),
 * on the rounded baseline; advances are the font's linear advances (no kerning in Selawik). Blink right-aligns a run
 * by its width rounded up to its 1/64 px layout unit. Composed this way, the captured strings of the taskbar clock are
 * equal channel for channel except where two glyphs' boxes overlap ("/" beside a digit), which are at most one level
 * off: Chromium blends each glyph onto the page, this composes them over transparent first (tests/test_pw_text_glyphs.c).
 *
 * A glyph is premultiplied BGRA, or, for a layer of one colour (the desktop's white labels and their black shadows), one alpha
 * byte per pixel that the font's colour tints: a quarter of the size, which matters in comctl32 (every program loads it).
 *
 * Positions are 16.16 fixed point pixels (int: a screen is far narrower than 32768 px). Header-only: Explorer's taskbar (Wine
 * patch 1140), the desktop's labels (src/pw_desktop_icons.h, comctl32 in patch 1105) and the host tests use it. */
#ifndef PW_TEXT_GLYPHS_H
#define PW_TEXT_GLYPHS_H

/* One phase of a glyph: its box from the pen's whole pixel and the baseline, premultiplied BGRA rows (w x h), or (bgra NULL)
 * alpha rows the font's colour tints. */
typedef struct { signed char x, y; unsigned char w, h; const unsigned int *bgra; const unsigned char *alpha; } PwGlyphImage;
typedef struct { unsigned short ch; int advance; PwGlyphImage phase[4]; } PwGlyph;
/* baseline: rows from the top of a line box of line pixels to the baseline, as the prototype lays the element out; color: the
 * opaque premultiplied BGRA that alpha glyphs take. */
typedef struct { const PwGlyph *glyphs; unsigned count; int baseline, line; unsigned int color; } PwGlyphFont;

static inline const PwGlyph *pw_text_glyph(const PwGlyphFont *font, unsigned short ch)
{
    for (unsigned i = 0; i < font->count; ++i) if (font->glyphs[i].ch == ch) return &font->glyphs[i];
    return 0;
}

/* The sum of the advances, or -1 when a character has no measured glyph (the caller draws that text its own way). */
static inline int pw_text_width(const PwGlyphFont *font, const unsigned short *text, int length)
{
    int width = 0;
    for (int i = 0; i < length; ++i)
    {
        const PwGlyph *glyph = pw_text_glyph(font, text[i]);
        if (!glyph) return -1;
        width += glyph->advance;
    }
    return width;
}

/* Where a run right-aligned at the whole pixel right starts: Blink's box is the width rounded up to 1/64 px. */
static inline int pw_text_right_aligned(int width, int right)
{
    return right * 65536 - ((width + 1023) & ~1023);
}

/* Premultiplied source-over, rounded as a division by 255. */
static inline unsigned pw_text_over(unsigned source, unsigned destination)
{
    unsigned keep = 255 - (source >> 24), result = 0;
    for (int shift = 0; shift < 32; shift += 8)
        result |= (((source >> shift) & 255) + ((((destination >> shift) & 255) * keep + 127) / 255)) << shift;
    return result;
}

/* The opaque colour at alpha, premultiplied, rounded as a division by 255. */
static inline unsigned pw_text_tint(unsigned color, unsigned alpha)
{
    unsigned result = 0;
    for (int shift = 0; shift < 32; shift += 8) result |= ((((color >> shift) & 255) * alpha + 127) / 255) << shift;
    return result;
}

/* Blink's width of a run: its advances summed, rounded up to its 1/64 px layout unit. -1 when a character has no glyph. */
static inline int pw_text_layout_width(const PwGlyphFont *font, const unsigned short *text, int length)
{
    int width = pw_text_width(font, text, length);
    return width < 0 ? -1 : (width + 1023) >> 10;
}

/* Where Blink ends a line of text[start..n) at most available (1/64 px) wide: the widest break opportunity that fits
 * (before a space, after a hyphen, or n), or, when a word alone is wider than a line, after as many characters as fit
 * (overflow-wrap: anywhere; one at least). The next line starts past a space the line ended before. */
static inline int pw_text_break(const PwGlyphFont *font, const unsigned short *text, int start, int n, int available)
{
    int end = -1;

    for (int p = start + 1; p <= n; ++p)
    {
        if (p < n && text[p] != ' ' && text[p - 1] != '-') continue;
        if (pw_text_layout_width(font, text + start, p - start) > available) break;
        end = p;
    }
    if (end < 0)
        for (end = start + 1; end < n && pw_text_layout_width(font, text + start, end + 1 - start) <= available; ++end) {}
    return end;
}

/* Composes the text into a premultiplied BGRA buffer (width x height, top-down rows of width words), its pen starting
 * at x and its baseline on row baseline; pixels outside the buffer are clipped. FALSE when a character has no glyph,
 * having drawn nothing. */
static inline int pw_text_draw(const PwGlyphFont *font, const unsigned short *text, int length, int x, int baseline,
                               unsigned int *bgra, int width, int height)
{
    if (pw_text_width(font, text, length) < 0) return 0;
    for (int i = 0; i < length; ++i)
    {
        const PwGlyph *glyph = pw_text_glyph(font, text[i]);
        int pen = x + 0x2000;
        const PwGlyphImage *image = &glyph->phase[(pen >> 14) & 3];
        int left = (int)(pen >> 16) + image->x, top = baseline + image->y;
        for (int row = 0; row < image->h; ++row)
        {
            if (top + row < 0 || top + row >= height) continue;
            for (int column = 0; column < image->w; ++column)
            {
                unsigned source = image->bgra ? image->bgra[row * image->w + column]
                                              : pw_text_tint(font->color, image->alpha[row * image->w + column]), *target;
                if (!source || left + column < 0 || left + column >= width) continue;
                target = &bgra[(top + row) * width + left + column];
                *target = pw_text_over(source, *target);
            }
        }
        x += glyph->advance;
    }
    return 1;
}
#endif
