/* SPDX-License-Identifier: GPL-3.0-or-later
 * src/pw_menu_style.h against the approved prototype's menus: the layout against the boxes measured with
 * tools/measure-prototype-surface.mjs (open=menu-desktop and menu-desktop-item, light), the plates drawn from their pieces
 * against the rows as captured (tools/build-menu-sprites.py's pw_menu_reference.h), the text colours against the tokens,
 * and the labels and shortcuts Chromium laid out against the same strings composed from its glyphs (pw_menu_glyphs.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pw_menu_style.h"
#include "pw_menu_sprites.h"
#include "pw_menu_glyphs.h"
#include "pw_menu_reference.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); ++failures; } } while (0)

/* The plate drawn into a row of the reference's size equals the capture in every channel. */
static void plate(const char *name, const PwMenuPlate *piece, const unsigned int *reference, int width, int left, int right)
{
    static unsigned int row[400 * PW_MENU_ROW];
    int wrong = 0;
    memset(row, 0, sizeof(row));
    pw_menu_plate(row, width, PW_MENU_ROW, left, right, 0, piece);
    for (int i = 0; i < width * PW_MENU_ROW; ++i) wrong += row[i] != reference[i];
    CHECK(!wrong, "%s: %d of %d pixels differ from the capture", name, wrong, width * PW_MENU_ROW);
}

/* A string Chromium laid out, composed from the glyph font where Chromium put its pen: every channel within a level
 * (Chromium blends each glyph onto the page, this composes over transparent first: overlapping boxes may round apart). */
static void string(const struct pw_menu_glyph_check *c)
{
    const PwGlyphFont *font = &pw_menu_fonts[c->dark][c->font];
    unsigned short text[64];
    unsigned int *got = calloc((size_t)c->w * c->h, sizeof(*got));
    int length = (int)strlen(c->text), exact = 0, worst = 0;
    for (int i = 0; i < length; ++i) text[i] = (unsigned char)c->text[i];
    CHECK(got && pw_text_draw(font, text, length, c->x - c->x0 * 65536, c->baseline - c->y0, got, c->w, c->h),
          "\"%s\" has a character with no glyph", c->text);
    for (int i = 0; got && i < c->w * c->h; ++i)
    {
        int off = 0;
        for (int shift = 0; shift < 32; shift += 8)
        {
            int d = abs((int)(got[i] >> shift & 255) - (int)(c->bgra[i] >> shift & 255));
            if (d > off) off = d;
        }
        exact += !off;
        if (off > worst) worst = off;
    }
    CHECK(worst <= 1, "%s \"%s\": %d of %d pixels exact, worst %d levels", c->dark ? "dark" : "light", c->text, exact,
          c->w * c->h, worst);
    free(got);
}

