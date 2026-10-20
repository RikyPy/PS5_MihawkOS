#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The shell's surfaces on the console against the approved prototype.

Runs tests/shell-surfaces-probe.c in the desktop session (one launch): it captures the desktop, Start in light and
dark, Search, Quick Settings, the notification centre and the hidden icons, and logs where each surface is. With
--reference, each capture is compared with the prototype's capture of the same surface (measure-prototype-surface.mjs
writes <open>-<theme>.png: start-light, start-dark, search-light, quicksettings-light, notifications-light) over the
surface and the 48 px its shadow reaches, above the taskbar: the share of pixels equal, within one level and within
two, and the worst channel difference. Text the prototype draws with Skia differs until text is drawn the same way;
the record names that, it does not hide it.

  tools/run-shell-surfaces.py --name <run> [--reference DIR]
"""
import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
REFERENCE = {2: 'start-light', 3: 'start-dark', 4: 'search-light', 5: 'quicksettings-light', 6: 'notifications-light'}


def compare(capture, reference, rect):
    """Pixel agreement over the surface's rectangle and its shadow, above the 48 px taskbar."""
    got = np.asarray(Image.open(capture).convert('RGB')).astype(int)
    want = np.asarray(Image.open(reference).convert('RGB')).astype(int)
    left, top, right, bottom = rect
    region = (slice(max(0, top - 48), min(1032, bottom + 48)), slice(max(0, left - 48), min(1920, right + 48)))
    surface = (slice(max(0, top), min(1032, bottom)), slice(max(0, left), min(1920, right)))
    result = {}
    for name, (ys, xs) in (('surface', surface), ('with_shadow', region)):
        error = np.abs(got[ys, xs] - want[ys, xs]).max(-1)
        result[name] = {'pixels': int(error.size), 'exact': round(float(np.mean(error == 0)) * 100, 1),
                        'within_1': round(float(np.mean(error <= 1)) * 100, 1),
                        'within_2': round(float(np.mean(error <= 2)) * 100, 1), 'worst': int(error.max())}
    return result


def surfaces(log):
    found = {}
    for number, name, rect in re.findall(r'shell-surfaces surface number=(\d+) name=([\w-]+) rect=([-\d,]+)', log):
        found[int(number)] = {'name': name, 'rect': [int(v) for v in rect.split(',')]}
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--name', required=True)
    parser.add_argument('--reference', type=Path, help="the prototype's captures (<open>-<theme>.png)")
    args = parser.parse_args()
    result = subprocess.run([sys.executable, str(ROOT / 'tools/run-console.py'), '--name', args.name,
        '--profile', 'mihawk-os',
        '--args=Z:\\data\\ps5proton\\prefixes\\probe\\drive_c\\shell-surfaces64-probe.exe --capture=Z:\\data\\ps5proton\\logs\\game-frame.ppm',
        '--marker', 'PS5_PROTON_SHELL_SURFACES_PROBE', '--seconds', '300', '--watchdog', '360'], cwd=ROOT)
    path = ROOT / 'evidence' / (args.name + '.json')
    if not path.exists():
        return result.returncode or 1
    record = json.loads(path.read_text())
    log = '\n'.join(log.read_text(errors='replace') for log in (ROOT / 'klog').glob(args.name + '*.log'))
    placed, captures = surfaces(log), record.get('captures', [])
    comparisons = []
    for index, capture in enumerate(captures, 1):
        entry = {'capture': capture, 'number': index, **placed.get(index, {})}
        reference = args.reference / f'{REFERENCE[index]}.png' if args.reference and index in REFERENCE else None
        if reference and reference.exists() and entry.get('rect') and entry['rect'][2] > entry['rect'][0]:
            entry['prototype'] = REFERENCE[index]
            entry.update(compare(ROOT / capture, reference, entry['rect']))
        comparisons.append(entry)
    record['surfaces'] = comparisons
    path.write_text(json.dumps(record, indent=1) + '\n')
    print(json.dumps({'surfaces': comparisons, 'passed': record['passed']}, indent=1))
    return 0 if record['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
