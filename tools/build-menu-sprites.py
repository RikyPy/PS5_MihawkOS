#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Menus as the approved prototype draws them: its fixed pixels, for Wine's popup menus (win32u, patch 1190) and the
desktop's menu icons (shell32, patch 1191).

themes/windows11/menus/{desktop,kbd,submenu,item,new,paste}-{light,dark}.json.gz are the prototype's menus (css/ui.css .menu)
captured by tools/measure-prototype-sprites.mjs at 1920 x 1080 with the menu 268 px wide, so that every right-anchored
part sits on a whole pixel as it does in a native menu (PROTOTYPE_SETUP adds `.menu{width:268px!important}`):

  desktop: ... menu-desktop <theme> 'hover=.menu-list > .menu-item:nth-child(3)!.menu-icon,.menu-label~hover'
           'press=<the same>~press' 'rows=.menu-list!.menu-label,.menu-shortcut'
  kbd:     (PROTOTYPE_SETUP also sends ArrowDown three times) ... menu-desktop <theme>
           'kbd=.menu-list!.menu-icon,.menu-label,.menu-chevron,.menu-shortcut,.menu-separator'
  submenu: (ArrowDown, ArrowRight: View's submenu) ... menu-desktop <theme>
           'checks=.menu ~ .menu .menu-list!.menu-label,.menu-shortcut,.menu-separator'
  item:    ... menu-desktop-item <theme> 'commands=.menu-commandbar' 'rows=.menu-list!.menu-label,.menu-shortcut,.menu-chevron'
           'command-hover=.menu-command:nth-child(1)!.icon~hover' 'command-press=<the same>~press'
  new:     (the plates hidden: .menu-item::before{display:none}; ArrowDown four times, ArrowRight: New's submenu)
           ... menu-desktop <theme> 'icons=.menu ~ .menu .menu-list!.menu-label,.menu-separator'
  paste:   (plates hidden; Shell.clipboard.set(['f-readme']), then a contextmenu event at 600,400: the Paste command)
           ... - <theme> 'commands=.menu-commandbar'

and the glyphs of its text (tools/measure-prototype-glyphs.mjs, PROTOTYPE_SETUP: a contextmenu event at 600,400), every
printable ASCII character and "…" alone at Skia's four pen phases, with strings Chromium laid out to check against:

  label-{light,dark}.json.gz:    ... <theme> .menu-label <chars> 'Show more options' 'Display settings' 'Open in Terminal' View
  shortcut-{light,dark}.json.gz: ... <theme> .menu-shortcut <chars> Shift+F10 Ctrl+Shift+C Alt+Enter

A row is 32 px tall and 258 px wide here, 4 px inside the list (.menu-item margin 0 4px). A plate is cut where its
columns stop changing: a left piece, one middle column the native menu repeats, a right piece anchored at the row's
right edge (the hover plate's corners are exact mirrors; the keyboard plate's ring is cut the same way). Icons are the
16 x 16 box Chromium snapped them to (row + 11, row + 8; a command's cell + 12, + 12); nothing they draw lies outside
it but anti-aliasing, kept in a 2 px margin (20 x 20 bitmaps a menu centres on its 16 px column), which the build
checks. Over black a pixel is its premultiplied colour, white minus black is 255 minus its alpha.

The glyphs of one capture are one colour (TextFillColorPrimary or Secondary: black in light, white in dark, each at its own
alpha, which the capture holds), so they are kept as alpha that opaque black or white tints (src/pw_text_glyphs.h).

Writes .deps/windows11-theme/pw_menu_sprites.h (the chrome, for win32u: types from src/pw_menu_style.h),
pw_menu_glyphs.h (the label and shortcut fonts, for win32u), pw_menu_icons.h (the desktop menus' icons, for Explorer) and
pw_menu_reference.h (the plates and strings as captured, which tests/test_pw_menu_style.c draws again).
"""
import base64
import gzip
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'themes/windows11/menus'
OUTPUT = ROOT / '.deps/windows11-theme'
ROW, ROW_LEFT, ROW_RIGHT = 32, 4, 262          # list coordinates of a row in a 268 px menu
CHECK_X = ROW_LEFT + 11                          # the check or icon column
# The desktop's menus, in their list's coordinates: rows of menu-desktop and of the first file's menu.
DESKTOP_ROWS = {'view': 0, 'sort': 32, 'refresh': 64, 'new': 101, 'display': 138, 'personalize': 170,
                'terminal': 207, 'more': 244}
ITEM_ROWS = {'open-with': 32, 'copy-path': 96, 'properties': 128, 'notepad': 165}   # Compress has no native action
NEW_ROWS = {'folder': 0, 'text': 69}             # New's submenu: Folder, Shortcut, a separator, Text Document
COMMANDS = ['cut', 'copy', 'rename', 'share', 'delete']
GLYPHS = ('label', 'shortcut')                   # 14 px TextFillColorPrimary labels, 12 px TextFillColorSecondary shortcuts
TINTS = {'light': 0xff000000, 'dark': 0xffffffff}
ICONS = list(DESKTOP_ROWS) + list(ITEM_ROWS) + list(NEW_ROWS) + COMMANDS + ['paste']


class Sprite:
    """A capture as premultiplied BGRA words with its own coordinates."""

    def __init__(self, capture):
        self.w, self.h = capture['w'], capture['h']
        black, white = base64.b64decode(capture['black']), base64.b64decode(capture['white'])
        self.words = []
        for i in range(self.w * self.h):
            alphas = [255 - (white[i * 3 + k] - black[i * 3 + k]) for k in range(3)]
            if max(alphas) - min(alphas) > 2:
                raise ValueError(f'pixel {i}: per-channel alpha {alphas} cannot be one alpha')
            alpha = max(0, min(255, round(sum(alphas) / 3)))
            red, green, blue = (min(alpha, black[i * 3 + k]) for k in range(3))
            self.words.append(alpha << 24 | red << 16 | green << 8 | blue)

    def at(self, x, y):
        return self.words[y * self.w + x]

    def box(self, x0, y0, x1, y1):
        """The alpha bounding box inside [x0, x1) x [y0, y1), or None."""
        found = [(x, y) for y in range(y0, y1) for x in range(x0, x1) if self.at(x, y) >> 24]
        if not found:
            return None
        return (min(x for x, _ in found), min(y for _, y in found), max(x for x, _ in found) + 1, max(y for _, y in found) + 1)

    def crop(self, x0, y0, x1, y1):
        return [self.at(x, y) for y in range(y0, y1) for x in range(x0, x1)]


def load(part, theme):
    return {name: Sprite(capture) for name, capture in
            json.loads(gzip.decompress((SOURCE / f'{part}-{theme}.json.gz').read_bytes()))['sprites'].items()}


def plate(sprite, y0, y1, left, right):
    """A plate in rows [y0, y1) of the sprite whose row spans [left, right): left piece, middle column, right piece,
    each {x, y, w, h, words} with x from the row's left (left piece, middle) or right (right piece) edge, y from its top."""
    x0, py0, x1, py1 = sprite.box(0, y0, sprite.w, y1)
    column = lambda x: sprite.crop(x, py0, x + 1, py1)
    middle = (x0 + x1) // 2
    a = x0
    while column(a) != column(middle):
        a += 1
    b = x1
    while column(b - 1) != column(middle):
        b -= 1
    if a >= b:
        raise ValueError('a plate with no repeated middle')
    piece = lambda xa, xb, origin: {'x': xa - origin, 'y': py0 - y0, 'w': xb - xa, 'h': py1 - py0,
                                    'words': sprite.crop(xa, py0, xb, py1)}
    return {'left': piece(x0, a, left), 'middle': piece(middle, middle + 1, left), 'right': piece(b, x1, right)}


def anchored(sprite, x0, y0, x1, y1, origin_x, origin_y):
    """The sprite's ink inside [x0, x1) x [y0, y1), placed from (origin_x, origin_y)."""
    bx0, by0, bx1, by1 = sprite.box(x0, y0, x1, y1)
    return {'x': bx0 - origin_x, 'y': by0 - origin_y, 'w': bx1 - bx0, 'h': by1 - by0, 'words': sprite.crop(bx0, by0, bx1, by1)}


def icon(sprite, x, y, area):
    """The 16 x 16 icon at (x, y) with a 2 px margin for its anti-aliasing; area is the region its ink may not leave."""
    inside = sprite.box(x - 2, y - 2, x + 18, y + 18)
    whole = sprite.box(*area)
    if inside != whole:
        raise ValueError(f'icon at {x},{y}: ink {whole} leaves its 20 x 20 box')
    return sprite.crop(x - 2, y - 2, x + 18, y + 18)


def chrome(theme):
    desktop, kbd, submenu, item, new, paste = (load(part, theme) for part in ('desktop', 'kbd', 'submenu', 'item', 'new', 'paste'))
    rows = desktop['rows']
    checks = submenu['checks']
    separator = rows.at(100, 98)
    if any(rows.at(x, 98) != separator for x in range(0, rows.w)) or rows.at(100, 97) or rows.at(100, 99):
        raise ValueError('the separator is not one flat 1 px line')
    return {
        'hover': plate(desktop['hover'], 0, ROW, 0, ROW_RIGHT - ROW_LEFT),
        'keyboard': plate(kbd['kbd'], 64, 64 + ROW, ROW_LEFT, ROW_RIGHT),
        'command_hover': anchored(item['command-hover'], 0, 0, 40, 40, 0, 0),
        'chevron': anchored(rows, 200, 0, rows.w, ROW, ROW_RIGHT, 0),
        'radio': anchored(checks, ROW_LEFT, 32, 60, 64, CHECK_X, 32),
        'check': anchored(checks, ROW_LEFT, 133, 60, 165, CHECK_X, 133),
        'separator': separator,
    }, {
        **{name: icon(rows, CHECK_X, y + 8, (ROW_LEFT, y, 60, y + ROW)) for name, y in DESKTOP_ROWS.items()},
        **{name: icon(item['rows'], CHECK_X, y + 8, (ROW_LEFT, y, 60, y + ROW)) for name, y in ITEM_ROWS.items()},
        **{name: icon(new['icons'], CHECK_X, y + 8, (ROW_LEFT, y, 60, y + ROW)) for name, y in NEW_ROWS.items()},
        **{name: icon(item['commands'], 2 + 40 * i + 12, 12, (2 + 40 * i, 0, 2 + 40 * i + 40, 40))
           for i, name in enumerate(COMMANDS)},
        'paste': icon(paste['commands'], 2 + 12, 12, (2, 0, 42, 40)),
    }


def glyph_font(theme, name, out):
    """Appends one capture's glyphs as alpha arrays to out; returns its PwGlyphFont initialiser and its check strings."""
    capture = json.loads(gzip.decompress((SOURCE / f'{name}-{theme}.json.gz').read_bytes()))
    entries, tint = [], TINTS[theme] & 0xff
    for index, (ch, glyph) in enumerate(sorted(capture['glyphs'].items())):
        if glyph['advance'] * 65536 != round(glyph['advance'] * 65536):
            raise ValueError(f'{theme} {name} {ch!r}: advance {glyph["advance"]} is not a 16.16 value')
        phases = []
        for phase, image in enumerate(glyph['phases']):
            if not image['w']:
                phases.append('{0, 0, 0, 0, 0, 0}')
                continue
            pixels = Sprite(image).words
            if any(abs((w >> k & 255) - ((w >> 24) * tint + 127) // 255) > 1 for w in pixels for k in (0, 8, 16)):
                raise ValueError(f'{theme} {name} {ch!r}: not one colour')
            array = f'pw_menu_glyph_{theme}_{name}_{index}_{phase}'
            out.append(f'static const unsigned char {array}[] = {{{",".join(str(w >> 24) for w in pixels)}}};')
            phases.append(f'{{{image["x"]}, {image["y"]}, {image["w"]}, {image["h"]}, 0, {array}}}')
        entries.append(f'    {{0x{ord(ch):04x}, {round(glyph["advance"] * 65536)}, {{{", ".join(phases)}}}}},')
    out += [f'static const PwGlyph pw_menu_glyphs_{theme}_{name}[] = {{', *entries, '};']
    font = capture['font']
    return (f'{{pw_menu_glyphs_{theme}_{name}, {len(entries)}, {font["baseline"]}, {round(font["line"])}, 0x{TINTS[theme]:08x}u}}',
            capture['strings'])


def words(values):
    return ',\n'.join(','.join(f'0x{w:08x}' for w in values[i:i + 12]) for i in range(0, len(values), 12))


def write(path, text):
    if not path.exists() or path.read_text() != text:
        path.write_text(text)


def main():
    themes = {theme: chrome(theme) for theme in ('light', 'dark')}
    arrays, tables = [], []
    for theme, (parts, _) in themes.items():
        def sprite(name, s):
            arrays.append(f'static const unsigned int pw_menu_{theme}_{name}[] = {{\n{words(s["words"])}}};')
            return f'{{{s["x"]}, {s["y"]}, {s["w"]}, {s["h"]}, pw_menu_{theme}_{name}}}'
        fields = []
        for name in ('hover', 'keyboard'):
            fields.append(f'.{name} = {{' + ', '.join(sprite(f'{name}_{piece}', parts[name][piece])
                                                    for piece in ('left', 'middle', 'right')) + '}')
        for name in ('command_hover', 'chevron', 'check', 'radio'):
            fields.append(f'.{name} = {sprite(name, parts[name])}')
        fields.append(f'.separator = 0x{parts["separator"]:08x}')
        tables.append('    {' + ', '.join(fields) + '},')
    OUTPUT.mkdir(parents=True, exist_ok=True)
    write(OUTPUT / 'pw_menu_sprites.h', '\n'.join(
        ['/* Generated by tools/build-menu-sprites.py from themes/windows11/menus: do not edit. */',
         '#ifndef PW_MENU_SPRITES_H', '#define PW_MENU_SPRITES_H', *arrays,
         'static const PwMenuChrome pw_menu_chrome[2] = {', *tables, '};', '#endif', '']))
    reference = ['/* Generated by tools/build-menu-sprites.py from themes/windows11/menus: do not edit. */',
                 '/* The rows as captured, 258 x 32 premultiplied BGRA: hover, keyboard (from 2 px left of the row, 262 wide). */']
    for theme in themes:
        desktop, kbd = load('desktop', theme), load('kbd', theme)
        for name, values in (('hover', desktop['hover'].crop(0, 0, 258, ROW)), ('keyboard', kbd['kbd'].crop(ROW_LEFT - 2, 64, ROW_RIGHT + 2, 64 + ROW))):
            reference.append(f'static const unsigned int pw_menu_reference_{theme}_{name}[] = {{\n{words(values)}}};')
    glyphs = ['/* Generated by tools/build-menu-sprites.py from themes/windows11/menus: do not edit. */',
              '#ifndef PW_MENU_GLYPHS_H', '#define PW_MENU_GLYPHS_H', '#include "pw_text_glyphs.h"']
    fonts, checks = [], []
    for theme in themes:
        row = []
        for number, name in enumerate(GLYPHS):
            font, strings = glyph_font(theme, name, glyphs)
            row.append(font)
            for string in strings:
                array = f'pw_menu_string_{theme}_{name}_{len(checks)}'
                reference.append(f'static const unsigned int {array}[] = {{\n{words(Sprite(string).words)}}};')
                checks.append(f'    {{{int(theme == "dark")}, {number}, {json.dumps(string["text"])}, {round(string["x"] * 65536)}, '
                              f'{string["baseline"]}, {string["x0"]}, {string["y0"]}, {string["w"]}, {string["h"]}, {array}}},')
        fonts.append('    {' + ', '.join(row) + '},')
    glyphs += ['/* [light, dark][label, shortcut] */', 'static const PwGlyphFont pw_menu_fonts[2][2] = {', *fonts, '};', '#endif', '']
    write(OUTPUT / 'pw_menu_glyphs.h', '\n'.join(glyphs))
    reference += ['/* Strings Chromium laid out: theme, font, text, pen x (16.16), baseline, the captured box and its pixels. */',
                  'struct pw_menu_glyph_check { int dark, font; const char *text; int x, baseline, x0, y0, w, h; const unsigned int *bgra; };',
                  'static const struct pw_menu_glyph_check pw_menu_glyph_checks[] = {', *checks, '};']
    write(OUTPUT / 'pw_menu_reference.h', '\n'.join(reference) + '\n')
    enum = ', '.join(f'PW_MENU_ICON_{n.upper().replace("-", "_")}' for n in ICONS)
    icons = []
    for theme, (_, found) in themes.items():
        icons.append('    {' + ',\n     '.join(f'{{{words(found[n])}}}' for n in ICONS) + '},')
    write(OUTPUT / 'pw_menu_icons.h', '\n'.join(
        ['/* Generated by tools/build-menu-sprites.py from themes/windows11/menus: do not edit. */',
         '#ifndef PW_MENU_ICONS_H', '#define PW_MENU_ICONS_H',
         '/* 20 x 20 premultiplied BGRA, top row first, the 16 px icon 2 px in; [0] light, [1] dark. */',
         f'enum {{{enum}, PW_MENU_ICON_COUNT}};',
         'static const unsigned int pw_menu_icons[2][PW_MENU_ICON_COUNT][400] = {', *icons, '};', '#endif', '']))
    print(json.dumps({'icons': len(ICONS), 'themes': 2, 'headers': ['.deps/windows11-theme/pw_menu_sprites.h',
                                                                    '.deps/windows11-theme/pw_menu_icons.h']}))


if __name__ == '__main__':
    main()
