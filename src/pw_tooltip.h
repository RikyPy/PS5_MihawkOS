/* SPDX-License-Identifier: GPL-3.0-or-later
 * Tooltips as the approved prototype draws them (prototypes/windows11-shell css/ui.css .tooltip, js/ui.js), measured at
 * 1920 x 1080 in a headless Chromium 154 (tools/build-tooltip-glyphs.py says how):
 *
 *   surface  the flyout's acrylic (the menus' material, PS5_DWMSBT_FLYOUT), a 1 px SurfaceStrokeColorFlyout stroke,
 *            4 px corners and --shadow-tooltip (--shadow-8: 0 0 2px and 0 4px 8px, black at 12% and 14%, 24% and 28% in
 *            dark): the compositor draws all of it (PW_SHADOW_TOOLTIP); the tooltip draws only its text.
 *   text     12 px caption on 16 px lines (baseline 12) in TextFillColorPrimary (black at 89.4%, white in dark),
 *            5 px below the stroke and 8 px in from it, 7 px above the bottom one; white space as CSS pre-line (a
 *            newline breaks, runs of spaces collapse), broken as Blink breaks it (after spaces and hyphens, a word wider
 *            than a line anywhere) at most 302 px wide, so the box is at most 320 px wide.
 *   box      the widest line rounded up to 1/64 px (Blink's unit), plus 18, rounded to a whole pixel as the page lays
 *            it out; or 320 when a line was broken.
 *   timing   shown 500 ms after the pointer comes to rest over its item (100 ms within 400 ms of another tip closing),
 *            hidden on a press, a key, the wheel or leaving the item.
 *   place    its top left at the pointer, 20 px below it, or above the pointer when there is more room there, kept 8 px
 *            inside the work area (ui.position 'bottom-start', gap 0, margin 8).
 *
 * Composed from the prototype's glyphs, the text of three tooltips equals Chromium's capture in every pixel but where
 * two glyphs' boxes overlap (one level off), light and dark (tests/test_pw_tooltip.c).
 *
 * Header-only, no allocation: comctl32's tooltips (Wine patch 1210) and the host test use it. */
#ifndef PW_TOOLTIP_H
#define PW_TOOLTIP_H

#include "pw_text_glyphs.h"

enum
{
    PW_TOOLTIP_BORDER = 1, PW_TOOLTIP_PAD_X = 8, PW_TOOLTIP_PAD_TOP = 5, PW_TOOLTIP_PAD_BOTTOM = 7,
    PW_TOOLTIP_LINE = 16, PW_TOOLTIP_MAX_WIDTH = 320, PW_TOOLTIP_RADIUS = 4,
    PW_TOOLTIP_DELAY = 500, PW_TOOLTIP_RESHOW = 100, PW_TOOLTIP_RESHOW_WITHIN = 400,   /* milliseconds */
    PW_TOOLTIP_BELOW = 20, PW_TOOLTIP_MARGIN = 8,
    PW_TOOLTIP_TEXT_MAX = 1024, PW_TOOLTIP_LINES_MAX = 64,
};

typedef struct { int start, length; } PwTooltipLine;

typedef struct
{
    unsigned short text[PW_TOOLTIP_TEXT_MAX];   /* the tip with its white space collapsed as pre-line does */
    PwTooltipLine line[PW_TOOLTIP_LINES_MAX];
    int lines;
    int width, height;                          /* the box, whole pixels */
} PwTooltipLayout;

/* The text's room: the box's widest less the stroke and padding, in 1/64 px. */
static inline int pw_tooltip_text_room(void)
{
    return (PW_TOOLTIP_MAX_WIDTH - 2 * (PW_TOOLTIP_BORDER + PW_TOOLTIP_PAD_X)) * 64;
}

