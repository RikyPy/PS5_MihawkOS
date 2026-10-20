/* SPDX-License-Identifier: GPL-3.0-or-later
 * MihawkBrowser's chrome tokens, plates and flyout geometry (pw_browser_chrome.h). */
#include "pw_browser_chrome.h"

#define RGBA(r, g, b, a) {(r), (g), (b), (a)}

/* The prototype's tokens (prototypes/windows11-shell/css/tokens.css). Where Chromium's blending rounds otherwise, the
 * alpha is the one that gives the pixel the reference measured: light ControlFillColorDefault is 0.7 in CSS but drew 252
 * over 243 and 254 over the flyout's 252 (0.75); four dark strokes and fills are a few thousandths off theirs. */
static const PwChromeTheme themes[2] = {
    {   /* light */
        .surface = RGBA(243, 243, 243, 1), .text = RGBA(0, 0, 0, 0.894f), .text_secondary = RGBA(0, 0, 0, 0.62f),
        .accent = RGBA(0, 95, 184, 1), .on_accent = RGBA(255, 255, 255, 1),
        .control = RGBA(255, 255, 255, 0.75f), .control_hover = RGBA(249, 249, 249, 0.5f),
        .control_pressed = RGBA(249, 249, 249, 0.3f),
        .control_stroke = RGBA(0, 0, 0, 0.06f), .control_stroke_bottom = RGBA(0, 0, 0, 0.16f),
        .input_stroke = RGBA(0, 0, 0, 0.06f), .input_stroke_bottom = RGBA(0, 0, 0, 0.447f),
        .input_focused = RGBA(255, 255, 255, 1),
        .accent_hover = RGBA(0, 95, 184, 0.9f), .accent_stroke = RGBA(255, 255, 255, 0.08f),
        .accent_stroke_bottom = RGBA(0, 0, 0, 0.4f),
        .subtle_hover = RGBA(0, 0, 0, 0.0373f), .subtle_pressed = RGBA(0, 0, 0, 0.0241f),
        .progress_track = RGBA(0, 0, 0, 0.447f), .divider = RGBA(0, 0, 0, 0.06f),
        .flyout = RGBA(252, 252, 252, 1),
    },
    {   /* dark */
        .surface = RGBA(32, 32, 32, 1), .text = RGBA(255, 255, 255, 1), .text_secondary = RGBA(255, 255, 255, 0.773f),
        .accent = RGBA(96, 205, 255, 1), .on_accent = RGBA(0, 0, 0, 1),
        .control = RGBA(255, 255, 255, 0.06f), .control_hover = RGBA(255, 255, 255, 0.081f),
        .control_pressed = RGBA(255, 255, 255, 0.0326f),
        .control_stroke = RGBA(255, 255, 255, 0.094f), .control_stroke_bottom = RGBA(255, 255, 255, 0.067f),
        .input_stroke = RGBA(255, 255, 255, 0.067f), .input_stroke_bottom = RGBA(255, 255, 255, 0.543f),
        .input_focused = RGBA(30, 30, 30, 1),
        .accent_hover = RGBA(96, 205, 255, 0.9f), .accent_stroke = RGBA(255, 255, 255, 0.08f),
        .accent_stroke_bottom = RGBA(0, 0, 0, 0.137f),
        .subtle_hover = RGBA(255, 255, 255, 0.059f), .subtle_pressed = RGBA(255, 255, 255, 0.035f),
        .progress_track = RGBA(255, 255, 255, 0.545f), .divider = RGBA(255, 255, 255, 0.082f),
        .flyout = RGBA(44, 44, 44, 1),
    },
};

const PwChromeTheme *pw_chrome_theme(int dark) { return &themes[!!dark]; }

static int inside(float x, float y, float x0, float y0, float x1, float y1, float r)
{
    if (x < x0 || x >= x1 || y < y0 || y >= y1) return 0;
    float cx = x < x0 + r ? x0 + r : x > x1 - r ? x1 - r : x, cy = y < y0 + r ? y0 + r : y > y1 - r ? y1 - r : y;
    return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r;
}

static uint8_t byte_of(float v) { return v <= 0 ? 0 : v >= 255 ? 255 : (uint8_t)(v + 0.5f); }

/* Premultiplied sums of 16 samples, out as opaque over under or as premultiplied BGRA. */
static uint32_t pixel(float r, float g, float b, float a, const PwRgba *under)
{
    r /= 16, g /= 16, b /= 16, a /= 16;
    if (under) r += under->r * (1 - a), g += under->g * (1 - a), b += under->b * (1 - a), a = 1;
    return (uint32_t)byte_of(b) | (uint32_t)byte_of(g) << 8 | (uint32_t)byte_of(r) << 16 | (uint32_t)byte_of(a * 255) << 24;
}

void pw_chrome_plate(uint32_t *bgra, int w, int h, float radius, PwRgba fill, PwRgba stroke, PwRgba bottom,
                     int bottom_width, const PwRgba *under)
{
    float inner = radius > 1 ? radius - 1 : 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float r = 0, g = 0, b = 0, a = 0;
            for (int s = 0; s < 16; ++s) {
                float sx = x + (s % 4 + 0.5f) / 4, sy = y + (s / 4 + 0.5f) / 4;
                const PwRgba *c = inside(sx, sy, 1, 1, w - 1, h - bottom_width, inner) ? &fill
                                : !inside(sx, sy, 0, 0, w, h, radius) ? 0 : sy >= h - bottom_width ? &bottom : &stroke;
                if (c) r += c->r * c->a, g += c->g * c->a, b += c->b * c->a, a += c->a;
            }
            bgra[y * w + x] = pixel(r, g, b, a, under);
        }
}

