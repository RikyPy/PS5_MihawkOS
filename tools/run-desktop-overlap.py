#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Qualify window occlusion against the console's displayed pixels."""
import argparse
import json
import subprocess
import sys
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--name', required=True)
    args = parser.parse_args()
    result = subprocess.run([sys.executable, str(ROOT / 'tools/run-console.py'), '--name', args.name,
        '--exe', 'desktop-overlap-probe.exe', '--marker', 'PS5_PROTON_DESKTOP_OVERLAP_PROBE',
        '--no-stdout', '--pad-script', str(ROOT / 'tests/pad-scripts/desktop-overlap.txt')], cwd=ROOT)
    path = ROOT / 'evidence' / (args.name + '.json')
    if not path.exists():
        return result.returncode or 1
    record = json.loads(path.read_text())
    pixels = []
    expected_desktop = (0, 0, 0)
    wallpaper = ROOT / '.deps/staged/runtime/ge11-7/lib/wine/prefix/drive_c/windows/web/wallpaper/proton.bmp'
    if wallpaper.is_file():
        with Image.open(wallpaper) as background:
            expected_desktop = background.convert('RGB').getpixel((1600, 900))
    for index, capture in enumerate(record.get('captures', [])):
        with Image.open(ROOT / capture) as image:
            image = image.convert('RGB')
            w, h = image.size
            # Production presents the whole 1920x1080 desktop. Sample the
            # probe's window coordinates, accounting for the TV output size.
            center = image.getpixel((700 * w // 1920, 450 * h // 1080))
            outside = image.getpixel((380 * w // 1920, 260 * h // 1080))
            desktop = image.getpixel((1600 * w // 1920, 900 * h // 1080))
            expected = (0x20,0xe0,0x40) if index < 3 else (0x20,0xa0,0xe0)
            pixels.append({'capture': capture, 'center': center, 'expected_center': expected, 'outside': outside,
                'desktop': desktop, 'expected_desktop': expected_desktop,
                'passed': center == expected and outside == (0x20,0xa0,0xe0) and desktop == expected_desktop})
    record['display_pixels'] = pixels
    record['passed'] = record['passed'] and len(pixels) == 4 and all(p['passed'] for p in pixels)
    path.write_text(json.dumps(record, indent=1) + '\n')
    print(json.dumps({'display_pixels': pixels, 'passed': record['passed']}, indent=1))
    return 0 if record['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
