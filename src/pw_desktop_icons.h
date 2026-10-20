/* SPDX-License-Identifier: GPL-3.0-or-later
 * The desktop's icons as the approved prototype draws them (prototypes/windows11-shell css/desktop.css, js/desktop.js),
 * measured at 1920 x 1080 in a headless Chromium 154 (tools/build-desktop-sprites.py says how), at each of its three icon
 * sizes, I = 32 (small), 48 (medium, the default) and 96 (large):
 *
 *   grid   cells of max(75, I + 27) x (I + 43) (the 75 px --desktop-grid, rows grown by the icon's height past 32) from
 *          1 px inside the work area: 75 x 75, 75 x 91, 123 x 139. An item's box, its plate, is 1 px inside its cell, the
 *          cell's width less 2 wide and I + 9 + 16 per label line tall, radius 4.
 *   icon   at +((cell width - I) / 2), +3 of the box: 21, 13, 13 (Chromium draws the 20.5 and 12.5 it centres at a whole
 *          pixel up).
 *   label  white 12 px text on 16 px lines (baseline 12) under two shadows, 1px 1px 2px black at 0.7 and 0 0 2px at 0.45,
 *          in a box at +(I + 5) that is the text's width plus 1 px each side (at most the cell's width less 6, centred),
 *          broken as Blink breaks it (after spaces and hyphens, a word wider than a line anywhere), clamped to 2 lines
 *          with "…" unless the item is selected and focused; a clamped label clips its shadows to its pixel-snapped box.
 *
 * What is measured, not modelled: the plates (nine-slices of their captures, equal at every size), the band's two colours and every
 * glyph of the label in each of its three layers (src/pw_text_glyphs.h, alpha only). The layout is Blink's: widths are
 * glyph advances summed in 16.16, then rounded up to Blink's 1/64 px unit; a one-line label is centred by
 * (box - width - 2) / 2 truncated to 1/64 px, each line of a longer one by (box - 2 - width) / 2; a clamped last line is centred
 * by its width before it was cut short for the ellipsis, which then hangs after it. Composed this way, the label's text
 * equals Chromium's capture in every pixel and each shadow layer is within a level (tests/test_pw_desktop_icons.c).
 * Chromium's own page draws the faintest shadow pixels up to 3 levels darker than any premultiplied blend of them: that
 * remainder is Chromium's rasterisation, not this layout.
 *
 * Header-only, no allocation: comctl32's list view draws the desktop with it (Wine patch 1105), shell32 places icons on
 * its grid and switches its size and arrangement (patch 1102), and the host test checks it. */
#ifndef PW_DESKTOP_ICONS_H
#define PW_DESKTOP_ICONS_H

#include "pw_text_glyphs.h"

enum
{
    PW_DESKTOP_SMALL = 32, PW_DESKTOP_MEDIUM = 48, PW_DESKTOP_LARGE = 96,   /* the icon sizes, in pixels */
    PW_DESKTOP_ICON_Y = 3, PW_DESKTOP_LINE = 16,
    PW_DESKTOP_CLAMP = 2,          /* lines a label shows unless its item is selected and focused */
    PW_DESKTOP_SLICE = 4,          /* a plate's corner: then its edge and fill are uniform */
    PW_DESKTOP_TEXT_MAX = 260,     /* MAX_PATH: a longer name is cut */
    PW_DESKTOP_LINES_MAX = 32,
};
#define PW_DESKTOP_ELLIPSIS 0x2026

/* The grid for an icon size: its cell, where the icon sits in the plate, the label's top and its box's widest. */
typedef struct { int icon, cell_w, cell_h, icon_x, label_y, label_w; } PwDesktopGrid;

static inline PwDesktopGrid pw_desktop_grid(int icon)
{
    PwDesktopGrid grid;

    grid.icon = icon;
    grid.cell_w = icon + 27 > 75 ? icon + 27 : 75;
    grid.cell_h = icon + 43;
    grid.icon_x = (grid.cell_w - icon) / 2;
    grid.label_y = icon + 5;
    grid.label_w = grid.cell_w - 6;
    return grid;
}

