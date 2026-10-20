/* SPDX-License-Identifier: GPL-3.0-or-later
 * src/pw_desktop_icons.h against Chromium: the desktop's labels laid out and composed from the prototype's own glyphs
 * (.deps/windows11-theme/pw_desktop_sprites.h) beside the same labels as Chromium drew them (pw_desktop_sprite_checks.h;
 * both from themes/windows11/desktop by tools/build-desktop-sprites.py): the short names, a two-line one, names clamped
 * with "…" (after a hyphen break, a word broken anywhere, a truncated word), collapsed spaces and a selected item's whole
 * name at medium icons, and some of them at small and large. Where the text is solid its pixels must be Chromium's
 * exactly (the layout and the glyphs); the shadows, composed glyph by glyph, stay within the measured spread of
 * Chromium's single capture (printed per name). Then the grid at each size, the plates' nine-slices and the band. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pw_desktop_icons.h"
#include "pw_desktop_sprites.h"
#include "pw_desktop_sprite_checks.h"

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

static void check_label(const struct pw_desktop_label_check *c)
{
    const PwGlyphFont *layers[3] = {&pw_desktop_label_fonts[0], &pw_desktop_label_fonts[1], &pw_desktop_label_fonts[2]};
    static const PwGlyphFont none = {0};   /* no glyphs: its layer draws nothing */
    const PwGlyphFont *text_only[3] = {&none, &none, &pw_desktop_label_fonts[2]};
    PwDesktopGrid grid = pw_desktop_grid(c->icon);
    unsigned short name[64];
    PwDesktopLabel label;
    int length = wide(c->name, name), pixels = c->w * c->h, ink = 0, within = 0, worst = 0, solid_wrong = 0;
    unsigned *got = calloc(pixels, 4), *text = calloc(pixels, 4);

    if (!pw_desktop_label_layout(&pw_desktop_label_fonts[2], name, length, c->clamped ? PW_DESKTOP_CLAMP : 0, &grid, &label))
    {
        CHECK(0, "\"%s\": a character has no glyph", c->name);
        goto done;
    }
    CHECK(pw_desktop_plate_width(&grid) == c->w && pw_desktop_plate_height(&grid, label.lines) == c->h,
          "\"%s\" at %d px: %d lines make a %d x %d plate, Chromium's is %d x %d", c->name, c->icon, label.lines,
          pw_desktop_plate_width(&grid), pw_desktop_plate_height(&grid, label.lines), c->w, c->h);
    pw_desktop_label_draw(layers, &label, c->clamped, 0, 0, got, c->w, c->h);
    /* the text alone: where it is solid, nothing under it shows, so those pixels are the text's placement and glyphs */
    pw_desktop_label_draw(text_only, &label, c->clamped, 0, 0, text, c->w, c->h);
    for (int i = 0; i < pixels; ++i)
    {
        int error = channel_error(got[i], c->bgra[i]);
        if (!got[i] && !c->bgra[i]) continue;
        ++ink;
        within += error <= 1;
        if (error > worst) worst = error;
        if ((text[i] >> 24) == 255 && got[i] != c->bgra[i]) ++solid_wrong;
    }
    printf("\"%s\" at %d px: %d line%s%s, %d ink pixels, %.1f%% within one level, worst %d\n", c->name, c->icon, label.lines,
           label.lines == 1 ? "" : "s", label.line[label.lines - 1].ellipsis ? " (clamped)" : "", ink, 100.0 * within / ink, worst);
    CHECK(!solid_wrong, "\"%s\": %d solid text pixels differ from Chromium's", c->name, solid_wrong);
    /* measured 2026-10-16: 80.3% to 98.7% within a level, 5 at worst (glyph-by-glyph shadows against one capture of the
     * label); over the wallpaper in the prototype's own page the same composition is 86% to 94% within a level, 3 at worst */
    CHECK(within * 100 >= ink * 80 && worst <= 5, "\"%s\": the shadows drifted from Chromium's", c->name);
done:
    free(got);
    free(text);
}

static void check_layout(void)
{
    static const struct { int icon; const char *name; int lines; const char *last; int ellipsis; } cases[] = {
        {PW_DESKTOP_MEDIUM, "This PC", 1, "This PC", 0},
        {PW_DESKTOP_MEDIUM, "Example Game", 2, "Game", 0},
        {PW_DESKTOP_MEDIUM, "Archive (2026-10-08)", 2, "(2026-10-", 1},          /* a line may end after a hyphen */
        {PW_DESKTOP_MEDIUM, "Supercalifragilisticexpialidocious", 2, "gilisticexpi", 1},
        {PW_DESKTOP_MEDIUM, "  Two   spaces  ", 1, "Two spaces", 0},              /* white space collapses as CSS's does */
        {PW_DESKTOP_MEDIUM, "", 1, "", 0},
        {PW_DESKTOP_SMALL, "Example Game", 2, "Game", 0},                        /* small icons keep medium's width */
        {PW_DESKTOP_LARGE, "Example Game", 1, "Example Game", 0},                /* large ones have 115 px of text */
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); ++i)
    {
        PwDesktopGrid grid = pw_desktop_grid(cases[i].icon);
        unsigned short name[64], last[64];
        PwDesktopLabel label;
        int length = wide(cases[i].name, name), ok = pw_desktop_label_layout(&pw_desktop_label_fonts[2], name, length, 2, &grid, &label);
        const PwDesktopLine *line = &label.line[label.lines - 1];

        CHECK(ok && label.lines == cases[i].lines && line->ellipsis == cases[i].ellipsis &&
              line->length == wide(cases[i].last, last) && !memcmp(label.text + line->start, last, line->length * 2),
              "\"%s\": not laid out as Blink does", cases[i].name);
    }
    {
        unsigned short name[2] = {'a', 0x4e00};   /* a character no glyph was measured for */
        PwDesktopGrid grid = pw_desktop_grid(PW_DESKTOP_MEDIUM);
        PwDesktopLabel label;
        CHECK(!pw_desktop_label_layout(&pw_desktop_label_fonts[2], name, 2, 2, &grid, &label), "an unmeasured character is laid out");
    }
}

