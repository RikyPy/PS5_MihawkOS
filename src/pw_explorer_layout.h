/* SPDX-License-Identifier: GPL-3.0-or-later
 * File Explorer laid out as the approved prototype lays it out (prototypes/windows11-shell css/apps/explorer.css), in
 * 96 DPI pixels of the client area, which starts inside the window's 1 px border and covers its title bar: a 48 px
 * title bar with the tab strip and the 46 x 48 caption buttons, a 48 px address row (four 32 x 32 buttons, the
 * address box that takes the rest, the 280 px search box), a 48 px command bar whose last pixel row is its divider,
 * then the 165 px navigation pane beside the items and a 26 px status bar.
 *
 * Boxes are the prototype's own, fractions kept: where Chromium places an element at a fraction (the command bar's
 * buttons sit at y 103.5, every one after "New" at x .9), the native Explorer draws that element's captured pixels
 * (tools/measure-explorer-sprites.mjs) from the whole pixel below the fraction, so the fraction stays in the pixels.
 * At the prototype's 1100 x 700 window (client 1098 x 698) every box equals its measured one
 * (tests/test_pw_explorer_layout.c). Wine patch 1150 (wine/ps5/pw_shell_explorer.h) places and hit-tests with it. */
#ifndef PW_EXPLORER_LAYOUT_H
#define PW_EXPLORER_LAYOUT_H

typedef struct { double x, y, w, h; } PwExplorerBox;

enum { PW_EXPLORER_BACK, PW_EXPLORER_FORWARD, PW_EXPLORER_UP, PW_EXPLORER_REFRESH, PW_EXPLORER_NAVIGATION };
enum { PW_EXPLORER_CMD_NEW, PW_EXPLORER_CMD_CUT, PW_EXPLORER_CMD_COPY, PW_EXPLORER_CMD_PASTE, PW_EXPLORER_CMD_RENAME,
       PW_EXPLORER_CMD_SHARE, PW_EXPLORER_CMD_DELETE, PW_EXPLORER_CMD_SORT, PW_EXPLORER_CMD_VIEW, PW_EXPLORER_CMD_FILTER,
       PW_EXPLORER_CMD_MORE, PW_EXPLORER_CMD_DETAILS, PW_EXPLORER_COMMANDS };
enum { PW_EXPLORER_CAPTION_MIN, PW_EXPLORER_CAPTION_MAX, PW_EXPLORER_CAPTION_CLOSE, PW_EXPLORER_CAPTIONS };
/* The navigation pane's rows, top to bottom: Home and Gallery, the six pinned folders, This PC and Network. */
enum { PW_EXPLORER_NAV_ROWS = 10, PW_EXPLORER_TITLE_H = 48, PW_EXPLORER_HEADER_H = 144, PW_EXPLORER_STATUS_H = 26,
       PW_EXPLORER_NAV_W = 165 };

typedef struct
{
    PwExplorerBox caption[PW_EXPLORER_CAPTIONS];
    PwExplorerBox navigation[PW_EXPLORER_NAVIGATION], address, search;
    PwExplorerBox command[PW_EXPLORER_COMMANDS], separator[3], divider;
    PwExplorerBox nav, nav_row[PW_EXPLORER_NAV_ROWS], nav_separator[2], content, details, status, status_details, status_large;
} PwExplorerPlace;

static inline PwExplorerBox pw_explorer_box(double x, double y, double w, double h)
{
    PwExplorerBox b = {x, y, w, h};
    return b;
}

/* width, height: the client area; filter: the command bar has Filter (a folder; Home, This PC, Gallery and Network
 * have none), which comes after View and is zero wide otherwise; details: the details pane is open (280 px at the
 * items area's right, which narrows; zero wide otherwise). */