/* The plate's top left for a cell: the first cell is 1 px inside the work area, the plate 1 px inside its cell. */
static inline int pw_desktop_cell_x(const PwDesktopGrid *grid, int column) { return 2 + grid->cell_w * column; }
static inline int pw_desktop_cell_y(const PwDesktopGrid *grid, int row) { return 2 + grid->cell_h * row; }
/* The nearest cell to a plate's top left (negative and off-grid positions round to the nearest). */
static inline int pw_desktop_nearest(int offset, int pitch) { int d = offset + pitch / 2; return (d < 0 ? d - pitch + 1 : d) / pitch; }
static inline int pw_desktop_cell_column(const PwDesktopGrid *grid, int x) { return pw_desktop_nearest(x - 2, grid->cell_w); }
static inline int pw_desktop_cell_row(const PwDesktopGrid *grid, int y) { return pw_desktop_nearest(y - 2, grid->cell_h); }
/* Cells that fit a work area of width x height (the prototype's metrics(): 1 px margins). */
static inline int pw_desktop_columns(const PwDesktopGrid *grid, int width) { int n = (width - 2) / grid->cell_w; return n > 0 ? n : 1; }
static inline int pw_desktop_rows(const PwDesktopGrid *grid, int height) { int n = (height - 2) / grid->cell_h; return n > 0 ? n : 1; }
static inline int pw_desktop_plate_width(const PwDesktopGrid *grid) { return grid->cell_w - 2; }
static inline int pw_desktop_plate_height(const PwDesktopGrid *grid, int lines)
{
    return grid->icon + 9 + PW_DESKTOP_LINE * (lines > 0 ? lines : 1);
}

typedef struct
{
    int start, length;   /* in the label's text */
    int center;          /* the width the line is centred by, 1/64 px: its own, or before it was cut for the ellipsis */
    int ellipsis;        /* "…" follows it */
} PwDesktopLine;

typedef struct
{
    unsigned short text[PW_DESKTOP_TEXT_MAX];   /* the name with its white space collapsed, as CSS does */
    PwDesktopLine line[PW_DESKTOP_LINES_MAX];
    int lines;
    int left, right;                            /* the label's box, 1/64 px from the plate's left */
    int top, text_w;                            /* the box's top from the plate's, and its text's widest (1/64 px) */
} PwDesktopLabel;

/* Blink's width of a run: its advances summed, rounded up to 1/64 px. -1 when a character has no glyph. */
static inline int pw_desktop_width(const PwGlyphFont *font, const unsigned short *text, int length)
{
    int width = pw_text_width(font, text, length);
    return width < 0 ? -1 : (width + 1023) >> 10;
}

/* Lays the name out as the prototype's label: at most max_lines lines (the last one clamped with "…"), or every line when
 * max_lines is 0. FALSE when a character, or the ellipsis, has no glyph: the caller draws that name its own way. */
static inline int pw_desktop_label_layout(const PwGlyphFont *font, const unsigned short *name, int length, int max_lines,
                                          const PwDesktopGrid *grid, PwDesktopLabel *label)
{
    const unsigned short ellipsis = PW_DESKTOP_ELLIPSIS;
    const int available = (grid->label_w - 2) * 64;
    int n = 0, start = 0, width;

    for (int i = 0; i < length && n < PW_DESKTOP_TEXT_MAX; ++i)
    {
        if (name[i] == ' ' && (!n || label->text[n - 1] == ' ')) continue;
        if (!pw_text_glyph(font, name[i])) return 0;
        label->text[n++] = name[i];
    }
    while (n && label->text[n - 1] == ' ') --n;
    if (!pw_text_glyph(font, ellipsis)) return 0;
    label->lines = 0;
    while (start < n)
    {
        PwDesktopLine *line;
        int end = -1;

        if (label->lines == PW_DESKTOP_LINES_MAX || (max_lines && label->lines == max_lines))
        {
            /* more text than lines: the last line is cut until "…" fits after it, and keeps its place */
            line = &label->line[label->lines - 1];
            while (line->length > 1 &&
                   pw_desktop_width(font, label->text + line->start, line->length) +
                   pw_desktop_width(font, &ellipsis, 1) > available)
                --line->length;
            line->ellipsis = 1;
            break;
        }
        /* the widest break opportunity that fits: before a space, after a hyphen, or the end */
        for (int p = start + 1; p <= n; ++p)
        {
            if (p < n && label->text[p] != ' ' && label->text[p - 1] != '-') continue;
            if (pw_desktop_width(font, label->text + start, p - start) > available) break;
            end = p;
        }
        /* a word wider than a line breaks anywhere (overflow-wrap: anywhere), one character at least */
        if (end < 0)
            for (end = start + 1; end < n && pw_desktop_width(font, label->text + start, end + 1 - start) <= available; ++end) {}
        width = pw_desktop_width(font, label->text + start, end - start);
        line = &label->line[label->lines++];
        line->start = start;
        line->length = end - start;
        line->center = width;
        line->ellipsis = 0;
        start = end < n && label->text[end] == ' ' ? end + 1 : end;
    }
    if (!label->lines) label->line[label->lines++] = (PwDesktopLine){0, 0, 0, 0};
    label->top = grid->label_y;
    label->text_w = available;
    if (label->lines == 1)
    {
        width = label->line[0].center + 128;
        label->left = 2 * 64 + (grid->label_w * 64 - width) / 2;
        label->right = label->left + width;
    }
    else
    {
        label->left = 2 * 64;
        label->right = (2 + grid->label_w) * 64;
    }
    return 1;
}

