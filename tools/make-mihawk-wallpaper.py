#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""The Mihawk OS desktop wallpaper, "Aero Meadow": original artwork made only by this script (no photograph, no
third-party image), in the Frutiger Aero manner: a deep-to-pale sky with a sun glow and soft rays, glossy soap bubbles
with iridescent rims, and rolling green hills under a horizon haze. Seeded, so every run gives the same pixels.

  tools/make-mihawk-wallpaper.py OUT.png [--size 1920x1080] [--dark]   rendered at twice the size and downsampled
                                                                     (antialiasing); --dark: the same meadow at night
"""
import argparse
import math

import numpy as np
from PIL import Image

SEED = 20261018
# The colours each layer is drawn in. Dark is the same meadow at night: a navy sky, the moon where the sun is, an aurora
# for the ribbons, moonlit hills and dimmer bubbles; every shape and the seeded layout are shared.
PALETTES = {
    'light': {'sky': ((0.13, 0.47, 0.88), (0.42, 0.74, 0.98), (0.88, 0.97, 1.0)),
              'glow': (0.97, 1.0, 1.0), 'glow_alpha': 0.75, 'sun': (1.0, 1.0, 1.0), 'sun_alpha': 0.55,
              'rays': (1.0, 1.0, 1.0), 'rays_alpha': 0.22, 'ribbon': (1.0, 1.0, 1.0), 'ribbon_glow': (0.85, 0.97, 1.0),
              'ribbons': 1.0, 'cloud': (1.0, 1.0, 1.0), 'cloud_alpha': 0.28, 'haze': (1.0, 1.0, 1.0), 'haze_alpha': 0.70,
              'far': ((0.70, 0.90, 0.62), (0.55, 0.82, 0.46)), 'near': ((0.64, 0.92, 0.27), (0.20, 0.58, 0.10)),
              'near_lit': (0.80, 1.0, 0.42), 'crest': (1.0, 1.0, 0.92), 'crest_alpha': 0.45,
              'front': ((0.42, 0.78, 0.18), (0.16, 0.48, 0.08)), 'front_shine': (1.0, 1.0, 0.9), 'front_alpha': 0.30,
              'bubbles': 1.0, 'sparkle': (1.0, 1.0, 1.0)},
    'dark': {'sky': ((0.02, 0.04, 0.13), (0.04, 0.10, 0.27), (0.13, 0.27, 0.42)),
             'glow': (0.42, 0.58, 0.92), 'glow_alpha': 0.30, 'sun': (0.92, 0.96, 1.0), 'sun_alpha': 0.70,
             'rays': (0.70, 0.82, 1.0), 'rays_alpha': 0.07, 'ribbon': (0.55, 1.0, 0.85), 'ribbon_glow': (0.30, 0.85, 0.75),
             'ribbons': 0.75, 'cloud': (0.45, 0.55, 0.80), 'cloud_alpha': 0.12, 'haze': (0.30, 0.48, 0.66), 'haze_alpha': 0.45,
             'far': ((0.11, 0.25, 0.25), (0.08, 0.20, 0.17)), 'near': ((0.13, 0.36, 0.18), (0.03, 0.14, 0.06)),
             'near_lit': (0.30, 0.52, 0.42), 'crest': (0.72, 0.86, 1.0), 'crest_alpha': 0.30,
             'front': ((0.09, 0.26, 0.12), (0.02, 0.10, 0.04)), 'front_shine': (0.70, 0.84, 1.0), 'front_alpha': 0.20,
             'bubbles': 0.70, 'sparkle': (0.86, 0.92, 1.0)},
}


def smoothstep(edge0, edge1, x):
    t = np.clip((x - edge0) / (edge1 - edge0), 0.0, 1.0)
    return t * t * (3 - 2 * t)


def mix(a, b, t):
    return a + (b - a) * t[..., None]


def over(image, colour, alpha):
    """Composite a colour (array or triple) with alpha in 0..1 over image, in place."""
    image *= 1 - alpha[..., None]
    image += np.asarray(colour) * alpha[..., None]


def sky(w, h, p):
    y = np.linspace(0, 1, h)[:, None] * np.ones((1, w))
    x = np.ones((h, 1)) * np.linspace(0, 1, w)[None, :]
    top, middle, horizon = (np.array(colour) for colour in p['sky'])
    image = mix(top[None, None, :] * np.ones((h, w, 3)), middle, smoothstep(0.0, 0.55, y))
    image = mix(image, horizon, smoothstep(0.50, 0.80, y))
    # the sun's glow in the upper right, and soft rays from it
    sx, sy = 0.80, 0.08
    dx, dy = (x - sx) * w / h, y - sy
    d = np.sqrt(dx * dx + dy * dy)
    over(image, p['glow'], p['glow_alpha'] * np.exp(-(d / 0.42) ** 2))
    over(image, p['sun'], p['sun_alpha'] * np.exp(-(d / 0.10) ** 2))
    angle = np.arctan2(dy, dx)
    rays = sum(strength * np.exp(-((angle - centre) / width) ** 2)
               for centre, width, strength in ((2.05, 0.035, 1.0), (2.30, 0.05, 0.7), (2.60, 0.03, 0.8), (2.85, 0.06, 0.5),
                                               (1.70, 0.04, 0.6), (3.10, 0.045, 0.45), (1.40, 0.03, 0.4)))
    over(image, p['rays'], p['rays_alpha'] * rays * np.exp(-(d / 1.0) ** 2) * smoothstep(0.03, 0.20, d))
    # two glowing ribbons across the sky
    for base, amplitude, frequency, phase, thickness, strength in ((0.30, 0.07, 0.8, 0.4, 0.004, 0.55),
                                                                   (0.36, 0.05, 0.95, 1.3, 0.0025, 0.40)):
        strength *= p['ribbons']
        curve = base + amplitude * np.sin(2 * math.pi * frequency * x + phase) + 0.12 * (x - 0.5)
        gap = np.abs(y - curve)
        fade = smoothstep(0.0, 0.25, x) * smoothstep(1.0, 0.70, x)
        over(image, p['ribbon'], strength * fade * np.exp(-(gap / thickness) ** 2))
        over(image, p['ribbon_glow'], 0.18 * strength * fade * np.exp(-(gap / (thickness * 8)) ** 2))
    # a few soft clouds near the horizon
    rng = np.random.default_rng(SEED + 1)
    for _ in range(9):
        cx, cy = rng.uniform(-0.05, 1.05), rng.uniform(0.55, 0.72)
        rx, ry = rng.uniform(0.10, 0.22), rng.uniform(0.025, 0.05)
        q = ((x - cx) * w / h / rx) ** 2 + ((y - cy) / ry) ** 2
        over(image, p['cloud'], p['cloud_alpha'] * np.exp(-q * 1.6))
    # the haze where sky meets land
    over(image, p['haze'], p['haze_alpha'] * np.exp(-((y - 0.765) / 0.055) ** 2))
    return image, x, y


def hills(image, x, y, w, h, p):
    aa = 1.5 / h

    def ridge(base, waves):
        return base + sum(a * np.sin(f * 2 * math.pi * x + p) for a, f, p in waves)

    # far hill: pale and hazy
    far = ridge(0.785, [(0.020, 0.55, 0.3), (0.010, 1.7, 2.1), (0.004, 4.3, 0.8)])
    cover = np.clip((y - far) / aa + 0.5, 0, 1)
    colour = mix(np.ones_like(image) * np.array(p['far'][0]), np.array(p['far'][1]),
                 smoothstep(0.0, 0.12, y - far))
    over(image, colour, cover * 0.95)
    # near hill: glossy grass, lit from the upper right
    near = ridge(0.865, [(0.045, 0.42, 4.0), (0.018, 1.1, 1.0), (0.006, 3.1, 2.6)])
    depth = y - near
    cover = np.clip(depth / aa + 0.5, 0, 1)
    colour = mix(np.ones_like(image) * np.array(p['near'][0]), np.array(p['near'][1]),
                 smoothstep(0.0, 0.16, depth))
    colour = mix(colour, np.array(p['near_lit']), 0.35 * smoothstep(0.3, 1.0, x) * np.exp(-(depth / 0.05) ** 2))
    over(image, colour, cover)
    over(image, p['crest'], cover * p['crest_alpha'] * np.exp(-(depth / 0.008) ** 2))  # the shine along the crest
    # a darker foreground swell on the left
    front = ridge(0.955, [(0.030, 0.35, 1.2), (0.010, 1.4, 0.4)])
    depth = y - front
    cover = np.clip(depth / aa + 0.5, 0, 1) * smoothstep(0.75, 0.15, x)
    colour = mix(np.ones_like(image) * np.array(p['front'][0]), np.array(p['front'][1]),
                 smoothstep(0.0, 0.08, depth))
    over(image, colour, cover)
    over(image, p['front_shine'], cover * p['front_alpha'] * np.exp(-(depth / 0.006) ** 2))


def bubble(image, cx, cy, radius, w, h, hue, soft=1.2, strength=1.0):
    """One soap bubble: a faint body with light gathered at its foot, an iridescent rim, a crescent window highlight
    and a small glint. soft widens the edge (a bubble out of focus); strength scales it (a farther one)."""
    pad = int(3 + soft)
    x0, x1 = max(0, int(cx - radius - pad)), min(w, int(cx + radius + pad + 1))
    y0, y1 = max(0, int(cy - radius - pad)), min(h, int(cy + radius + pad + 1))
    if x0 >= x1 or y0 >= y1:
        return
    yy, xx = np.mgrid[y0:y1, x0:x1].astype(np.float64)
    dx, dy = (xx - cx) / radius, (yy - cy) / radius
    d = np.sqrt(dx * dx + dy * dy)
    cover = np.clip((1.0 - d) * radius / soft, 0, 1) * strength
    region = image[y0:y1, x0:x1]
    angle = np.arctan2(dy, dx)
    iris = 0.5 + 0.5 * np.sin(angle * 1.3 + hue)
    rim = mix(np.ones(region.shape) * np.array([1.0, 0.72, 0.95]), np.array([0.62, 0.96, 1.0]), iris)
    rim = mix(rim, np.array([0.80, 1.0, 0.70]), 0.35 * (0.5 + 0.5 * np.sin(angle * 2.1 - hue)))
    over(region, (0.88, 0.97, 1.0), cover * 0.08)
    over(region, (0.92, 1.0, 1.0), cover * 0.30 * smoothstep(0.1, 0.95, dy) * smoothstep(1.0, 0.6, d))  # light below
    over(region, rim, cover * 0.75 * smoothstep(0.80, 0.985, d))
    over(region, (1.0, 1.0, 1.0), cover * 0.55 * np.exp(-((d - 0.975) / 0.018) ** 2))  # the thin bright edge
    # the crescent window highlight, upper left
    hx, hy = dx + 0.30, dy + 0.38
    c, s = math.cos(-0.65), math.sin(-0.65)
    u, v = (hx * c - hy * s) / 0.40, (hx * s + hy * c) / 0.22
    window = smoothstep(1.0, 0.70, np.sqrt(u * u + v * v))
    u2, v2 = (hx * c - (hy - 0.10) * s) / 0.40, (hx * s + (hy - 0.10) * c) / 0.20
    window *= smoothstep(0.75, 1.0, np.sqrt(u2 * u2 + v2 * v2)) * 0.6 + 0.4
    over(region, (1.0, 1.0, 1.0), cover * 0.85 * window)
    g = np.sqrt((dx - 0.52) ** 2 + (dy - 0.40) ** 2) / 0.10
    over(region, (1.0, 1.0, 1.0), cover * 0.75 * smoothstep(1.0, 0.25, g))


def sparkles(image, w, h, rng, p):
    """Small soft glints around the sun and the larger bubbles."""
    for _ in range(46):
        cx = rng.normal(0.80, 0.16) * w
        cy = abs(rng.normal(0.08, 0.20)) * h
        r = rng.uniform(1.5, 5.0) * h / 1080
        x0, x1, y0, y1 = int(cx - 6 * r), int(cx + 6 * r) + 1, int(cy - 6 * r), int(cy + 6 * r) + 1
        if x0 < 0 or y0 < 0 or x1 > w or y1 > h:
            continue
        yy, xx = np.mgrid[y0:y1, x0:x1].astype(np.float64)
        q = ((xx - cx) ** 2 + (yy - cy) ** 2) / (r * r)
        over(image[y0:y1, x0:x1], p['sparkle'], rng.uniform(0.35, 0.8) * np.exp(-q))


def render(w, h, theme='light'):
    p = PALETTES[theme]
    image, x, y = sky(w, h, p)
    rng = np.random.default_rng(SEED)
    # two large bubbles close to the eye, out of focus; the others keep clear of them
    near = [(0.90 * w, 0.58 * h, 0.15 * h, 1.0), (0.47 * w, 0.90 * h, 0.11 * h, 3.4)]
    placed = [(cx, cy, r) for cx, cy, r, _ in near]
    # bubbles in front of the sky and over the hills, larger ones fewer, none on the taskbar's strip
    for size, count in ((0.090, 3), (0.060, 4), (0.035, 7), (0.018, 11)):
        for _ in range(count):
            for _attempt in range(80):
                r = size * h * rng.uniform(0.75, 1.15)
                cx, cy = rng.uniform(0.02, 0.98) * w, rng.uniform(0.04, 0.86) * h
                if size > 0.03 and (cx < 0.16 * w or math.hypot(cx - 0.80 * w, cy - 0.08 * h) < 0.20 * h + r):
                    continue  # the desktop icons' column and the sun's core stay clear
                if all(math.hypot(cx - px, cy - py) > (r + pr) * 1.08 for px, py, pr in placed):
                    placed.append((cx, cy, r))
                    break
    hills(image, x, y, w, h, p)
    sparkles(image, w, h, rng, p)
    for cx, cy, r in sorted(placed[len(near):], key=lambda b: b[2]):
        far = r < 0.025 * h
        bubble(image, cx, cy, r, w, h, rng.uniform(0, 2 * math.pi), strength=(0.75 if far else 1.0) * p['bubbles'])
    for cx, cy, r, hue in near:
        bubble(image, cx, cy, r, w, h, hue, soft=0.10 * r, strength=0.55 * p['bubbles'])
    return Image.fromarray((np.clip(image, 0, 1) * 255 + 0.5).astype(np.uint8), 'RGB')


def wallpaper(w, h, theme='light'):
    """The wallpaper at w x h: drawn at twice the size and downsampled, which antialiases every edge."""
    return render(w * 2, h * 2, theme).resize((w, h), Image.Resampling.LANCZOS)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('out')
    parser.add_argument('--size', default='1920x1080')
    parser.add_argument('--dark', action='store_true', help='the same meadow at night')
    args = parser.parse_args()
    w, h = (int(v) for v in args.size.split('x'))
    wallpaper(w, h, 'dark' if args.dark else 'light').save(args.out)


if __name__ == '__main__':
    main()