static inline void pw_explorer_place(PwExplorerPlace *p, int width, int height, int filter, int details)
{
    /* Command widths: 10 px padding round a 16 px icon, 8 px gaps, Selawik 12 px labels at Chromium's advances
     * ("New" 23.9, "Sort" 21.7, "View" 25.3, "Filter" 26.2, "Details" 35.8), the 12 px chevron 2 px in and 8 px of
     * padding after it. */
    static const double command_w[PW_EXPLORER_COMMANDS] = {83.9, 40, 40, 40, 40, 40, 40, 81.7, 85.3, 86.19, 40, 79.8};
    double x = 8, address_w = width - 8 - 12 - 4 * 32 - 5 * 4 - 4 - 4 - 280;
    int i;
    if (address_w < 72) address_w = 72;
    for (i = 0; i < PW_EXPLORER_CAPTIONS; ++i) p->caption[i] = pw_explorer_box(width - 138 + 46 * i, 0, 46, 48);
    for (i = 0; i < PW_EXPLORER_NAVIGATION; ++i) p->navigation[i] = pw_explorer_box(8 + 36 * i, 56, 32, 32);
    p->address = pw_explorer_box(156, 56, address_w, 32);
    p->search = pw_explorer_box(156 + address_w + 8, 56, 280, 32);
    /* New | Cut Copy Paste Rename Share Delete | Sort View | See more, then a spacer and Details at the right. */
    for (i = 0; i <= PW_EXPLORER_CMD_MORE; ++i)
    {
        if (i == PW_EXPLORER_CMD_FILTER && !filter) { p->command[i] = pw_explorer_box(x, 103.5, 0, 32); continue; }
        if (i == PW_EXPLORER_CMD_CUT || i == PW_EXPLORER_CMD_SORT || i == PW_EXPLORER_CMD_MORE)
        {
            p->separator[i == PW_EXPLORER_CMD_CUT ? 0 : i == PW_EXPLORER_CMD_SORT ? 1 : 2] = pw_explorer_box(x + 4, 103.5 + 4, 1, 24);
            x += 9;
        }
        p->command[i] = pw_explorer_box(x, 103.5, command_w[i], 32);
        x += command_w[i];
    }
    p->command[PW_EXPLORER_CMD_DETAILS] = pw_explorer_box(width - 12 - command_w[PW_EXPLORER_CMD_DETAILS], 103.5,
                                                          command_w[PW_EXPLORER_CMD_DETAILS], 32);
    p->divider = pw_explorer_box(0, PW_EXPLORER_HEADER_H - 1, width, 1);
    p->nav = pw_explorer_box(0, PW_EXPLORER_HEADER_H, PW_EXPLORER_NAV_W, height - PW_EXPLORER_HEADER_H - PW_EXPLORER_STATUS_H);
    /* 4 px of padding, 32 px rows; a separator is a 1 px line with 8 px above and below, inset 8 px. */
    for (i = 0; i < PW_EXPLORER_NAV_ROWS; ++i)
        p->nav_row[i] = pw_explorer_box(4, PW_EXPLORER_HEADER_H + 4 + 32 * i + 17 * (i >= 2) + 17 * (i >= 8), 147, 32);
    p->nav_separator[0] = pw_explorer_box(12, p->nav_row[1].y + 32 + 8, 131, 1);
    p->nav_separator[1] = pw_explorer_box(12, p->nav_row[7].y + 32 + 8, 131, 1);
    p->content = pw_explorer_box(PW_EXPLORER_NAV_W, PW_EXPLORER_HEADER_H, width - PW_EXPLORER_NAV_W - (details ? 280 : 0),
                                 height - PW_EXPLORER_HEADER_H - PW_EXPLORER_STATUS_H);
    p->details = pw_explorer_box(width - (details ? 280 : 0), PW_EXPLORER_HEADER_H, details ? 280 : 0, p->content.h);
    p->status = pw_explorer_box(0, height - PW_EXPLORER_STATUS_H, width, PW_EXPLORER_STATUS_H);
    p->status_details = pw_explorer_box(width - 60, height - 24, 28, 22);
    p->status_large = pw_explorer_box(width - 32, height - 24, 28, 22);
}

/* The details pane's content, relative to the pane (a 1 px divider at its left, 20 px and 16 px of padding): the 96 px
 * icon centred (at a half pixel), 12 px below it the name (600 14/20 px, name_lines lines), then 6 px and the type
 * (below_lines of 16 px; hint: the hint's 12 px further down; 0: neither), 16 px, then when there are rows the "Details"
 * title (a divider, 12 px, 20 px, 8 px) and each row (row_lines of 16 px, 8 px apart): its key in a column key_w wide
 * (the widest key, Chromium's: "Date modified" 75.5, "Total file size" 68.52), 12 px, its value. */
