# SPDX-License-Identifier: GPL-3.0-or-later
"""Windows 11 caption buttons for the managed theme, as the approved prototype draws them, to the pixel.

The prototype (PS5_Proton-design prototypes/windows11-shell, css/window.css) draws each button 46 x 32 with no
plate at rest, SubtleFillColorSecondary while hot, SubtleFillColorTertiary while pressed (the glyph then
TextFillColorSecondary), #C42B1C behind a white glyph for a hot close button and 90% of it behind 70% white while
pressed; an inactive window's glyphs are TextFillColorDisabled until hovered. Glyphs are its 10 x 10 strokes, as
coverage measured from its own rendering (themes/windows11/caption/glyphs.json). Colours are the WinUI resources in
docs/windows11-reference/winui-colors.json. The pixels themselves are measured from the prototype
(tools/measure-prototype-caption.mjs): Skia's coverage and rounding are data here, not a model.

The bitmaps are drawn by uxtheme (Window.CloseButton and the others, eight states: normal, hot, pushed, disabled,
then the same inactive) over the caption bar. Each alpha is chosen so that Wine's own arithmetic (uxtheme's
premultiply, then AlphaBlend's source-over in the DIB engine) lands on the value the prototype renders over the
reference bar (the inactive caption: #f3f3f3 light, #202020 dark); over another bar colour it is within one level.
"""
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RED = (0xc4, 0x2b, 0x1c)  # tier-a: the caption close red, both themes
BAR = {'light': (0xf3, 0xf3, 0xf3), 'dark': (0x20, 0x20, 0x20)}
STATES = ('normal', 'hot', 'pushed', 'disabled')
GLYPH = {'close': 'close', 'min': 'min', 'max': 'max', 'restore': 'restore'}


def token(theme, name):
    value = json.loads((ROOT / 'docs/windows11-reference/winui-colors.json').read_text())['themes'][theme][name].lstrip('#')
    if len(value) == 6: value = 'ff' + value
    a, r, g, b = (int(value[i:i + 2], 16) for i in (0, 2, 4, 6))
    return (r, g, b), a / 255


def css(dst, colour, alpha):
    """What the prototype's renderer produces: straight-alpha source-over, rounded."""
    return tuple(round(d * (1 - alpha) + c * alpha) for d, c in zip(dst, colour))


def wine(dst, colour, a):
    """uxtheme premultiplies ((c * (a + 1)) >> 8), then the DIB engine's blend_argb adds dst * (255 - a)."""
    return tuple(((c * (a + 1)) >> 8) + (d * (255 - a) + 127) // 255 for d, c in zip(dst, colour))


def fit(dst, colour, want):
    """The straight RGBA pixel whose Wine result over dst is exactly want: the alpha nearest the one the colour
    implies, and per channel the colour nearest the nominal one that lands on want (Wine and Skia round apart)."""
    spans = [abs(c - d) for c, d in zip(colour, dst)]
    nominal = round(255 * max((abs(w - d) / s for w, d, s in zip(want, dst, spans) if s), default=0))
    best = None
    for a in sorted(range(1, 256), key=lambda a: abs(a - nominal))[:12]:
        pixel = []
        for d, c, w in zip(dst, colour, want):
            rest = (d * (255 - a) + 127) // 255
            pixel.append(min(range(256), key=lambda v: (abs(((v * (a + 1)) >> 8) + rest - w), abs(v - c))))
        error = max(abs(x - y) for x, y in zip(wine(dst, pixel, a), want))
        if best is None or error < best[0]: best = (error, (*pixel, a))
        if not error: break
    if best[0]: raise ValueError(f'no pixel reaches {want} over {dst} from {colour}')
    return best[1]


def rules(theme, kind, state, active):
    """The prototype's colours for one button state: (plate colour, alpha) or None, and (glyph colour, alpha)."""
    primary, secondary, disabled = (token(theme, n) for n in
                                    ('TextFillColorPrimary', 'TextFillColorSecondary', 'TextFillColorDisabled'))
    if state == 'disabled': return None, disabled
    if state == 'hot':
        return ((RED, 1.0), ((255, 255, 255), 1.0)) if kind == 'close' else (token(theme, 'SubtleFillColorSecondary'), primary)
    if state == 'pushed':
        return ((RED, 0.9), ((255, 255, 255), 0.7)) if kind == 'close' else (token(theme, 'SubtleFillColorTertiary'), secondary)
    return None, primary if active else disabled


def button_states(theme, kind):
    """Per image (normal, hot, pushed, disabled; then inactive): plate RGBA, and the glyph's 10 x 10 RGBA pixels,
    fitted to the prototype's measured pixels (targets in glyphs.json) over the reference bar."""
    targets = json.loads((ROOT / 'themes/windows11/caption/glyphs.json').read_text())['targets'][theme][GLYPH[kind]]
    bar = BAR[theme]
    images = []
    for index, target in enumerate(targets):
        plate, (ink, _) = rules(theme, kind, STATES[index % 4], index < 4)
        want = tuple(target['plate'])
        plate_rgba = fit(bar, plate[0], want) if want != bar else (0, 0, 0, 0)
        base = wine(bar, plate_rgba[:3], plate_rgba[3]) if plate_rgba[3] else bar
        glyph = [[fit(base, ink, tuple(target['glyph'][y][x])) if tuple(target['glyph'][y][x]) != base else (*ink, 0)
                  for x in range(10)] for y in range(10)]
        images.append((plate_rgba, glyph))
    return images


def render(theme, kind, image, bar=None):
    """The 46 x 32 button exactly as Wine composes it over a bar (default: the reference bar)."""
    bar = bar or BAR[theme]
    (pr, pg, pb, pa), glyph = image
    base = wine(bar, (pr, pg, pb), pa) if pa else bar
    pixels = [[base] * 46 for _ in range(32)]
    for y in range(10):
        for x in range(10):
            r, g, b, a = glyph[y][x]
            if a: pixels[11 + y][18 + x] = wine(base, (r, g, b), a)
    return pixels


if __name__ == '__main__':
    # Self-check: over the reference bar every image Wine composes is the prototype's measured button, to the level.
    for theme in BAR:
        for kind in GLYPH:
            targets = json.loads((ROOT / 'themes/windows11/caption/glyphs.json').read_text())['targets'][theme][GLYPH[kind]]
            for index, image in enumerate(button_states(theme, kind)):
                got = render(theme, kind, image)
                assert got[2][2] == tuple(targets[index]['plate']), (theme, kind, index)
                for y in range(10):
                    for x in range(10):
                        assert got[11 + y][18 + x] == tuple(targets[index]['glyph'][y][x]), (theme, kind, index, x, y)
    print('caption buttons: 64 images equal the prototype over the reference bar')
