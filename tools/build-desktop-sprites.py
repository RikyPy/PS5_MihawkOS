#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The desktop's icons as the approved prototype's own pixels, for src/pw_desktop_icons.h (comctl32, Wine patch 1105).

themes/windows11/desktop holds what tools/measure-prototype-sprites.mjs and tools/measure-prototype-glyphs.mjs captured
from prototypes/windows11-shell at 1920 x 1080 (PROTOTYPE_SETUP as given; FIXTURES adds nine desktop folders t0..t8 named
as in LABEL_NAMES below), each over black and over white, gzipped:

  plates-{light,dark}.json.gz  the item boxes with their children hidden, merged from three captures:
      ... - <theme> 'hover=#desk-item-recycle!*~hover' 'hover-2=#desk-item-sc-game!*~hover'
      PROTOTYPE_SETUP="Shell.desktop.select(['recycle', 'sc-game']); document.querySelector('.desk').focus()"
          ... - <theme> 'selected=#desk-item-recycle!*' 'selected-2=#desk-item-sc-game!*'
      PROTOTYPE_SETUP="Shell.desktop.select(['recycle', 'sc-game']); document.activeElement.blur()"
          ... - <theme> 'inactive=#desk-item-recycle!*' 'selected-hover=#desk-item-recycle!*~hover' 'inactive-2=#desk-item-sc-game!*'
      (a selected item under the pointer has the same colours focused or not: .desk-item.sel:hover)
  band.json.gz  PROTOTYPE_SETUP shows .desk-band at 20,40, 380 x 290 (the probe's drag): ... - light 'band=.desk-band'
  label-{text,shadow-1,shadow-2}.json.gz  every printable ASCII character and "…" in each layer of a .desk-label: a clone of
      the first label with text-shadow none, then color transparent under each shadow alone (PROTOTYPE_SETUP), measured
      with measure-prototype-glyphs.mjs light '#pw-label-<layer>' <chars>
  labels.json.gz  PROTOTYPE_SETUP=FIXTURES: the items with their icon hidden ('<name>=#desk-item-<id>!.desk-icon'):
      This PC, Recycle Bin, Example Game, Demo Game, Read me, t0..t7; labels-full.json.gz: t8 selected and focused,
      its plate's colours off, showing its whole name.
  sizes.json.gz  the same at small and large icons (PROTOTYPE_SETUP="Shell.set('iconSize', <size>)", with only t0 for
      FIXTURES): 'hover-<size>' and 'selected-<size>' plates, and '<size>_<name>' labels (icon and badge hidden) for This PC,
      Recycle Bin, Example Game, Demo Game and t0.

The plates are the same in light and dark (the desktop's own tokens) and at every icon size, which this checks, and each
is a nine-slice: 4 px corners, then a uniform edge and fill, which this checks against every capture. The glyphs are one colour per layer
(white text, black shadows), so they are kept as alpha (src/pw_text_glyphs.h). Written to
.deps/windows11-theme/pw_desktop_sprites.h, and the captured labels to pw_desktop_sprite_checks.h for
tests/test_pw_desktop_icons.c.
"""
import base64
import gzip
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'themes/windows11/desktop'
OUTPUT = ROOT / '.deps/windows11-theme'
PLATES = ['hover', 'selected', 'selected-hover', 'inactive']
SLICE = 4
LAYERS = ['shadow-2', 'shadow-1', 'text']      # drawn in this order: CSS paints the last shadow first, the text last
COLOURS = {'shadow-2': 0xff000000, 'shadow-1': 0xff000000, 'text': 0xffffffff}
SIZES = {'small': 32, 'medium': 48, 'large': 96}
LABEL_NAMES = {'this-pc': 'This PC', 'recycle-bin': 'Recycle Bin', 'example-game': 'Example Game', 'demo-game': 'Demo Game',
               'read-me': 'Read me', 't0': 'Archive (2026-10-08)', 't1': 'Supercalifragilisticexpialidocious',
               't2': 'A very long folder name that needs three lines', 't3': 'Example Game Gold Deluxe Edition',
               't4': 'Two  spaces', 't5': 'Program Files (x86)', 't6': 'x', 't7': 'WWWWWWWWWWWWWWWWWWWWWWWWWWWW',
               't8': 'The full name of a selected item'}


def load(name):
    return json.loads(gzip.decompress((SOURCE / f'{name}.json.gz').read_bytes()))


def premultiplied(sprite):
    """(alpha, red, green, blue) per pixel, row by row: over black is the premultiplied colour, white minus black 255 - alpha."""
    black, white = base64.b64decode(sprite['black']), base64.b64decode(sprite['white'])
    pixels = []
    for i in range(sprite['w'] * sprite['h']):
        alphas = [255 - (white[i * 3 + k] - black[i * 3 + k]) for k in range(3)]
        if max(alphas) - min(alphas) > 2:
            raise ValueError(f'pixel {i}: per-channel alpha {alphas} cannot be one AlphaBlend alpha')
        alpha = max(0, min(255, round(sum(alphas) / 3)))
        pixels.append((alpha, *(min(alpha, black[i * 3 + k]) for k in range(3))))
    return pixels


def word(pixel):
    alpha, red, green, blue = pixel
    return alpha << 24 | red << 16 | green << 8 | blue


def words(values, per_line=12):
    return ',\n'.join(','.join(f'0x{w:08x}' for w in values[i:i + per_line]) for i in range(0, len(values), per_line))


def nine_slice(sprite, name):
    """The plate's (2 * SLICE + 1) square, checked to rebuild the capture exactly."""
    w, h, pixels = sprite['w'], sprite['h'], premultiplied(sprite)
    side = 2 * SLICE + 1
    pick = lambda i, n: i if i < SLICE else side - (n - i) if i >= n - SLICE else SLICE
    rows = [[pixels[y * w + x] for x in [*range(SLICE + 1), *range(w - SLICE, w)]] for y in [*range(SLICE + 1), *range(h - SLICE, h)]]
    for y in range(h):
        for x in range(w):
            if pixels[y * w + x] != rows[pick(y, h)][pick(x, w)]:
                raise ValueError(f'{name}: pixel {x},{y} is not its nine-slice\'s')
    return [word(p) for row in rows for p in row]


def write(path, lines):
    text = '\n'.join(lines + [''])
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_text() != text:
        path.write_text(text)


def main():
    plates = {theme: load(f'plates-{theme}')['sprites'] for theme in ('light', 'dark')}
    for name in plates['light']:
        if premultiplied(plates['light'][name]) != premultiplied(plates['dark'][name]):
            raise ValueError(f'{name}: the plate differs between light and dark')
    slices = {}
    for name in PLATES:
        slices[name] = nine_slice(plates['light'][name], name)
        two = plates['light'].get(f'{name}-2')
        if two and nine_slice(two, f'{name}-2') != slices[name]:
            raise ValueError(f'{name}: the two-line plate is another nine-slice')
    sizes = load('sizes')['sprites']
    for key, sprite in sizes.items():
        state, _, size = key.rpartition('-')
        if '_' not in key and nine_slice(sprite, key) != slices[state]:
            raise ValueError(f'{key}: the plate at {size} icons is another nine-slice')
    band = load('band')['sprites']['band']
    band_pixels = premultiplied(band)
    border, fill = word(band_pixels[0]), word(band_pixels[band['w'] + 1])
    for y in range(band['h']):
        for x in range(band['w']):
            edge = x in (0, band['w'] - 1) or y in (0, band['h'] - 1)
            if word(band_pixels[y * band['w'] + x]) != (border if edge else fill):
                raise ValueError(f'band: pixel {x},{y} is neither its border nor its fill')

    out = ['/* Generated by tools/build-desktop-sprites.py from themes/windows11/desktop: do not edit. */',
           '#ifndef PW_DESKTOP_SPRITES_H', '#define PW_DESKTOP_SPRITES_H', '#include "pw_text_glyphs.h"',
           'enum {' + ', '.join(f'PW_DESKTOP_PLATE_{n.upper().replace("-", "_")}' for n in PLATES) + ', PW_DESKTOP_PLATES};',
           f'static const unsigned int pw_desktop_plates[PW_DESKTOP_PLATES][{(2 * SLICE + 1) ** 2}] = {{']
    out += [f'{{{words(slices[name], 9)}}},' for name in PLATES] + ['};']
    out += [f'#define PW_DESKTOP_BAND_BORDER 0x{border:08x}u', f'#define PW_DESKTOP_BAND_FILL 0x{fill:08x}u']
    fonts = {layer: load(f'label-{layer}') for layer in LAYERS}
    if any(set(fonts[layer]['glyphs']) != set(fonts['text']['glyphs']) for layer in LAYERS):
        raise ValueError('the layers measured different characters')
    for number, layer in enumerate(LAYERS):
        entries = []
        for index, (ch, glyph) in enumerate(sorted(fonts[layer]['glyphs'].items())):
            if glyph['advance'] * 65536 != round(glyph['advance'] * 65536):
                raise ValueError(f'{layer} {ch!r}: advance {glyph["advance"]} is not a 16.16 value')
            phases = []
            for phase, image in enumerate(glyph['phases']):
                if not image['w']:
                    phases.append('{0, 0, 0, 0, 0, 0}')
                    continue
                pixels = premultiplied(image)
                tint = COLOURS[layer] & 0xff
                if any(abs(colour - (alpha * tint + 127) // 255) > 1 for alpha, *rgb in pixels for colour in rgb):
                    raise ValueError(f'{layer} {ch!r}: not one colour')
                name = f'pw_desktop_glyph_{number}_{index}_{phase}'
                out.append(f'static const unsigned char {name}[] = {{{",".join(str(p[0]) for p in pixels)}}};')
                phases.append(f'{{{image["x"]}, {image["y"]}, {image["w"]}, {image["h"]}, 0, {name}}}')
            entries.append(f'    {{0x{ord(ch):04x}, {round(glyph["advance"] * 65536)}, {{{", ".join(phases)}}}}},')
        out += [f'static const PwGlyph pw_desktop_glyphs_{number}[] = {{'] + entries + ['};']
    out.append('/* [0] the 0 0 2px shadow, [1] the 1px 1px 2px shadow, [2] the text: drawn in this order. */')
    out.append('static const PwGlyphFont pw_desktop_label_fonts[3] = {')
    for number, layer in enumerate(LAYERS):
        font = fonts[layer]['font']
        out.append(f'    {{pw_desktop_glyphs_{number}, {len(fonts[layer]["glyphs"])}, {font["baseline"]}, {round(font["line"])}, '
                   f'0x{COLOURS[layer]:08x}u}},')
    out += ['};', '#endif']
    write(OUTPUT / 'pw_desktop_sprites.h', out)

    labels = {**load('labels')['sprites'], **load('labels-full')['sprites']}
    checks = ['/* Generated by tools/build-desktop-sprites.py from themes/windows11/desktop: do not edit. */',
              '#ifndef PW_DESKTOP_SPRITE_CHECKS_H', '#define PW_DESKTOP_SPRITE_CHECKS_H',
              '/* A label as Chromium drew it (premultiplied BGRA, the item\'s box with its icon hidden) at an icon size, and whether',
              ' * it is clamped. */',
              'struct pw_desktop_label_check { const char *name; int icon, clamped, w, h; const unsigned int *bgra; };']
    rows = []
    cases = [('medium', key, labels[key]) for key in LABEL_NAMES] + \
            [(*key.split('_'), sprite) for key, sprite in sizes.items() if '_' in key]
    for index, (size, key, sprite) in enumerate(cases):
        if sprite['w'] != max(75, SIZES[size] + 27) - 2:
            raise ValueError(f'{size} {key}: the item is {sprite["w"]} wide')
        checks.append(f'static const unsigned int pw_desktop_label_check_{index}[] = {{\n{words([word(p) for p in premultiplied(sprite)])}}};')
        rows.append(f'    {{{json.dumps(LABEL_NAMES[key])}, {SIZES[size]}, {0 if key == "t8" else 1}, {sprite["w"]}, {sprite["h"]}, '
                    f'pw_desktop_label_check_{index}}},')
    checks += ['static const struct pw_desktop_label_check pw_desktop_label_checks[] = {'] + rows + ['};', '#endif']
    write(OUTPUT / 'pw_desktop_sprite_checks.h', checks)
    print(json.dumps({'plates': len(PLATES), 'glyphs': len(fonts['text']['glyphs']), 'labels': len(rows),
                      'band': [f'0x{border:08x}', f'0x{fill:08x}'],
                      'header': str((OUTPUT / 'pw_desktop_sprites.h').relative_to(ROOT))}))


if __name__ == '__main__':
    main()