enum { PW_EXPLORER_DETAIL_ROWS = 4 };
typedef struct { PwExplorerBox icon, name, below, title, key[PW_EXPLORER_DETAIL_ROWS], value[PW_EXPLORER_DETAIL_ROWS]; } PwExplorerDetails;
static inline void pw_explorer_details(PwExplorerDetails *d, int name_lines, int below_lines, int hint, int rows,
                                       const int *row_lines, double key_w)
{
    double y = 20 + 96 + 12;
    int i;
    d->icon = pw_explorer_box(17 + (247 - 96) / 2.0, 20, 96, 96);
    d->name = pw_explorer_box(17, y, 247, 20.0 * name_lines);
    y += 20.0 * name_lines;
    if (below_lines) y += 6 + (hint ? 12 : 0);
    d->below = pw_explorer_box(17, y, 247, 16.0 * below_lines);
    y += 16.0 * below_lines + 16;
    d->title = pw_explorer_box(17, y, 247, rows ? 41 : 0);
    y += 41;
    for (i = 0; i < rows && i < PW_EXPLORER_DETAIL_ROWS; ++i)
    {
        d->key[i] = pw_explorer_box(17, y, key_w, 16.0 * row_lines[i]);
        d->value[i] = pw_explorer_box(17 + key_w + 12, y, 247 - key_w - 12, 16.0 * row_lines[i]);
        y += 16.0 * row_lines[i] + 8;
    }
}

/* The title bar's tabs (WinUI's TabView, TabWidthMode Equal): count tabs of one width from 8 px in, 240 px until the
 * strip is full (the client less the caption buttons, 40 px of drag region, the + button and the list's 8 px padding
 * either side), then shrinking to 88 px, past which the strip clips them; each tab's close button 37 px in from its
 * right; the + button 4 px into the strip's end. */
enum { PW_EXPLORER_TABS_MAX = 20 };
static inline void pw_explorer_tab_strip(PwExplorerBox *tab, PwExplorerBox *close, PwExplorerBox *add, int count, int width)
{
    double room = width - 138 - 40 - 28 - 16, w = count > 0 ? room / count : 240, strip;
    int i;
    if (w > 240) w = 240;
    if (w < 88) w = 88;
    for (i = 0; i < count; ++i)
    {
        tab[i] = pw_explorer_box(8 + w * i, 8, w, 40);
        close[i] = pw_explorer_box(8 + w * (i + 1) - 37, 16.5, 32, 24);
    }
    strip = 8 + w * count + 8;
    if (strip > room + 16) strip = room + 16;
    *add = pw_explorer_box(strip - 4, 14, 32, 28);
}

/* The Home page inside the items area (its client origin at the area's top left, before scrolling): the Quick access,
 * Favorites and Recent sections, each a 32 px head over its body. Quick access tiles fill columns of at least 196 px
 * with 4 px gaps; the Recent list is a 32 px column header 2 px above 32 px rows of Name 280, Date modified 170 and
 * Location 220. A collapsed section shows its head alone. */
enum { PW_EXPLORER_HOME_TILES = 6, PW_EXPLORER_SECTIONS = 3 };
typedef struct
{
    PwExplorerBox head[PW_EXPLORER_SECTIONS], tile[PW_EXPLORER_HOME_TILES], favorites_note, recent_head, recent_note;
    double recent_rows_y; /* the first row's top; rows are 32 px apart and 670 px wide at recent_head.x */
    double height;        /* what scrolls: the page and the items area's 12 px of bottom padding */
} PwExplorerHome;