int main(void)
{
    /* menu-desktop at 600,400: the list at 601,403, rows at 605 from 403 by 32, separators 5 px, icons at 616, labels at
     * 644, "Show more options" 120.44 px and "Shift+F10" 50.91 px make the menu 267.34 px wide. */
    CHECK(PW_MENU_INSET + PW_MENU_ROW_MARGIN == 605 - 600 + 2, "row left");
    CHECK(PW_MENU_BORDER + PW_MENU_ROW_MARGIN + pw_menu_icon_x(0) == 616 - 600, "icon x %d", pw_menu_icon_x(0));
    CHECK(PW_MENU_BORDER + PW_MENU_ROW_MARGIN + pw_menu_label_x(0, 1) == 644 - 600, "label x %d", pw_menu_label_x(0, 1));
    {
        /* the glyph font's own widths of the two texts, rounded once for the whole row */
        unsigned short show[] = {'S','h','o','w',' ','m','o','r','e',' ','o','p','t','i','o','n','s'}, key[] = {'S','h','i','f','t','+','F','1','0'};
        int label = pw_text_width(&pw_menu_fonts[0][0], show, 17), shortcut = pw_text_width(&pw_menu_fonts[0][1], key, 9);
        int row = pw_menu_row_width(0, 1, label, shortcut, 0);
        CHECK(abs(label - (int)(120.44 * 65536)) < 656 && abs(shortcut - (int)(50.91 * 65536)) < 656, "text widths %f %f",
              label / 65536.0, shortcut / 65536.0);
        CHECK(pw_menu_width((row + 32768) / 65536 + 2 * PW_MENU_ROW_MARGIN, 0, 1) == 267, "desktop menu width %d",
              pw_menu_width((row + 32768) / 65536 + 2 * PW_MENU_ROW_MARGIN, 0, 1));
    }
    CHECK(pw_menu_row_width(0, 1, (int)(159.34 * 65536), 0, 1) / 65536 == 257, "a chevron row: View's label box 159.34 px in a 257.34 px row");
    {
        unsigned short in[] = {'S','&','o','r','t',' ','a','&','&','b'}, out[16];
        int key, length = pw_menu_strip(in, 10, out, &key);
        CHECK(length == 8 && key == 1 && out[1] == 'o' && out[6] == '&' && out[7] == 'b', "strip: %d characters, key %d", length, key);
    }
    /* View's submenu: checks and no icons, labels 44 px into the menu. */
    CHECK(PW_MENU_BORDER + PW_MENU_ROW_MARGIN + pw_menu_label_x(1, 0) == 44, "submenu label x");
    /* menu-desktop-item: the command row's cells from 3 px in, 40 px apart; narrow menus are 200 px. */
    CHECK(pw_menu_width(0, 0, 1) == 200 && pw_menu_width(500, 0, 1) == 456 && pw_menu_width(500, 0, 2) == 502, "width limits");
    CHECK(pw_menu_width(0, 6, 1) == 6 * 40 + 4 + 2, "a command row wider than every row");
    /* Two rows and a separator: 32 + 5 + 32, as View, Sort by, Refresh then New at 403, 435, 467, 504. */
    CHECK(403 + 3 * PW_MENU_ROW + PW_MENU_SEPARATOR == 504, "separator pitch");

    /* --TextFillColorPrimary rgba(0,0,0,0.894), Secondary 0.62, Disabled 0.361; dark #fff, 0.773, 0.365. */
    CHECK(pw_menu_text_colour(0, PW_MENU_PRIMARY) == 0xe4000000u, "primary light %08x", pw_menu_text_colour(0, PW_MENU_PRIMARY));
    CHECK(pw_menu_text_colour(1, PW_MENU_PRIMARY) == 0xffffffffu, "primary dark");
    CHECK(pw_menu_text_colour(1, PW_MENU_SECONDARY) == 0xc5c5c5c5u, "secondary dark");
    CHECK(pw_menu_text_colour(0, PW_MENU_DISABLED) == 0x5c000000u, "disabled light");
    /* Skia's mask gamma: ends fixed; white text a^0.842, black text 1 - (1 - c)^0.842 after the contrast boost. */
    CHECK(pw_menu_text_mask(0, 0) == 0 && pw_menu_text_mask(255, 0) == 255 && pw_menu_text_mask(0, 1) == 0 &&
          pw_menu_text_mask(255, 1) == 255, "mask ends");
    CHECK(pw_menu_text_mask(128, 1) == 143, "white at 128: %u", pw_menu_text_mask(128, 1));
    CHECK(pw_menu_text_mask(128, 0) == 125, "black at 128: %u", pw_menu_text_mask(128, 0));
    for (unsigned i = 1; i < 256; ++i)
        CHECK(pw_menu_text_mask(i, 0) >= pw_menu_text_mask(i - 1, 0) && pw_menu_text_mask(i, 1) >= pw_menu_text_mask(i - 1, 1),
              "mask not monotonic at %u", i);
    /* Composition: over nothing a premultiplied pixel is itself; over opaque white, black at 15 is 240. */
    CHECK(pw_menu_over(0, 0x0f000000u) == 0x0f000000u, "over transparent");
    CHECK(pw_menu_over(0xffffffffu, 0x0f000000u) == 0xfff0f0f0u, "over white %08x", pw_menu_over(0xffffffffu, 0x0f000000u));

    /* The plates redrawn from their pieces at the capture's 258 px row (the keyboard ring from 2 px outside it). */
    plate("hover light", &pw_menu_chrome[0].hover, pw_menu_reference_light_hover, 258, 0, 258);
    plate("keyboard light", &pw_menu_chrome[0].keyboard, pw_menu_reference_light_keyboard, 262, 2, 260);
    plate("hover dark", &pw_menu_chrome[1].hover, pw_menu_reference_dark_hover, 258, 0, 258);
    plate("keyboard dark", &pw_menu_chrome[1].keyboard, pw_menu_reference_dark_keyboard, 262, 2, 260);
    /* DividerStrokeColorDefault: black at 5.9% (15), white at 8.2% (21). */
    CHECK(pw_menu_chrome[0].separator == 0x0f000000u && pw_menu_chrome[1].separator == 0x15151515u, "separators %08x %08x",
          pw_menu_chrome[0].separator, pw_menu_chrome[1].separator);

    for (unsigned i = 0; i < sizeof(pw_menu_glyph_checks) / sizeof(pw_menu_glyph_checks[0]); ++i) string(&pw_menu_glyph_checks[i]);

    printf("%s\n", failures ? "FAILED" : "ok");
    return failures != 0;
}
