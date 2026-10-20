#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The compositor's material recipe must equal appearance.json's tokens."""
import importlib.util
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def css_shadow(value):
    """A CSS box-shadow list of black layers as (y offset, blur, spread, alpha), bottom layer (listed last) first."""
    layers = []
    for lengths, alpha in re.findall(r'((?:-?\d+(?:px)?\s+){2,4})rgba\(0, 0, 0, ([\d.]+)\)', value):
        x, y, blur, spread = (list(map(int, re.findall(r'-?\d+', lengths))) + [0, 0])[:4]
        assert x == 0, value
        layers.append((y, blur, spread, float(alpha)))
    return layers[::-1]


def main():
    tokens = json.loads((ROOT / 'themes/windows11/appearance.json').read_text())
    materials = tokens['materials']
    header = (ROOT / 'src/pw_window_effects.h').read_text() + (ROOT / 'src/pw_window_snapshot.h').read_text()
    enum = dict((name, int(value)) for name, value in re.findall(r'PW_(\w+)=(\d+)', header))
    expected = {
        'MICA_SCALE': materials['mica_scale'], 'MICA_RADIUS': materials['mica_radius'],
        'ACRYLIC_SCALE': materials['acrylic_scale'], 'ACRYLIC_RADIUS': materials['acrylic_radius'],
        'BLUR_PASSES': materials['blur_passes'],
        'MICA_ALT_MARGIN': materials['mica_alt']['margin'], 'MICA_ALT_RADIUS': materials['mica_alt']['radius'],
        'MICA_ALT_LUMINOSITY': int(materials['mica_alt']['luminosity'][1:3], 16),
        'MICA_ALT_LUMINOSITY_DARK': int(materials['mica_alt']['luminosity_dark'][1:3], 16),
        'SHADOW_SMALL_SIZE': materials['shadow_small']['size'],
        'SHADOW_SMALL_ALPHA': materials['shadow_small']['alpha'], 'SHADOW_SMALL_OFFSET': materials['shadow_small']['offset'],
    }
    for name, value in expected.items():
        assert enum.get(name) == value, (name, enum.get(name), value)
    table = re.search(r'pw_material_tints\[7\]=\{(.*?)\};', header, re.S).group(1)
    rows = re.findall(r'\{(0x[0-9a-f]+|0),(0x[0-9a-f]+|0),(\d+),(\d+)\}', table)
    assert len(rows) == 7, rows
    for index, name in ((1, 'mica'), (2, 'acrylic'), (3, 'mica_alt'), (4, 'taskbar'), (5, 'flyout_base'), (6, 'flyout')):
        light, dark, tint, tint_dark = rows[index]
        material = materials[name]
        assert int(light, 16) == int(material['light'][1:], 16), (name, 'light')
        assert int(dark, 16) == int(material['dark'][1:], 16), (name, 'dark')
        assert int(tint) == material['tint'], (name, 'tint')
        assert int(tint_dark) == material.get('tint_dark', material['tint']), (name, 'tint_dark')
        assert 0 <= material['tint'] <= 255
    # The window shadow: appearance.json is the prototype's tokens.css, and the C table is appearance.json with each
    # alpha*255 rounded half up, as Chromium makes the colour.
    css = (ROOT / 'prototypes/windows11-shell/css/tokens.css').read_text()
    themes = [dict(re.findall(r'--(shadow-window(?:-inactive)?):\s*([^;]+);', part)) for part in css.split('[data-theme="dark"]')]
    table = re.search(r'pw_window_shadow_layers\[2\]\[2\]\[3\]=\{(.*?)\};', header, re.S).group(1)
    layers = [tuple(map(int, layer)) for layer in re.findall(r'\{(\d+),(\d+),(\d+),(\d+)\}', table)]
    assert len(layers) == 12, layers
    for active, state in ((0, 'inactive'), (1, 'active')):
        for dark, theme in ((0, 'light'), (1, 'dark')):
            ours = [(l['offset'], l['blur'], l['spread'], l['alpha']) for l in materials['shadow_window'][state][theme]]
            assert ours == css_shadow(themes[dark]['shadow-window' + ('' if active else '-inactive')]), (state, theme)
            c = layers[(active * 2 + dark) * 3:(active * 2 + dark) * 3 + 3]
            assert c == [(o, b, s, int(a * 255 + 0.5)) for o, b, s, a in ours], (state, theme, c)
    # The flyout shadow, the same way: --shadow-28 (light and dark), appearance.json, pw_flyout_shadow_layers.
    flyout = re.search(r'pw_flyout_shadow_layers\[2\]\[2\]=\{(.*?)\};', header, re.S).group(1)
    flyout_layers = [tuple(map(int, layer)) for layer in re.findall(r'\{(\d+),(\d+),(\d+),(\d+)\}', flyout)]
    shadow_28 = [dict(re.findall(r'--(shadow-28):\s*([^;]+);', part)) for part in css.split('[data-theme="dark"]')]
    for dark, theme in ((0, 'light'), (1, 'dark')):
        ours = [(l['offset'], l['blur'], l['spread'], l['alpha']) for l in materials['shadow_flyout'][theme]]
        assert ours == css_shadow(shadow_28[dark]['shadow-28']), theme
        assert flyout_layers[dark * 2:dark * 2 + 2] == [(o, b, s, int(a * 255 + 0.5)) for o, b, s, a in ours], theme
    # The ring's corners: what tools/skia-rrect-coverage.py computes for the shadow layers' spread around the window.
    spec = importlib.util.spec_from_file_location('skia_rrect', ROOT / 'tools/skia-rrect-coverage.py')
    skia = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(skia)
    ring = re.search(r'pw_window_ring_corners\[4\]\[PW_SHADOW_RING_RADIUS\]\[PW_SHADOW_RING_RADIUS\]=\s*(\{.*?\});', header, re.S).group(1)
    assert enum['SHADOW_RING_RADIUS'] == tokens['metrics']['window_radius'] + 1
    assert json.loads(ring.replace('{', '[').replace('}', ']')) == skia.ring_corners(enum['SHADOW_RING_RADIUS'])
    radius = tokens['metrics']
    assert enum['WINDOW_RADIUS_LIMIT'] >= radius['window_radius'] >= radius['control_radius'] > 0
    print('window material tokens: PASS', len(expected) + 24, 'values match appearance.json')


if __name__ == '__main__':
    main()
