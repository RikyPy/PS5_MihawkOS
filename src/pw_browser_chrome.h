/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_BROWSER_CHROME_H
#define PW_BROWSER_CHROME_H
#include <stddef.h>
#include <stdint.h>

/* MihawkBrowser's chrome as the approved prototype draws it (docs/MIHAWK_BROWSER.md §4.1; boxes, colours and pixels in
 * docs/windows11-reference/mihawk-browser.json): WinUI's colour tokens, its control plates and the Downloads flyout's
 * geometry. src/pw_browser.c paints with them; tests/test_pw_browser_chrome.c checks them against the reference's
 * pixels and boxes. No Windows, no libc, so the same file builds for the host tests and for x86_64-windows. */

/* Straight alpha as a fraction (0.06, not 15/255), so over a known opaque colour the result is the reference's pixel to
 * the unit (pw_browser_chrome.c says where a token was fitted to Chromium's rounding). */
typedef struct PwRgba { uint8_t r, g, b; float a; } PwRgba;

typedef struct PwChromeTheme {
    PwRgba surface;                                   /* SolidBackgroundFillColorBase, opaque */
    PwRgba text, text_secondary, accent, on_accent;   /* TextFillColorPrimary, Secondary; AccentFillColorDefault */
    PwRgba control, control_hover, control_pressed;   /* ControlFillColorDefault, Secondary, Tertiary */
    PwRgba control_stroke, control_stroke_bottom;     /* a Button's elevation border: top and sides, bottom */
    PwRgba input_stroke, input_stroke_bottom, input_focused;  /* a TextBox's; its fill while focused */
    PwRgba accent_hover, accent_stroke, accent_stroke_bottom; /* an accent Button's */
    PwRgba subtle_hover, subtle_pressed;              /* SubtleFillColorSecondary, Tertiary: command buttons */
    PwRgba progress_track, divider;                   /* ControlStrongStrokeColorDefault, DividerStrokeColorDefault */
    PwRgba flyout;                                    /* what the flyout's acrylic shows: under its text and plates */
} PwChromeTheme;

const PwChromeTheme *pw_chrome_theme(int dark);

/* A rounded WinUI plate of w x h, corner radius in px: fill inside the 1 px stroke (top and sides in stroke, the last
 * bottom_width rows in bottom), the stroke over what lies beneath rather than over the fill, as the prototype's
 * padding-box backgrounds. A transparent stroke makes a plain rounded fill. BGRA, w per row, 4 x 4 samples a pixel:
 * opaque over *under when under is given (a solid surface), else premultiplied (glass, for the compositor). */
void pw_chrome_plate(uint32_t *bgra, int w, int h, float radius, PwRgba fill, PwRgba stroke, PwRgba bottom,
                     int bottom_width, const PwRgba *under);
/* A WinUI ProgressBar of w x 3: the 1 px track through its middle, the accent fill over the first percent. */
void pw_chrome_progress(uint32_t *bgra, int w, int percent, PwRgba track, PwRgba fill, const PwRgba *under);
/* A colour over an opaque one, as a solid surface or the compositor shows it. */
PwRgba pw_chrome_blend(PwRgba colour, PwRgba under);

/* The prototype's file sizes (Shell.fs.formatSize): "512 bytes", "2.3 MB", "191 MB". */
void pw_chrome_size(long long bytes, char *out, size_t capacity);

/* The Downloads flyout (.br-downloads, .surface-flyout): 360 px wide with its 1 px stroke, a 40 px head, one row per
 * download newest first, a 4 px foot. Boxes are in the flyout's own coordinates, its stroke included. */
enum {
    PW_FLYOUT_WIDTH = 360, PW_FLYOUT_HEAD = 40, PW_FLYOUT_FOOT = 4, PW_FLYOUT_EMPTY = 40,
    PW_FLYOUT_BUTTON = 32, PW_FLYOUT_BUTTON_PAD = 12, PW_FLYOUT_BUTTON_GAP = 8, PW_FLYOUT_ACTIONS = 2,
};
typedef enum PwRowState { PW_ROW_ACTIVE, PW_ROW_DONE, PW_ROW_CANCELED, PW_ROW_FAILED } PwRowState;
typedef struct PwBox { int x, y, w, h; } PwBox;
typedef struct PwRowBoxes {
    PwBox row, icon, name, progress, status;          /* progress is empty unless the row is active */
    PwBox action[PW_FLYOUT_ACTIONS];                  /* Cancel; or Open and Show in folder; or none */
    int actions;
} PwRowBoxes;
/* A row's boxes at top (its 1 px divider's row), given its buttons' label widths in px (Body, 14 px). */
void pw_chrome_row(PwRowState state, int top, const int *label_widths, PwRowBoxes *out);
int pw_chrome_row_height(PwRowState state);
/* The flyout's height for its rows, at most max_height (it scrolls past that); empty shows "No downloads yet". */
int pw_chrome_flyout_height(const PwRowState *rows, int count, int max_height);
#endif