/* Where a line's text starts, 1/64 px from the plate's left. */
static inline int pw_desktop_line_x(const PwDesktopLabel *label, int index)
{
    if (label->lines == 1) return label->left + 64;
    return 3 * 64 + (label->text_w - label->line[index].center) / 2;
}

/* Composes the label into a premultiplied BGRA buffer (width x height, top-down) whose plate has its top left at (x, y):
 * layers[0] the 0 0 2px shadow, [1] the 1px 1px 2px shadow, [2] the text, each glyph over the last. A clamped label clips
 * all three to its box snapped to whole pixels (CSS overflow: hidden); the buffer should start out transparent there. */
static inline void pw_desktop_label_draw(const PwGlyphFont *const layers[3], const PwDesktopLabel *label, int clamped,
                                         int x, int y, unsigned int *bgra, int width, int height)
{
    const unsigned short ellipsis = PW_DESKTOP_ELLIPSIS;

    for (int layer = 0; layer < 3; ++layer)
        for (int i = 0; i < label->lines; ++i)
        {
            const PwDesktopLine *line = &label->line[i];
            int pen = (x * 64 + pw_desktop_line_x(label, i)) * 1024;
            int baseline = y + label->top + PW_DESKTOP_LINE * i + layers[layer]->baseline;

            pw_text_draw(layers[layer], label->text + line->start, line->length, pen, baseline, bgra, width, height);
            if (line->ellipsis)
                pw_text_draw(layers[layer], &ellipsis, 1, pen + pw_text_width(layers[layer], label->text + line->start, line->length),
                             baseline, bgra, width, height);
        }
    if (!clamped) return;
    {
        int left = x + ((label->left + 32) >> 6), right = x + ((label->right + 32) >> 6);
        int top = y + label->top, bottom = top + PW_DESKTOP_LINE * label->lines;

        for (int row = 0; row < height; ++row)
            for (int column = 0; column < width; ++column)
                if (row < top || row >= bottom || column < left || column >= right) bgra[row * width + column] = 0;
    }
}

/* A plate of width x height from its nine-slice: a (2 * PW_DESKTOP_SLICE + 1) square of premultiplied BGRA whose corners
 * are the measured ones and whose middle row and column repeat. */
static inline void pw_desktop_plate_draw(const unsigned int *slice, unsigned int *bgra, int width, int height)
{
    const int s = PW_DESKTOP_SLICE, side = 2 * PW_DESKTOP_SLICE + 1;

    for (int row = 0; row < height; ++row)
    {
        int sy = row < s ? row : row >= height - s ? side - (height - row) : s;
        for (int column = 0; column < width; ++column)
        {
            int sx = column < s ? column : column >= width - s ? side - (width - column) : s;
            bgra[row * width + column] = slice[sy * side + sx];
        }
    }
}

/* The rubber band: a 1 px border and a fill, premultiplied BGRA, no rounding. */
static inline void pw_desktop_band_draw(unsigned int border, unsigned int fill, unsigned int *bgra, int width, int height)
{
    for (int row = 0; row < height; ++row)
        for (int column = 0; column < width; ++column)
            bgra[row * width + column] = !row || !column || row == height - 1 || column == width - 1 ? border : fill;
}

#endif
