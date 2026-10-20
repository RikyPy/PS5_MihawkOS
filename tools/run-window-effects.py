#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Qualify DWM corners, borders, shadows, materials and thumbnails on the displayed frame.

The probe (tests/window-effects-probe.c) logs window rectangles and capture
phases. This verdict computes every expected pixel independently, in floating
point, from appearance.json's material tokens and the documented geometry:
supersampled corner coverage, the small shadow's quadratic falloff and a framed
window's shadow as Chromium draws the prototype's box-shadow layers (an 8-bit
three-box blur of the rounded rectangle, then each layer's alpha; the unblurred
ring's corners as Skia's analytic AA covers them, tools/skia-rrect-coverage.py), straight tint mixing,
a float box-blur model of Mica over the captured desktop root, and area-mapped
thumbnails. It never imports the compositor's C code.
"""
import argparse
import functools
import importlib.util
import json
import math
import re
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
THEME = ROOT / 'themes/windows11/appearance.json'
PHASES = ('root', 'effects', 'thumbnail-half', 'thumbnail-live', 'thumbnail-hidden', 'acrylic-dark')
BACK, RED, GREEN, YELLOW = (40, 90, 160), (220, 50, 60), (30, 180, 70), (240, 180, 30)
GREY, ORANGE, CYAN, PURPLE, WHITE = (128, 128, 128), (250, 130, 20), (20, 200, 220), (150, 40, 190), (255, 255, 255)
BORDER = (0, 120, 215)
STROKE = (0x75, 0x75, 0x75)  # the default border's colour; its alpha is 0x66


def tokens():
    return json.loads(THEME.read_text())['materials']


def hex_rgb(value):
    return tuple(int(value[i:i + 2], 16) for i in (1, 3, 5))


def coverage(radius, i, j):
    """Fraction of the pixel square [i,i+1)x[j,j+1) inside the circle of the
    given radius centred at (radius, radius), by 16x16 supersampling."""
    inside = 0
    for b in range(16):
        for a in range(16):
            x, y = i + (a + 0.5) / 16, j + (b + 0.5) / 16
            inside += (x - radius) ** 2 + (y - radius) ** 2 <= radius * radius
    return inside / 256


def ring(radius, i, j):
    inside = 0
    for b in range(16):
        for a in range(16):
            x, y = i + (a + 0.5) / 16, j + (b + 0.5) / 16
            d2 = (x - radius) ** 2 + (y - radius) ** 2
            inside += (radius - 1) ** 2 < d2 <= radius * radius
    return inside / 256


def corner_index(rect, radius, x, y):
    left, top, right, bottom = rect
    lx, rx, ty, by = x - left, right - 1 - x, y - top, bottom - 1 - y
    i = lx if lx < radius else rx if rx < radius else None
    j = ty if ty < radius else by if by < radius else None
    return i, j


def window_coverage(rect, radius, x, y):
    left, top, right, bottom = rect
    if not (left <= x < right and top <= y < bottom):
        return 0.0
    i, j = corner_index(rect, radius, x, y)
    return 1.0 if i is None or j is None else coverage(radius, i, j)


def shadow_alpha(rect, radius, size, alpha, offset, x, y):
    left, top, right, bottom = rect
    top, bottom = top + offset, bottom + offset
    px, py = x + 0.5, y + 0.5
    nx = min(max(px, left + radius), right - radius)
    ny = min(max(py, top + radius), bottom - radius)
    distance = math.hypot(px - nx, py - ny) - radius
    if distance >= size:
        return 0.0
    return alpha if distance <= 0 else alpha * (1 - distance / size) ** 2


def coverage8(radius, i, j):
    return (round(coverage(radius, i, j) * 256) * 255 + 128) // 256


@functools.lru_cache(maxsize=None)
def ring_corners(radius):
    """[corner][j][i] of an unblurred rounded rectangle as Skia fills it (top left, top right, bottom left, bottom right)."""
    spec = importlib.util.spec_from_file_location('skia_rrect', ROOT / 'tools/skia-rrect-coverage.py')
    skia = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(skia)
    return skia.ring_corners(radius)


def shadow_kernel(blur):
    """The W3C three-box approximation of a Gaussian of deviation blur/2, as Skia's mask blur builds it: weights at
    offsets -border..border, and its rounding weight round(2^32 / their sum)."""
    window = max(1, math.floor(blur / 2 * 3 * math.sqrt(2 * math.pi) / 4 + 0.5))
    half = window // 2
    boxes = [(-half, window)] * 3 if window % 2 else [(-half, window), (-half + 1, window), (-half, window + 1)]
    weights, low = [1], 0
    for first, size in boxes:
        wider = [0] * (len(weights) + size - 1)
        for i, w in enumerate(weights):
            for j in range(size):
                wider[i + j] += w
        weights, low = wider, low + first
    return -low, weights, round(1.0 / sum(weights) * 2 ** 32)


def shadow_mask(layer, rect, radius, x, y):
    """A box-shadow layer's 8-bit mask at (x, y): its rounded rectangle (offset down, grown by the spread) blurred
    across each row, rounded to 8 bits, then down, rounded again."""
    border, weights, weight = shadow_kernel(layer['blur'])
    left, top, right, bottom = rect
    spread = layer['spread']
    l, r, t, b = left - spread, right + spread, top + layer['offset'] - spread, bottom + layer['offset'] + spread
    corner = radius + spread if radius else 0
    ring = ring_corners(corner) if not layer['blur'] and corner == 9 else None  # the compositor's PW_SHADOW_RING_RADIUS

    def pass8(total):
        return (total * weight + (1 << 31)) >> 32

    def across(j):
        jj = j - t if j - t < corner else b - 1 - j if b - 1 - j < corner else None
        total = 0
        for i in range(max(l, x - border), min(r, x + border + 1)):
            ii = i - l if i - l < corner else r - 1 - i if r - 1 - i < corner else None
            if jj is None or ii is None:
                cover = 255
            elif ring:
                cover = ring[(0 if j - t < corner else 2) + (0 if i - l < corner else 1)][jj][ii]
            else:
                cover = coverage8(corner, ii, jj)
            total += weights[x - i + border] * cover
        return pass8(total)
    return pass8(sum(weights[y - j + border] * across(j) for j in range(max(t, y - border), min(b, y + border + 1))))


def window_shadow(colour, rect, radius, x, y, layers, cover=0):
    """colour under a framed window's shadow layers, bottom first: each darkens by a = (alpha*(mask + 1)) >> 8, every
    channel becoming (v*(256 - a)) >> 8; where the window's anti-aliased edge covers the pixel, the mask is scaled
    by what it leaves uncovered. alpha is the layer's alpha*255 rounded half up."""
    colour = tuple(int(c) for c in colour)
    for layer in layers:
        mask = shadow_mask(layer, rect, radius, x, y)
        if cover:
            product = mask * (255 - cover) + 128
            mask = (product + (product >> 8)) >> 8
        a = (math.floor(layer['alpha'] * 255 + 0.5) * (mask + 1)) >> 8
        colour = tuple((c * (256 - a)) >> 8 for c in colour)
    return colour


