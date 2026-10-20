#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Qualify native Start/taskbar/Explorer interactions and actual display captures."""
import argparse
import importlib.util
import json
import re
import subprocess
import sys
from pathlib import Path
import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import pw_explorer_expected  # noqa: E402

_effects_spec = importlib.util.spec_from_file_location('window_effects', ROOT / 'tools/run-window-effects.py')
effects = importlib.util.module_from_spec(_effects_spec)
_effects_spec.loader.exec_module(effects)
PHASES = ('desktop-light', 'start-light', 'start-dark', 'preview', 'explorer-light', 'explorer-dark')


def flyout_material(beneath, rect, dark, materials, untinted=False):
    """The prototype's flyout acrylic-base over what lies beneath Start (src/pw_window_effects.h): the blur of the
    window's own rectangle (edges repeated), the taskbar's transfer (light) or saturate(1.25) (dark), then 90% #f3f3f3
    or 92% #202020, none on the 1 px stroke ring."""
    left, top, right, bottom = rect
    scale, radius, passes = materials['acrylic_scale'], materials['acrylic_radius'], materials['blur_passes']
    image = np.asarray(beneath.convert('RGB'), dtype=np.float64)
    bw, bh = -(-(right - left) // scale), -(-(bottom - top) // scale)
    ys = np.clip(top + np.arange(bh * scale), top, bottom - 1)
    xs = np.clip(left + np.arange(bw * scale), left, right - 1)
    blocks = image[ys][:, xs].reshape(bh, scale, bw, scale, 3).mean(axis=(1, 3))
    for _ in range(passes):
        for axis in (1, 0):
            length = blocks.shape[axis]
            index = np.clip(np.arange(length)[:, None] + np.arange(-radius, radius + 1)[None, :], 0, length - 1)
            blocks = np.take(blocks, index, axis=axis).mean(axis=axis + 1)
    flyout = materials['flyout_base']

    def sample(x, y):
        u = min(max((x - left + 0.5) / scale - 0.5, 0), bw - 1)
        v = min(max((y - top + 0.5) / scale - 0.5, 0), bh - 1)
        x0, y0 = int(u), int(v)
        x1, y1 = min(x0 + 1, bw - 1), min(y0 + 1, bh - 1)
        fx, fy = u - x0, v - y0
        c = (blocks[y0, x0] * (1 - fx) + blocks[y0, x1] * fx) * (1 - fy) + (blocks[y1, x0] * (1 - fx) + blocks[y1, x1] * fx) * fy
        if dark:
            m = ((1.197, -0.179, -0.018), (-0.053, 1.071, -0.018), (-0.053, -0.179, 1.232))
            c = tuple(min(255, max(0, sum(m[i][k] * c[k] for k in range(3)))) for i in range(3))
        else:
            c = tuple(min(255.0, 0.2925 * v + 211.33) for v in c)
        return c if untinted else effects.tint(c, effects.hex_rgb(flyout['dark' if dark else 'light']),
                                                  flyout['tint_dark' if dark else 'tint'])
    return sample


def start_samples(beneath, window, point, dark, materials):
    """Start on the prototype's flyout acrylic-base, judged from the desktop captured beneath it: a glass point, the
    rounded-off corner (beneath darkened by the flyout shadow) and the top stroke (black at 5.9%, 20% dark, over the
    untinted backdrop)."""
    glass = flyout_material(beneath, window, dark, materials)
    ring = flyout_material(beneath, window, dark, materials, untinted=True)
    corner = effects.flyout_shadow(beneath.getpixel((window[0], window[1])), window, 8, window[0], window[1],
                                   materials['shadow_flyout']['dark' if dark else 'light'])
    top = ((window[0] + window[2]) // 2, window[1])
    stroke = tuple(c * (1 - (51 if dark else 15) / 255) for c in ring(*top))
    return [('start-glass', point, glass(*point), 2), ('start-corner', (window[0], window[1]), corner, 2),
            ('start-border', top, stroke, 2)]


def display_verdict(captures, log):
    samples = re.findall(r'shell-capture number=(\d+) phase=([\w-]+) point=(\d+),(\d+) '
                         r'expected=(\d+),(\d+),(\d+) result=(PASS|FAIL|HOST)', log)
    explorers = {int(n): (tuple(map(int, rect.split(','))), int(active), tuple(map(int, pointer.split(','))))
                 for n, rect, active, pointer in re.findall(r'shell-explorer number=(\d+) frame=([-\d,]+) active=(\d) '
                                                            r'pointer=([-\d,]+)', log)}
    glass = {int(n): tuple(map(int, rect.split(','))) for n, rect in re.findall(r'shell-glass number=(\d+) window=([-\d,]+)', log)}
    previews = {int(n): (tuple(map(int, rect.split(','))), tuple(map(int, colour.split(','))))
                for n, rect, colour in re.findall(r'shell-preview number=(\d+) thumb=([-\d,]+) expected=([\d,]+)', log)}
    materials = effects.tokens()
    results, beneath = [], None
    for number, capture in enumerate(captures, 1):
        found = set(tuple(sample) for sample in samples if int(sample[0]) == number)
        passed = False
        record = {'capture': capture, 'number': number}
        if len(found) == 1 and number <= len(PHASES):
            _, phase, x, y, _, _, _, verdict = found.pop()
            point = (int(x), int(y))
            with Image.open(ROOT / capture) as source:
                frame = source.convert('RGB')
            whole = frame.size == (1920, 1080)
            record.update(phase=phase, point=point)
            checks = []
            if phase == 'desktop-light':
                beneath = frame
                passed = phase == PHASES[number - 1] and verdict == 'HOST' and whole
            elif phase.startswith('start-'):
                window = glass.get(number)
                if window and beneath is not None and verdict == 'HOST' and whole:
                    for role, at, value, tolerance in start_samples(beneath, window, point, phase.endswith('dark'), materials):
                        actual = frame.getpixel(at)
                        checks.append({'role': role, 'point': at, 'expected': [round(c) for c in value], 'actual': actual,
                                       'passed': all(abs(a - e) <= tolerance + 0.5 for a, e in zip(actual, value))})
                passed = phase == PHASES[number - 1] and len(checks) == 3 and all(c['passed'] for c in checks)
            elif phase == 'preview':
                thumb = previews.get(number)
                if thumb and verdict == 'HOST' and whole:
                    (left, top, right, bottom), colour = thumb
                    at = ((left + right) // 2, (top + bottom) // 2)
                    actual = frame.getpixel(at)
                    checks.append({'role': 'thumbnail', 'point': at, 'expected': colour, 'actual': actual, 'passed': actual == colour})
                passed = phase == PHASES[number - 1] and len(checks) == 1 and checks[0]['passed']
            elif phase.startswith('explorer-'):
                # File Explorer on Home where the prototype's is (1100 x 700 at 410,166) and active, equal to the
                # prototype's own pixels within 2 levels (tools/pw_explorer_expected.py), as desktop-session judges it.
                window = explorers.get(number)
                if window and verdict == 'HOST' and whole:
                    bounds, active, pointer = window
                    result = pw_explorer_expected.compare(np.asarray(frame), bounds[0] + 1, bounds[1] + 1, phase[9:], pointer)
                    record.update(frame=list(bounds), active=active, **result)
                    passed = phase == PHASES[number - 1] and bounds == (410, 166, 1510, 866) and bool(active) and \
                        result['compared'] > 100000 and result['wrong'] == 0
            record['samples'] = checks
        record['passed'] = passed
        results.append(record)
    return results, len(results) == len(PHASES) and all(item['passed'] for item in results)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--name', required=True)
    parser.add_argument('--arch', choices=('32', '64'), required=True)
    args = parser.parse_args()
    result = subprocess.run([sys.executable, str(ROOT / 'tools/run-console.py'), '--name', args.name,
        '--path', r'C:\windows\system32\mihawk.exe', '--args', f'C:\\shell{args.arch}-probe.exe',
        '--marker', 'PS5_PROTON_SHELL', '--env', 'PROTON_LAYOUT=launcher',
        '--env', 'WINE_PS5_WINDOW_SURFACES=1', '--env', 'MIHAWK_DESKTOP=1',
        '--env', 'MIHAWK_SHELL_CAPTURE=1', '--env', 'PROTON_WINDOW_SNAPSHOT_PROBE=1',
        '--seconds', '150', '--watchdog', '190'], cwd=ROOT)
    path = ROOT / 'evidence' / (args.name + '.json')
    if not path.exists():
        return result.returncode or 1
    record = json.loads(path.read_text())
    log = '\n'.join(p.read_text(errors='replace') for p in (ROOT / 'klog').glob(args.name + '*.log'))
    pixels, passed = display_verdict(record.get('captures', []), log)
    record['semantics_passed'] = record['passed']
    record['shell_pixels'] = pixels
    record['passed'] = result.returncode == 0 and record['passed'] and passed
    path.write_text(json.dumps(record, indent=1) + '\n')
    print(json.dumps({'shell_pixels': pixels, 'passed': record['passed']}, indent=1))
    return 0 if record['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
