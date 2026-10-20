/* SPDX-License-Identifier: GPL-3.0-or-later
 * Windows 11's popup menus as the approved prototype draws them (prototypes/windows11-shell/css/ui.css .menu, .menu-item,
 * .menu-separator, .menu-commandbar; behaviour js/ui.js), in 96 DPI pixels. Wine's popup menus (#32768, patch 1190 in
 * win32u) lay out and paint every program's menus with this; the compositor draws the material, the 8 px corners, the
 * stroke and the shadow (.acrylic.surface-flyout, PS5_DWMSBT_FLYOUT), the menu paints its items over that glass.
 *
 * Measured (tools/measure-prototype-surface.mjs, open=menu-desktop and menu-desktop-item): the list starts 3 px inside
 * the menu (1 px stroke, 2 px padding); a row is 32 px tall and 8 px narrower than the list (4 px margins); inside it
 * 11 px of padding, then the check column and the icon column (16 px each, when the menu has any) and the label, 12 px
 * apart; a shortcut (12 px text) or a submenu's 12 px chevron ends 11 px from the row's right edge, 24 px + 12 px after
 * the label. A separator is a 1 px line across the whole list with 2 px above and below. Commands without labels (Cut,
 * Copy, Rename, Share, Delete) are 40 x 40 cells in a row 2 px inside the list. The menu is at least 200 px wide and at
 * most 456. The label's glyphs end 20 px into the row (their baseline), a shortcut's 19 px.
 *
 * The fixed pixels (plates, keyboard ring, chevron, check, radio dot, separator) are the prototype's own, generated
 * into pw_menu_sprites.h by tools/build-menu-sprites.py with the types below. Labels and shortcuts are Chromium's own
 * glyphs placed on Skia's grid (src/pw_text_glyphs.h, pw_menu_glyphs.h) when the font holds every character; other text
 * (and disabled or default items) is the glyphs' coverage composed here in Chromium's way (Skia's mask gamma) in the
 * token's colour. tests/test_pw_menu_style.c checks the layout against the prototype's boxes and the composition
 * against its plates and its strings. */
#ifndef PW_MENU_STYLE_H
#define PW_MENU_STYLE_H
#include <math.h>

enum
{
    PW_MENU_INSET = 3,          /* the list's top and left inside the menu: 1 px stroke + 2 px padding (top) */
    PW_MENU_BORDER = 1,
    PW_MENU_ROW = 32,
    PW_MENU_ROW_MARGIN = 4,
    PW_MENU_ROW_PAD = 11,
    PW_MENU_GAP = 12,
    PW_MENU_ICON = 16,
    PW_MENU_TAIL = 24,
    PW_MENU_CHEVRON = 12,
    PW_MENU_SEPARATOR = 5,
    PW_MENU_MIN_WIDTH = 200,
    PW_MENU_MAX_WIDTH = 456,
    PW_MENU_COMMAND = 40,
    PW_MENU_COMMAND_PAD = 2,
    PW_MENU_TEXT = 14,          /* --type-body */
    PW_MENU_SHORTCUT_TEXT = 12, /* --type-caption */
    PW_MENU_LABEL_INK = 20,     /* the label's last row of ink above its baseline, from the row's top */
    PW_MENU_SHORTCUT_INK = 19,
};

/* A premultiplied BGRA picture placed at (x, y) from its anchor. */
typedef struct { int x, y, w, h; const unsigned int *bits; } PwMenuSprite;
/* A plate as wide as its row: the left piece from the row's left edge, its middle column repeated, the right piece from
 * the row's right edge. */
typedef struct { PwMenuSprite left, middle, right; } PwMenuPlate;
typedef struct
{
    PwMenuPlate hover, keyboard;                   /* a row's plates; keyboard adds the focus ring, 2 px outside */
    PwMenuSprite command_hover;                    /* from a command cell's top left */
    PwMenuSprite chevron;                          /* from the row's top right */
    PwMenuSprite check, radio;                     /* from the check column's left and the row's top */
    unsigned int separator;                        /* one premultiplied colour */
} PwMenuChrome;