def flyout_shadow(colour, rect, radius, x, y, layers):
    """colour under a flyout's box-shadow layers: Chromium draws a flyout (backdrop-filter) and its shadows into a layer
    of their own, each a = (alpha*(mask + 1)) >> 8 over the ones below it, A = a + (A*(256 - a)) >> 8, then darkens the
    page by A once, every channel becoming (v*(256 - A)) >> 8."""
    combined = 0
    for layer in layers:
        a = (math.floor(layer['alpha'] * 255 + 0.5) * (shadow_mask(layer, rect, radius, x, y) + 1)) >> 8
        combined = a + ((combined * (256 - a)) >> 8)
    return tuple((int(c) * (256 - combined)) >> 8 for c in colour)


def window_cover8(rect, radius, x, y):
    left, top, right, bottom = rect
    if not (left <= x < right and top <= y < bottom):
        return 0
    i, j = corner_index(rect, radius, x, y)
    return 255 if i is None or j is None else coverage8(radius, i, j)


def darken(colour, alpha):
    return tuple(c * (255 - alpha) / 255 for c in colour)


def mix(dest, source, amount):
    return tuple(d * (1 - amount) + s * amount for d, s in zip(dest, source))


def tint(colour, tint_colour, amount):
    return tuple(c * (255 - amount) / 255 + t * amount / 255 for c, t in zip(colour, tint_colour))