void pw_chrome_progress(uint32_t *bgra, int w, int percent, PwRgba track, PwRgba fill, const PwRgba *under)
{
    float end = (float)w * (percent < 0 ? 0 : percent > 100 ? 100 : percent) / 100;
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < w; ++x) {
            float r = 0, g = 0, b = 0, a = 0;
            for (int s = 0; s < 16; ++s) {
                float sx = x + (s % 4 + 0.5f) / 4, sy = y + (s / 4 + 0.5f) / 4;
                const PwRgba *c = inside(sx, sy, 0, 0, end, 3, 1.5f) ? &fill : sy >= 1 && sy < 2 ? &track : 0;
                if (c) r += c->r * c->a, g += c->g * c->a, b += c->b * c->a, a += c->a;
            }
            bgra[y * w + x] = pixel(r, g, b, a, under);
        }
}

PwRgba pw_chrome_blend(PwRgba colour, PwRgba under)
{
    float a = colour.a;
    PwRgba out = {byte_of(colour.r * a + under.r * (1 - a)), byte_of(colour.g * a + under.g * (1 - a)),
                  byte_of(colour.b * a + under.b * (1 - a)), 1};
    return out;
}

static char *put(char *p, char *end, const char *text) { while (*text && p < end) *p++ = *text++; return p; }

static char *put_number(char *p, char *end, unsigned long long n)
{
    char digits[24];
    int count = 0;
    do digits[count++] = (char)('0' + n % 10), n /= 10; while (n);
    while (count && p < end) *p++ = digits[--count];
    return p;
}

void pw_chrome_size(long long bytes, char *out, size_t capacity)
{
    static const char *const units[] = {" bytes", " KB", " MB", " GB", " TB"};
    if (!capacity) return;
    char *p = out, *end = out + capacity - 1;
    if (bytes < 0) bytes = 0;
    int unit = 0;
    unsigned long long scale = 1;
    while (unit < 4 && (unsigned long long)bytes >= scale * 1024) scale *= 1024, ++unit;
    if (!unit) p = put_number(p, end, (unsigned long long)bytes);
    else if ((unsigned long long)bytes < 10 * scale) {  /* toFixed(1) */
        unsigned long long tenths = ((unsigned long long)bytes * 10 + scale / 2) / scale;
        p = put_number(p, end, tenths / 10);
        p = put(p, end, ".");
        p = put_number(p, end, tenths % 10);
    } else p = put_number(p, end, ((unsigned long long)bytes + scale / 2) / scale);  /* Math.round */
    p = put(p, end, units[unit]);
    *p = 0;
}

/* .br-dl-row: 10 px above, 12 below, a 32 px icon and a 12 px gap before the column of name (20), progress (4 + 3 + 2
 * of margins), status (16) and actions (4 + 32), 4 px apart; 16 px at each side. */
enum { ROW_PAD_TOP = 10, ROW_PAD_BOTTOM = 12, ROW_SIDE = 16, ICON = 32, ICON_GAP = 12, GAP = 4 };

int pw_chrome_row_height(PwRowState state)
{
    int height = 1 + ROW_PAD_TOP + 20 + GAP + 16 + ROW_PAD_BOTTOM;
    if (state == PW_ROW_ACTIVE) height += 4 + 3 + 2 + GAP;
    if (state == PW_ROW_ACTIVE || state == PW_ROW_DONE) height += GAP + 4 + PW_FLYOUT_BUTTON;
    return height;
}

void pw_chrome_row(PwRowState state, int top, const int *label_widths, PwRowBoxes *out)
{
    int x = 1 + ROW_SIDE + ICON + ICON_GAP, w = PW_FLYOUT_WIDTH - 1 - ROW_SIDE - x, y = top + 1 + ROW_PAD_TOP;
    *out = (PwRowBoxes){0};
    out->row = (PwBox){1, top, PW_FLYOUT_WIDTH - 2, pw_chrome_row_height(state)};
    out->icon = (PwBox){1 + ROW_SIDE, y, ICON, ICON};
    out->name = (PwBox){x, y, w, 20};
    y += 20 + GAP;
    if (state == PW_ROW_ACTIVE) { out->progress = (PwBox){x, y + 4, w, 3}; y += 4 + 3 + 2 + GAP; }
    out->status = (PwBox){x, y, w, 16};
    y += 16 + GAP + 4;
    out->actions = state == PW_ROW_ACTIVE ? 1 : state == PW_ROW_DONE ? 2 : 0;
    for (int i = 0; i < out->actions; ++i) {
        out->action[i] = (PwBox){x, y, label_widths[i] + 2 * PW_FLYOUT_BUTTON_PAD, PW_FLYOUT_BUTTON};
        x += out->action[i].w + PW_FLYOUT_BUTTON_GAP;
    }
}

int pw_chrome_flyout_height(const PwRowState *rows, int count, int max_height)
{
    int height = 2 + PW_FLYOUT_HEAD + (count ? PW_FLYOUT_FOOT : PW_FLYOUT_EMPTY);
    for (int i = 0; i < count; ++i) height += pw_chrome_row_height(rows[i]);
    return height < max_height ? height : max_height;
}