/* The width a row needs, 16.16 like the text widths it adds (Chromium's are fractional; the menu rounds its widest row
 * once): checks/icons say whether the menu has those columns, label and shortcut are the texts' widths (shortcut 0:
 * none), submenu whether it shows a chevron. */
static inline int pw_menu_row_width(int checks, int icons, int label, int shortcut, int submenu)
{
    int width = 2 * PW_MENU_ROW_PAD;
    if (checks) width += PW_MENU_ICON + PW_MENU_GAP;
    if (icons) width += PW_MENU_ICON + PW_MENU_GAP;
    if (shortcut) width += PW_MENU_GAP + PW_MENU_TAIL;
    if (submenu) width += PW_MENU_GAP + PW_MENU_TAIL + PW_MENU_CHEVRON;
    return width * 65536 + label + shortcut;
}

/* A menu text without its access-key prefixes ("&File" is File, "&&" one "&"), up to 256 characters: its length, and in
 * *key the index of the character the prefix marks (underlined with keyboard cues) or -1. */
static inline int pw_menu_strip(const unsigned short *text, int count, unsigned short *out, int *key)
{
    int length = 0;
    *key = -1;
    for (int i = 0; i < count && length < 256; ++i)
    {
        if (text[i] == '&' && i + 1 < count)
        {
            if (text[++i] != '&' && *key < 0) *key = length;
        }
        out[length++] = text[i];
    }
    return length;
}

static inline int pw_menu_icon_x(int checks)
{
    return PW_MENU_ROW_PAD + (checks ? PW_MENU_ICON + PW_MENU_GAP : 0);
}

static inline int pw_menu_label_x(int checks, int icons)
{
    return pw_menu_icon_x(checks) + (icons ? PW_MENU_ICON + PW_MENU_GAP : 0);
}

/* The menu's width: list is its rows' widths with their margins (columns side by side), commands its command cells.
 * One column stops at the maximum width (labels are cut with an ellipsis); several do not. */
static inline int pw_menu_width(int list, int commands, int columns)
{
    int bar = commands * PW_MENU_COMMAND + 2 * PW_MENU_COMMAND_PAD, width = (list > bar ? list : bar) + 2 * PW_MENU_BORDER;
    if (width < PW_MENU_MIN_WIDTH) width = PW_MENU_MIN_WIDTH;
    if (columns <= 1 && width > PW_MENU_MAX_WIDTH) width = PW_MENU_MAX_WIDTH;
    return width;
}

/* Text colours (--TextFillColorPrimary, Secondary, Disabled) as premultiplied BGRA at full coverage. */
enum { PW_MENU_PRIMARY, PW_MENU_SECONDARY, PW_MENU_DISABLED };
static inline unsigned int pw_menu_text_colour(int dark, int role)
{
    static const unsigned char alpha[2][3] = {{228, 158, 92}, {255, 197, 93}}; /* 0.894, 0.62, 0.361; 1, 0.773, 0.365 */
    unsigned int a = alpha[!!dark][role];
    return a << 24 | (dark ? a << 16 | a << 8 | a : 0);
}

/* Skia's mask gamma as Chromium draws text here (gamma 1.2 quantised to 76/64, contrast 0.2): coverage 0-255 of a
 * glyph to the alpha the blit uses, for black (dark == 0) or white paint. Black: c = a + (1 - a) 0.2 a, then
 * 1 - (1 - c)^(64/76); white: a^(64/76). */
