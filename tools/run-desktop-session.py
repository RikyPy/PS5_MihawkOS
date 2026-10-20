#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check persistent shell semantics and the actual displayed child windows."""
import argparse
import json
import re
import subprocess
import sys
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import numpy as np  # noqa: E402
import pw_explorer_expected  # noqa: E402


def explorer_frame_verdict(image, log, what='explorer', state='home'):
    """File Explorer on Home (capture 3), on Documents (capture 4) and on Documents in large icons, small icons and list
    (captures 5 to 7, what and state 'large', 'small', 'list') against the approved prototype's own pixels
    (tools/pw_explorer_expected.py): the window where the prototype's is (1100 x 700 at 410,166) and active; its header,
    navigation pane, Home page above the Recent rows (or a folder's column header) and status buttons equal to the
    prototype's sprites over their surfaces within 2 levels (in an icon view the first tile's icon), and the title bar's
    opaque sprite pixels (the tab, the caption glyphs) too. Mica Alt, the border, the location's names and the files are not compared."""
    found = re.findall(r'desktop-session ' + what + r' frame=(-?\d+),(-?\d+),(-?\d+),(-?\d+) client=[-\d,]+ active=(\d) '
                       r'bar=(\d+),(\d+),(\d+) pointer=(-?\d+),(-?\d+)', log)
    if not found: return {'passed': False, 'error': f'no {what} frame line'}
    l, t, r, b, active, red, green, blue, px, py = map(int, found[-1])
    theme = 'light' if red + green + blue > 384 else 'dark'  # the theme variant's caption colour
    result = pw_explorer_expected.compare(np.asarray(image.convert('RGB')), l + 1, t + 1, theme, (px, py), state)
    placed = (l, t, r, b) == (410, 166, 1510, 866)
    return {'frame': [l, t, r, b], 'active': active, 'theme': theme, **result,
            'passed': placed and bool(active) and result['compared'] > 100000 and result['wrong'] == 0}


# Captures 3 on: File Explorer on Home, on Documents, Documents in each icon view, with the details pane and Filter on,
# with the pane and an item selected, then a second tab on Home.
STATES = [('explorer', 'home'), ('folder', 'documents'), *[(v, v) for v in pw_explorer_expected.VIEWS], ('filter', 'filter'),
          ('pane', 'pane'), ('tabs', 'tabs'), ('parent', 'parent')]


def display_verdict(captures, log='', theme='light'):
    pixels = []
    for index, capture in enumerate(captures):
        with Image.open(ROOT / capture) as source:
            image = source.convert('RGB')
            if index < 2:
                expected = (30, 110, 210) if index == 0 else (30, 180, 70)
                actual = image.getpixel((500, 400)) if image.size == (1920, 1080) else None
                pixels.append({'capture': capture, 'architecture': 64 if index == 0 else 32,
                               'actual': actual, 'expected': expected, 'passed': actual == expected})
            else:
                # The probe places the real browser where the prototype's is (1100 x 700 at 410,166).
                # Its body (white, or #1c1c1c in dark) must cover a substantial area; a shell-only
                # wallpaper/taskbar frame has no such pixels in this rectangle.
                body = sum((min(p) >= 200 if theme == 'light' else max(p) <= 60) and max(p) - min(p) < 25
                           for p in image.crop((430, 230, 1490, 850)).getdata())
                what, state = STATES[index - 2]
                frame = explorer_frame_verdict(image, log, what, state)
                pixels.append({'capture': capture, 'window': 'File Explorer ' + {
                                   'home': 'Home', 'documents': 'Documents', 'tabs': 'with a second tab on Home',
                                   'filter': 'Documents filtered, details pane', 'pane': 'details pane on a folder',
                                   'parent': 'gone up from Documents'}.get(
                                   state, f'Documents in {state} view'),
                               'body_pixels': body, 'frame': frame,
                               'passed': image.size == (1920, 1080) and body >= 10000 and frame['passed'] and
                                         frame.get('theme') == theme})
    # Every view File Explorer created was invisible when created (its window held through the navigation), so none
    # could paint before it was styled: Wine's classic list never reaches the screen (patch 1152).
    views = re.findall(r'pw-explorer: view created held=(\d) visible=(\d)', log)
    pixels.append({'views_created': len(views), 'views_visible_when_created': sum(v == '1' for _, v in views),
                   'passed': len(views) >= 3 and all(v == '0' for _, v in views)})
    return pixels, len(pixels) == 3 + len(STATES) and all(p['passed'] for p in pixels)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--name', required=True)
    parser.add_argument('--theme', choices=('light', 'dark'), default='light',
                        help='dark: the probe applies the Dark variant for the run and puts the prior theme back')
    args = parser.parse_args()
    result = subprocess.run([sys.executable, str(ROOT / 'tools/run-console.py'), '--name', args.name,
        '--profile', 'mihawk-os',
        '--args=Z:\\data\\ps5proton\\prefixes\\probe\\drive_c\\desktop-session64-probe.exe --capture=Z:\\data\\ps5proton\\logs\\game-frame.ppm'
        + (' --dark' if args.theme == 'dark' else ''),
        '--marker', 'PS5_PROTON_DESKTOP_SESSION_PROBE', '--seconds', '600', '--watchdog', '660',
        '--pad-script', str(ROOT / 'tests/pad-scripts/desktop-session.txt')], cwd=ROOT)
    path = ROOT / 'evidence' / (args.name + '.json')
    if not path.exists():
        return result.returncode or 1
    record = json.loads(path.read_text())
    log = '\n'.join(log.read_text(errors='replace') for log in (ROOT / 'klog').glob(args.name + '*.log'))
    pixels, passed = display_verdict(record.get('captures', []), log, args.theme)
    record['theme'] = args.theme
    record['session_passed'] = record['passed']
    record['display_pixels'] = pixels
    record['passed'] = record['passed'] and passed
    path.write_text(json.dumps(record, indent=1) + '\n')
    print(json.dumps({'display_pixels': pixels, 'passed': record['passed']}, indent=1))
    return 0 if record['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