def mica_reference(root, materials):
    """Float model: exact block means at 1/scale, clamped box passes, bilinear."""
    scale, radius, passes = materials['mica_scale'], materials['mica_radius'], materials['blur_passes']
    image = np.asarray(root.convert('RGB'), dtype=np.float64)
    height, width = image.shape[:2]
    bw, bh = -(-width // scale), -(-height // scale)
    ys = np.clip(np.arange(bh * scale), 0, height - 1)
    xs = np.clip(np.arange(bw * scale), 0, width - 1)
    blocks = image[ys][:, xs].reshape(bh, scale, bw, scale, 3).mean(axis=(1, 3))
    for _ in range(passes):
        for axis in (1, 0):
            length = blocks.shape[axis]
            index = np.clip(np.arange(length)[:, None] + np.arange(-radius, radius + 1)[None, :], 0, length - 1)
            blocks = np.take(blocks, index, axis=axis).mean(axis=axis + 1)

    def sample(x, y):
        u = min(max((x + 0.5) / scale - 0.5, 0), bw - 1)
        v = min(max((y + 0.5) / scale - 0.5, 0), bh - 1)
        x0, y0 = int(u), int(v)
        x1, y1 = min(x0 + 1, bw - 1), min(y0 + 1, bh - 1)
        fx, fy = u - x0, v - y0
        top = blocks[y0, x0] * (1 - fx) + blocks[y0, x1] * fx
        bottom = blocks[y1, x0] * (1 - fx) + blocks[y1, x1] * fx
        return tuple(top * (1 - fy) + bottom * fy)
    return sample


def acrylic_reference(beneath, rect, materials, screen=(1920, 1080)):
    """Float model of acrylic: the window rectangle grown by the blur reach and
    clamped to the screen, exact block means from that origin, clamped box
    passes, bilinear expansion. `beneath` is the frame composed without the
    window (a capture taken before it appeared)."""
    scale, radius, passes = materials['acrylic_scale'], materials['acrylic_radius'], materials['blur_passes']
    margin = scale * radius * passes
    left, top = max(0, rect[0] - margin), max(0, rect[1] - margin)
    right, bottom = min(screen[0], rect[2] + margin), min(screen[1], rect[3] + margin)
    image = np.asarray(beneath.convert('RGB'), dtype=np.float64)
    bw, bh = -(-(right - left) // scale), -(-(bottom - top) // scale)
    ys = np.clip(top + np.arange(bh * scale), 0, image.shape[0] - 1)
    xs = np.clip(left + np.arange(bw * scale), 0, image.shape[1] - 1)
    blocks = image[ys][:, xs].reshape(bh, scale, bw, scale, 3).mean(axis=(1, 3))
    for _ in range(passes):
        for axis in (1, 0):
            length = blocks.shape[axis]
            index = np.clip(np.arange(length)[:, None] + np.arange(-radius, radius + 1)[None, :], 0, length - 1)
            blocks = np.take(blocks, index, axis=axis).mean(axis=axis + 1)

    def sample(x, y):
        u = min(max((x - left + 0.5) / scale - 0.5, 0), bw - 1)
        v = min(max((y - top + 0.5) / scale - 0.5, 0), bh - 1)
        x0, y0 = int(u), int(v)
        x1, y1 = min(x0 + 1, bw - 1), min(y0 + 1, bh - 1)
        fx, fy = u - x0, v - y0
        upper = blocks[y0, x0] * (1 - fx) + blocks[y0, x1] * fx
        lower = blocks[y1, x0] * (1 - fx) + blocks[y1, x1] * fx
        return tuple(upper * (1 - fy) + lower * fy)
    return sample


def parse(log):
    rects = {}
    active = re.search(r'window-effects active name=(\w+)', log)
    for name, window, client in re.findall(r'window-effects rect name=(\w+) window=([-\d,]+) client=([-\d,]+)', log):
        rects[name] = {'window': tuple(map(int, window.split(','))), 'client': tuple(map(int, client.split(',')))}
    for name, r, g, b in re.findall(r'window-effects frame name=(\w+) colour=(\d+),(\d+),(\d+)', log):
        if name in rects: rects[name]['frame'] = (int(r), int(g), int(b))
    if active and active.group(1) in rects:
        rects[active.group(1)]['active'] = True
    phases = {}
    for number, phase in re.findall(r'window-effects capture=(\d+) phase=([\w-]+)', log):
        phases[int(number)] = phase
    return rects, phases


def expectations(rects, materials, mica):
    """Named sample points per phase: (x, y, expected RGB floats, tolerance)."""
    def framed_layers(name):  # the probe's framed windows are light; the one it logs as active casts the active shadow
        return materials['shadow_window']['active' if rects[name].get('active') else 'inactive']['light']
    small_shadow = (materials['shadow_small']['size'], materials['shadow_small']['alpha'], materials['shadow_small']['offset'])
    acrylic = materials['acrylic']
    mica_dark = materials['mica']
    samples = {phase: [] for phase in PHASES}
    effects = samples['effects']

    def rounded(name, colour, radius, points):
        rect = rects[name]['window']
        for x, y in points:
            under = window_shadow(BACK, rect, radius, x, y, framed_layers(name), window_cover8(rect, radius, x, y))
            effects.append((name, x, y, mix(under, colour, window_coverage(rect, radius, x, y)), 2))

    r = rects['round']['window']
    rounded('round', RED, 8, [(r[0], r[1]), (r[0] + 1, r[1] + 1), (r[0] + 2, r[1]), (r[0] + 3, r[1] + 1),
                                             (r[0] + 5, r[1]), (r[2] - 1, r[3] - 1), (r[2] - 2, r[3] - 1),
                                             ((r[0] + r[2]) // 2, (r[1] + r[3]) // 2), ((r[0] + r[2]) // 2, r[3] + 2),
                                             ((r[0] + r[2]) // 2, r[1] - 4), (r[2] + 5, (r[1] + r[3]) // 2)])
    f = rects['framed']['window']
    rounded('framed', YELLOW, 8, [(f[0], f[1]), (f[2] - 1, f[1]), (f[0], f[3] - 1), ((f[0] + f[2]) // 2, f[3] + 3)])
    c = rects['framed']['client']
    effects.append(('framed-client', (c[0] + c[2]) // 2, (c[1] + c[3]) // 2, YELLOW, 0))
    # Windows 11's default 1 px border (#66757575) over the framed window's own frame colour, which the probe logs
    edge = rects['framed']['frame']
    stroke = mix(edge, STROKE, 0x66 / 255)
    for x, y in [((f[0] + f[2]) // 2, f[1]), (f[0], (f[1] + f[3]) // 2), (f[2] - 1, (f[1] + f[3]) // 2),
                 ((f[0] + f[2]) // 2, f[3] - 1)]:
        effects.append(('framed-border', x, y, stroke, 1))
    x, y = f[0] + 2, f[1] + 1  # on the top left corner's ring
    i, j = corner_index(f, 8, x, y)
    under = mix(window_shadow(BACK, f, 8, x, y, framed_layers('framed'), window_cover8(f, 8, x, y)), edge,
                window_coverage(f, 8, x, y))
    effects.append(('framed-corner', x, y, mix(under, STROKE, ring(8, i, j) * 0x66 / 255), 2))
    d = rects['dropped']['window']
    for x, y in [((d[0] + d[2]) // 2, d[3]), ((d[0] + d[2]) // 2, d[3] + 4), (d[2] + 2, (d[1] + d[3]) // 2)]:
        effects.append(('dropped', x, y, darken(BACK, shadow_alpha(d, 0, *small_shadow, x, y)), 2))
    effects.append(('dropped', (d[0] + d[2]) // 2, (d[1] + d[3]) // 2, GREY, 0))
    s = rects['small']['window']
    for x, y in [(s[0], s[1]), (s[0] + 1, s[1] + 1), (s[0] + 2, s[1]), (s[0] + 1, s[1] + 2), (s[2] - 1, s[3] - 1),
                 ((s[0] + s[2]) // 2, s[1]), (s[0], (s[1] + s[3]) // 2), ((s[0] + s[2]) // 2, s[3] - 1),
                 ((s[0] + s[2]) // 2, (s[1] + s[3]) // 2), ((s[0] + s[2]) // 2, s[3] + 1)]:
        shade = shadow_alpha(s, 4, *small_shadow, x, y)
        value = mix(darken(BACK, shade), GREEN, window_coverage(s, 4, x, y))
        if s[0] <= x < s[2] and s[1] <= y < s[3]:
            i, j = corner_index(s, 4, x, y)
            if i is not None and j is not None:
                value = mix(value, BORDER, ring(4, i, j))
            elif x in (s[0], s[2] - 1) or y in (s[1], s[3] - 1):
                value = BORDER
        effects.append(('small', x, y, value, 2))
    g = rects['acrylic']['window']
    light, dark = hex_rgb(acrylic['light']), hex_rgb(acrylic['dark'])
    glass = [(g[0] + 200, g[1] + 150), (g[0] + 150, g[1] + 200), (g[0] + 250, g[1] + 100)]
    for x, y in glass:
        effects.append(('acrylic', x, y, tint(BACK, light, acrylic['tint']), 0))
        samples['acrylic-dark'].append(('acrylic', x, y, tint(BACK, dark, acrylic['tint']), 0))
    for phase in ('effects', 'acrylic-dark'):
        samples[phase].append(('acrylic-solid', g[0] + 75, g[1] + 75, WHITE, 0))
    m = rects['mica']['window']
    for x, y in [(m[0] + 150, m[1] + 125), (m[0] + 50, m[1] + 50), (m[0] + 250, m[1] + 200)]:
        effects.append(('mica', x, y, tint(mica(x, y), hex_rgb(mica_dark['dark']), mica_dark['tint']), 3))
    t = rects['destination']['client']
    left_half, right_half = (t[0] + 60, t[1] + 90), (t[0] + 180, t[1] + 90)
    outside = [(t[0] + 10, t[1] + 10), (t[0] + 150, t[1] + 230)]
    effects += [('thumbnail', *left_half, ORANGE, 0), ('thumbnail', *right_half, CYAN, 0)]
    effects += [('destination', x, y, GREY, 0) for x, y in outside]
    src = rects['source']['window']
    effects += [('source', src[0] + 100, src[1] + 100, ORANGE, 0), ('source', src[0] + 300, src[1] + 100, CYAN, 0)]
    samples['thumbnail-half'] += [('thumbnail', *left_half, mix(GREY, ORANGE, 128 / 255), 1),
                                  ('thumbnail', *right_half, mix(GREY, CYAN, 128 / 255), 1)]
    samples['thumbnail-live'] += [('thumbnail', *left_half, PURPLE, 0), ('thumbnail', *right_half, CYAN, 0),
                                  ('source', src[0] + 100, src[1] + 100, PURPLE, 0)]
    samples['thumbnail-hidden'] += [('thumbnail', *left_half, GREY, 0), ('thumbnail', *right_half, GREY, 0)]
    return samples


def display_verdict(captures, log, materials=None):
    materials = materials or tokens()
    rects, phases = parse(log)
    required = {'back', 'round', 'small', 'framed', 'dropped', 'acrylic', 'mica', 'destination', 'source'}
    results = []
    # The last capture logged for each phase is the one judged (a root still flat is taken again under the next number).
    by_number = {int(m.group(1)): c for c in captures if (m := re.search(r'(\d+)\.png$', str(c)))}
    chosen = {phase: number for number, phase in sorted(phases.items())}
    order = [chosen.get(phase) for phase in PHASES]
    if None in order or order != sorted(order) or any(n not in by_number for n in order) or not required <= rects.keys() \
            or 'frame' not in rects['framed']:
        return [{'error': 'missing captures, phases or rectangles', 'captures': len(captures),
                 'phases': phases, 'rects': sorted(rects)}], False
    captures = [by_number[n] for n in order]
    with Image.open(ROOT / captures[0]) as root:
        mica = mica_reference(root, materials)
    samples = expectations(rects, materials, mica)
    for number, capture in enumerate(captures, 1):
        phase = PHASES[number - 1]
        with Image.open(ROOT / capture) as source:
            frame = source.convert('RGB')
        pixels = []
        for role, x, y, expected, tolerance in samples[phase]:
            actual = frame.getpixel((x, y)) if frame.size == (1920, 1080) and 0 <= x < 1920 and 0 <= y < 1080 else None
            rounded = tuple(int(math.floor(c + 0.5)) for c in expected)
            passed = actual is not None and all(abs(a - e) <= tolerance + 0.5 for a, e in zip(actual, expected))
            pixels.append({'role': role, 'point': (x, y), 'expected': rounded, 'actual': actual,
                           'tolerance': tolerance, 'passed': passed})
        results.append({'capture': capture, 'number': number, 'phase': phase, 'pixels': pixels,
                        'passed': frame.size == (1920, 1080) and all(p['passed'] for p in pixels)})
    return results, all(item['passed'] for item in results)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--name', required=True)
    parser.add_argument('--arch', choices=('32', '64'), required=True)
    args = parser.parse_args()
    result = subprocess.run([sys.executable, str(ROOT / 'tools/run-console.py'), '--name', args.name,
        '--path', r'C:\windows\system32\mihawk.exe', '--args', f'C:\\window-effects{args.arch}-probe.exe',
        '--marker', 'PS5_PROTON_WINDOW_EFFECTS', '--env', 'PROTON_LAYOUT=launcher',
        '--env', 'WINE_PS5_WINDOW_SURFACES=1', '--env', 'PROTON_WINDOW_SNAPSHOT_PROBE=1',
        '--seconds', '180', '--watchdog', '240'], cwd=ROOT)
    path = ROOT / 'evidence' / (args.name + '.json')
    if not path.exists():
        return result.returncode or 1
    record = json.loads(path.read_text())
    log = '\n'.join(p.read_text(errors='replace') for p in (ROOT / 'klog').glob(args.name + '*.log'))
    pixels, passed = display_verdict(record.get('captures', []), log)
    record['semantics_passed'] = record['passed']
    record['effect_pixels'] = pixels
    # How long the desktop showed no wallpaper: the shell window's wait, then root captures still flat.
    startup = re.search(r'window-effects startup shell_wait_ms=(\d+) root_flat_ms=(\d+) retakes=(\d+)', log)
    if startup:
        record['startup'] = dict(zip(('shell_wait_ms', 'root_flat_ms', 'root_retakes'), map(int, startup.groups())))
    record['passed'] = result.returncode == 0 and record['passed'] and passed
    path.write_text(json.dumps(record, indent=1) + '\n')
    print(json.dumps({'effect_pixels': pixels, 'passed': record['passed']}, indent=1))
    return 0 if record['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