/* Lays the tip out. FALSE when a character has no glyph: the caller draws that tip its own way. */
static inline int pw_tooltip_layout(const PwGlyphFont *font, const unsigned short *tip, int length, PwTooltipLayout *layout)
{
    const int room = pw_tooltip_text_room();
    int n = 0, widest = 0, broken = 0, start, end;

    /* pre-line: a newline stays, a run of spaces (and tabs) becomes one, spaces around a newline go */
    for (int i = 0; i < length && n < PW_TOOLTIP_TEXT_MAX; ++i)
    {
        unsigned short c = tip[i] == '\t' ? ' ' : tip[i];
        if (c == '\r') continue;
        if (c == ' ' && (!n || layout->text[n - 1] == ' ' || layout->text[n - 1] == '\n')) continue;
        if (c == '\n' && n && layout->text[n - 1] == ' ') --n;
        if (c != '\n' && !pw_text_glyph(font, c)) return 0;
        layout->text[n++] = c;
    }
    while (n && layout->text[n - 1] == ' ') --n;
    layout->lines = 0;
    for (start = 0; start <= n && layout->lines < PW_TOOLTIP_LINES_MAX; start = end + 1)
    {
        int paragraph_end = start;

        while (paragraph_end < n && layout->text[paragraph_end] != '\n') ++paragraph_end;
        end = paragraph_end;
        if (start == paragraph_end)
            layout->line[layout->lines++] = (PwTooltipLine){start, 0};
        for (int s = start; s < paragraph_end && layout->lines < PW_TOOLTIP_LINES_MAX;)
        {
            int e = pw_text_break(font, layout->text, s, paragraph_end, room), w = pw_text_layout_width(font, layout->text + s, e - s);

            if (e < paragraph_end) broken = 1;
            if (w > widest) widest = w;
            layout->line[layout->lines++] = (PwTooltipLine){s, e - s};
            s = e < paragraph_end && layout->text[e] == ' ' ? e + 1 : e;
        }
        if (end >= n) break;
    }
    /* the page rounds its box (offsetWidth) to a whole pixel */
    layout->width = broken ? PW_TOOLTIP_MAX_WIDTH : (widest + 2 * (PW_TOOLTIP_BORDER + PW_TOOLTIP_PAD_X) * 64 + 32) >> 6;
    layout->height = PW_TOOLTIP_LINE * layout->lines + 2 * PW_TOOLTIP_BORDER + PW_TOOLTIP_PAD_TOP + PW_TOOLTIP_PAD_BOTTOM;
    return 1;
}

/* Composes the tip's text into a premultiplied BGRA buffer (width x height, top-down) whose box has its top left at (x, y). */
static inline void pw_tooltip_draw(const PwGlyphFont *font, const PwTooltipLayout *layout, int x, int y, unsigned int *bgra,
                                   int width, int height)
{
    for (int i = 0; i < layout->lines; ++i)
        pw_text_draw(font, layout->text + layout->line[i].start, layout->line[i].length,
                     (x + PW_TOOLTIP_BORDER + PW_TOOLTIP_PAD_X) * 65536,
                     y + PW_TOOLTIP_BORDER + PW_TOOLTIP_PAD_TOP + PW_TOOLTIP_LINE * i + font->baseline, bgra, width, height);
}

/* Where a tip of width x height goes for the pointer at (px, py): its top left. The work area is left..right,
 * top..bottom (the taskbar below it). */
static inline void pw_tooltip_place(int px, int py, int width, int height, int left, int top, int right, int bottom,
                                    int *x, int *y)
{
    const int min_x = left + PW_TOOLTIP_MARGIN, max_x = right - PW_TOOLTIP_MARGIN, min_y = top + PW_TOOLTIP_MARGIN,
              max_y = bottom - PW_TOOLTIP_MARGIN;
    const int room_below = max_y - (py + PW_TOOLTIP_BELOW), room_above = py - min_y;

    *y = room_below < height && room_above > room_below ? py - height : py + PW_TOOLTIP_BELOW;
    *x = px + width <= max_x || px - width < min_x ? px : px - width;
    if (*x > max_x - width) *x = max_x - width;
    if (*x < min_x) *x = min_x;
    if (*y > max_y - height) *y = max_y - height;
    if (*y < min_y) *y = min_y;
}

#endif