/* width: the items area's width; collapsed: bit n collapses section n; recent: rows in the Recent list. */
static inline void pw_explorer_home(PwExplorerHome *p, int width, unsigned collapsed, int recent)
{
    double x = 12, y = 4 + 4, inner = width - 12 - 8 - 4 - 4, tile_w;
    int columns = (int)((inner + 4) / (196 + 4)), i;
    if (columns < 1) columns = 1;
    tile_w = (inner - 4.0 * (columns - 1)) / columns;
    p->head[0] = pw_explorer_box(x, y, 105.2, 32);
    for (i = 0; i < PW_EXPLORER_HOME_TILES; ++i)
        p->tile[i] = pw_explorer_box(x + 4 + (tile_w + 4) * (i % columns), y + 32 + 4 + 76 * (i / columns), tile_w, 72);
    if (!(collapsed & 1)) y += 32 + 4 + 76 * ((PW_EXPLORER_HOME_TILES + columns - 1) / columns) - 4 + 8;
    else y += 32;
    y += 4;
    p->head[1] = pw_explorer_box(x, y, 85.3, 32);
    p->favorites_note = pw_explorer_box(x + 4, y + 32 + 4, inner, 36);
    y += 32 + ((collapsed & 2) ? 0 : 4 + 36 + 8) + 4;
    p->head[2] = pw_explorer_box(x, y, 74.1, 32);
    p->recent_head = pw_explorer_box(x + 4, y + 32 + 4, 670, 32);
    p->recent_rows_y = y + 32 + 4 + 32 + 2;
    /* No recent file: the note stands where the rows would, 16 px of padding round one line. */
    p->recent_note = pw_explorer_box(x + 4, p->recent_rows_y, inner, 48);
    y += 32;
    if (!(collapsed & 4)) y += 4 + 32 + 2 + (recent ? 32.0 * recent : 48) + 4 + 8;
    p->height = y + 4 + 8 + 12;
}

/* A folder's items in an icon view, as the prototype's .ex-grid lays them out, relative to the items area's content
 * box (12 px in from its left, at its top; width and height that box's, less the 8 px right and 12 px bottom padding):
 *   Large icons  116 px tiles in columns 118 px apart, from 8 px down; a row is as tall as its tallest tile (6 px of
 *                padding, the 96 px icon, 4 px, 16 px a line of the name, 6 px), 2 px between rows.
 *   Small icons  240 x 32 tiles in as many 244 px columns as 240 px tracks allow, row by row from 8 px down.
 *   List         32 px rows down columns of at least 220 px (as many as fit with 4 px gaps, sharing the width),
 *                from 4 px down, as many rows as the column's height holds (the box less 4, less 4 + 4 of padding). */
enum { PW_EXPLORER_VIEW_LARGE, PW_EXPLORER_VIEW_SMALL, PW_EXPLORER_VIEW_LIST };
/* lines: each tile's name lines for large icons (1 to 4; NULL elsewhere). */
static inline void pw_explorer_tiles(PwExplorerBox *tile, int count, int view, double width, double height, const int *lines)
{
    int i, start, columns, rows;
    if (view == PW_EXPLORER_VIEW_LARGE)
    {
        double y = 8, tallest;
        columns = (int)((width + 2) / 118);
        if (columns < 1) columns = 1;
        for (start = 0; start < count; start += columns, y += tallest + 2)
        {
            for (tallest = 0, i = start; i < count && i < start + columns; ++i)
                if (112 + 16.0 * lines[i] > tallest) tallest = 112 + 16.0 * lines[i];
            for (i = start; i < count && i < start + columns; ++i) tile[i] = pw_explorer_box(118.0 * (i - start), y, 116, tallest);
        }
    }
    else if (view == PW_EXPLORER_VIEW_SMALL)
    {
        double w = width >= 240 ? 240 : width > 200 ? width : 200;
        columns = (int)((width + 4) / 244);
        if (columns < 1) columns = 1;
        for (i = 0; i < count; ++i) tile[i] = pw_explorer_box((w + 4) * (i % columns), 8 + 32.0 * (i / columns), w, 32);
    }
    else
    {
        double w;
        columns = (int)((width + 4) / 224);
        if (columns < 1) columns = 1;
        w = (width + 4) / columns - 4;
        rows = (int)((height - 12) / 32);
        if (rows < 1) rows = 1;
        for (i = 0; i < count; ++i) tile[i] = pw_explorer_box((w + 4) * (i / rows), 4 + 32.0 * (i % rows), w, 32);
    }
}

#endif