static inline unsigned int pw_menu_text_mask(unsigned int coverage, int white)
{
    static unsigned char table[2][256];
    static int ready;
    if (!ready)
    {
        for (int i = 0; i < 256; ++i)
        {
            float a = i / 255.0f, c = a + (1.0f - a) * 0.2f * a;
            table[0][i] = (unsigned char)((1.0f - powf(1.0f - c, 64.0f / 76.0f)) * 255.0f + 0.5f);
            table[1][i] = (unsigned char)(powf(a, 64.0f / 76.0f) * 255.0f + 0.5f);
        }
        ready = 1;
    }
    return table[!!white][coverage > 255 ? 255 : coverage];
}

/* Premultiplied source over destination, rounded as Skia rounds (x * 255 / 255 with +128 >> 8 twice). */
static inline unsigned int pw_menu_over(unsigned int dst, unsigned int src)
{
    unsigned int inverse = 255 - (src >> 24), out = 0;
    for (int shift = 0; shift < 32; shift += 8)
    {
        unsigned int d = (dst >> shift & 0xff) * inverse + 128;
        out |= ((src >> shift & 0xff) + ((d + (d >> 8)) >> 8)) << shift;
    }
    return out;
}

/* A premultiplied colour scaled by coverage 0-255. */
static inline unsigned int pw_menu_scale(unsigned int colour, unsigned int coverage)
{
    unsigned int out = 0;
    for (int shift = 0; shift < 32; shift += 8)
    {
        unsigned int v = (colour >> shift & 0xff) * coverage + 128;
        out |= ((v + (v >> 8)) >> 8) << shift;
    }
    return out;
}

/* Composes text coverage (8-bit, stride bytes per row) at (x, y) of a top-down premultiplied BGRA buffer in colour,
 * clipped to [0, width) x [0, height). */
static inline void pw_menu_text(unsigned int *bits, int width, int height, int x, int y, const unsigned char *coverage,
                                int cover_width, int cover_height, int stride, unsigned int colour, int white)
{
    for (int j = 0; j < cover_height; ++j)
    {
        if (y + j < 0 || y + j >= height) continue;
        for (int i = 0; i < cover_width; ++i)
        {
            unsigned int c = coverage[j * stride + i];
            unsigned int *pixel = &bits[(y + j) * width + x + i];
            if (!c || x + i < 0 || x + i >= width) continue;
            *pixel = pw_menu_over(*pixel, pw_menu_scale(colour, pw_menu_text_mask(c, white)));
        }
    }
}

/* Draws a sprite at (x, y) over a top-down premultiplied BGRA buffer, clipped. */
static inline void pw_menu_sprite(unsigned int *bits, int width, int height, int x, int y, const PwMenuSprite *sprite)
{
    for (int j = 0; j < sprite->h; ++j)
        for (int i = 0; i < sprite->w; ++i)
        {
            int px = x + sprite->x + i, py = y + sprite->y + j;
            unsigned int s = sprite->bits[j * sprite->w + i];
            if (s && px >= 0 && px < width && py >= 0 && py < height)
                bits[py * width + px] = pw_menu_over(bits[py * width + px], s);
        }
}

/* Draws a plate across the row [left, right) whose top is top. */
static inline void pw_menu_plate(unsigned int *bits, int width, int height, int left, int right, int top,
                                 const PwMenuPlate *plate)
{
    PwMenuSprite column = plate->middle;
    pw_menu_sprite(bits, width, height, left, top, &plate->left);
    for (int x = left + plate->left.x + plate->left.w; x < right + plate->right.x; ++x)
    {
        column.x = x - left;
        pw_menu_sprite(bits, width, height, left, top, &column);
    }
    pw_menu_sprite(bits, width, height, right, top, &plate->right);
}

/* A 1 px separator line across [left, right) at y. */
static inline void pw_menu_separator(unsigned int *bits, int width, int height, int left, int right, int y,
                                     unsigned int colour)
{
    if (y < 0 || y >= height) return;
    for (int x = left < 0 ? 0 : left; x < right && x < width; ++x) bits[y * width + x] = pw_menu_over(bits[y * width + x], colour);
}

#endif