static void check_grid(void)
{
    /* the prototype's boxes at 1920 x 1080 (measured): second row, second column, icon, label, plates for 1 and 2 lines */
    static const struct { int icon, row1, column1, icon_x, label_y, columns, rows, plate_w, plate1, plate2; } sizes[] = {
        {PW_DESKTOP_SMALL, 77, 77, 21, 37, 25, 13, 73, 57, 73},
        {PW_DESKTOP_MEDIUM, 93, 77, 13, 53, 25, 11, 73, 73, 89},
        {PW_DESKTOP_LARGE, 141, 125, 13, 101, 15, 7, 121, 121, 137},
    };
    for (size_t s = 0; s < sizeof(sizes) / sizeof(*sizes); ++s)
    {
        PwDesktopGrid g = pw_desktop_grid(sizes[s].icon);

        CHECK(pw_desktop_cell_x(&g, 0) == 2 && pw_desktop_cell_y(&g, 0) == 2 && pw_desktop_cell_y(&g, 1) == sizes[s].row1 &&
              pw_desktop_cell_x(&g, 1) == sizes[s].column1 && g.icon_x == sizes[s].icon_x && g.label_y == sizes[s].label_y,
              "%d px icons: the cells, icon or label are not the prototype's", sizes[s].icon);
        CHECK(pw_desktop_columns(&g, 1920) == sizes[s].columns && pw_desktop_rows(&g, 1032) == sizes[s].rows,
              "%d px icons: a 1920 x 1032 work area does not hold %d x %d cells", sizes[s].icon, sizes[s].columns, sizes[s].rows);
        CHECK(pw_desktop_plate_width(&g) == sizes[s].plate_w && pw_desktop_plate_height(&g, 1) == sizes[s].plate1 &&
              pw_desktop_plate_height(&g, 2) == sizes[s].plate2, "%d px icons: the plates are not the prototype's", sizes[s].icon);
        for (int i = -3; i < 30; ++i)
            CHECK(pw_desktop_cell_column(&g, pw_desktop_cell_x(&g, i) + g.cell_w / 2 - 1) == i &&
                  pw_desktop_cell_column(&g, pw_desktop_cell_x(&g, i) - g.cell_w / 2 + 1) == i &&
                  pw_desktop_cell_row(&g, pw_desktop_cell_y(&g, i) + g.cell_h / 2 - 1) == i &&
                  pw_desktop_cell_row(&g, pw_desktop_cell_y(&g, i) - g.cell_h / 2 + 1) == i,
                  "%d px icons: cell %d is not the nearest to points within half a cell", sizes[s].icon, i);
    }
}

static void check_plates(void)
{
    const int side = 2 * PW_DESKTOP_SLICE + 1;
    unsigned *plate = calloc(121 * 169, 4), band[6 * 5];

    for (int p = 0; p < PW_DESKTOP_PLATES; ++p)
        for (int w = 73; w <= 121; w += 48)
            for (int lines = 1; lines <= 4; ++lines)
            {
                int h = (w == 73 ? 57 : 105) + 16 * lines;
                pw_desktop_plate_draw(pw_desktop_plates[p], plate, w, h);
                CHECK(plate[0] == pw_desktop_plates[p][0] && plate[w * h - 1] == pw_desktop_plates[p][side * side - 1] &&
                      plate[(h / 2) * w + w / 2] == pw_desktop_plates[p][side * PW_DESKTOP_SLICE + PW_DESKTOP_SLICE] &&
                      plate[(h - 2) * w + 1] == pw_desktop_plates[p][(side - 2) * side + 1] && !(plate[0] >> 24),
                      "plate %d, %d x %d: not its nine-slice", p, w, h);
            }
    pw_desktop_band_draw(PW_DESKTOP_BAND_BORDER, PW_DESKTOP_BAND_FILL, band, 6, 5);
    CHECK(band[0] == PW_DESKTOP_BAND_BORDER && band[29] == PW_DESKTOP_BAND_BORDER && band[7] == PW_DESKTOP_BAND_FILL &&
          (PW_DESKTOP_BAND_BORDER >> 24) == 255, "the band is not a border round a fill");
    free(plate);
}

int main(void)
{
    for (size_t i = 0; i < sizeof(pw_desktop_label_checks) / sizeof(*pw_desktop_label_checks); ++i) check_label(&pw_desktop_label_checks[i]);
    check_layout();
    check_grid();
    check_plates();
    if (failures) { printf("%d failures\n", failures); return 1; }
    printf("ok\n");
    return 0;
}
