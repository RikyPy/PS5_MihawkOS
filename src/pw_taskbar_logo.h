/* SPDX-License-Identifier: GPL-3.0-or-later
 * The Start button's Mihawk mark at any size, as themes/windows11/start.svg draws it in its 100-unit box: a white
 * squircle (1 to 99, rx 30) under a 2-unit #C9D3DD stroke, a 2-unit amber frame (6 to 94, rx 25) and a navy M, an
 * 11-unit stroke through 25,80.5 25,34.5 50,64.5 75,34.5 75,80.5 with mitred corners and butt ends. The taskbar draws it
 * at every DPI, 22 px at 96 (Wine patch 1162). Each shape is antialiased by 16 x 16 samples.
 *
 * Output: extent x extent premultiplied BGRA words (0xAARRGGBB), as GdiAlphaBlend takes them. tests/test_taskbar_logo.py
 * compares it with librsvg's drawing of start.svg. */
#ifndef PW_TASKBAR_LOGO_H
#define PW_TASKBAR_LOGO_H
#include <stdint.h>

/* Whether x, y lies in the rounded square centred on 50,50 (half side `half`, corner radius `radius`) grown by `grow`
 * units (negative: shrunk), its corners' centres kept; squared distances, so no libm in Explorer. */
static inline int pw_taskbar_logo_inside(double x, double y, double half, double radius, double grow)
{
    const double r = radius + grow, qx = (x < 50 ? 50 - x : x - 50) - (half - radius);
    const double qy = (y < 50 ? 50 - y : y - 50) - (half - radius);
    if (qx <= 0 || qy <= 0) return qx <= r && qy <= r;
    return qx * qx + qy * qy <= r * r;
}

static inline void pw_taskbar_logo(uint32_t *bgra, int extent)
{
    /* The M's stroke as its outline: the inner side up the left stem, the V's underside, down the right stem, then the
     * outer side back; the mitre tips are where the offset edges meet. */
    static const double m[10][2] = {{30.5, 80.5}, {30.5, 49.6913}, {50, 73.0913}, {69.5, 49.6913}, {69.5, 80.5},
                                    {80.5, 80.5}, {80.5, 19.3087}, {50, 55.9087}, {19.5, 19.3087}, {19.5, 80.5}};
    static const uint32_t colours[4] = {0xffffff, 0xc9d3dd, 0xffb020, 0x0e2b46};  /* fill, its stroke, frame, M */
    for (int y = 0; y < extent; ++y)
        for (int x = 0; x < extent; ++x)
        {
            int covered[4] = {0, 0, 0, 0};
            double out[4] = {0, 0, 0, 0};  /* premultiplied B, G, R, A in 0..1 */
            uint32_t word = 0;
            for (int sy = 0; sy < 16; ++sy)
                for (int sx = 0; sx < 16; ++sx)
                {
                    const double u = (x + (sx + 0.5) / 16) * 100 / extent, v = (y + (sy + 0.5) / 16) * 100 / extent;
                    int in_m = 0;
                    covered[0] += pw_taskbar_logo_inside(u, v, 49, 30, 0);
                    covered[1] += pw_taskbar_logo_inside(u, v, 49, 30, 1) && !pw_taskbar_logo_inside(u, v, 49, 30, -1);
                    covered[2] += pw_taskbar_logo_inside(u, v, 44, 25, 1) && !pw_taskbar_logo_inside(u, v, 44, 25, -1);
                    for (int i = 0, j = 9; i < 10; j = i++)
                        if ((m[i][1] > v) != (m[j][1] > v) &&
                            u < m[j][0] + (v - m[j][1]) * (m[i][0] - m[j][0]) / (m[i][1] - m[j][1])) in_m = !in_m;
                    covered[3] += in_m;
                }
            /* Each shape antialiased alone and put over what is beneath, as an SVG renderer composes them. */
            for (int layer = 0; layer < 4; ++layer)
            {
                const double a = covered[layer] / 256.0;
                for (int k = 0; k < 3; ++k) out[k] = (colours[layer] >> (8 * k) & 255) / 255.0 * a + out[k] * (1 - a);
                out[3] = a + out[3] * (1 - a);
            }
            for (int k = 0; k < 4; ++k) word |= (uint32_t)(out[k] * 255 + 0.5) << (8 * k);
            bgra[y * extent + x] = word;
        }
}
#endif
