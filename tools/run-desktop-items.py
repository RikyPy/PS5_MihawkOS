#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The desktop's items in the Mihawk session: menus, a double click, shortcuts, and (with --soak N) N pictures of the idle desktop."""
import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--name', required=True)
    parser.add_argument('--soak', type=int, default=0)
    parser.add_argument('--launch', nargs='?', const='', default=None, help='double click the game shortcuts and see which open a window')
    parser.add_argument('--env', action='append', default=[], help='NAME=VALUE for the launch')
    args = parser.parse_args()
    probe = 'Z:\\data\\ps5proton\\prefixes\\probe\\drive_c\\desktop-items64-probe.exe --capture=Z:\\data\\ps5proton\\logs\\game-frame.ppm'
    if args.soak:
        probe += f' --soak={args.soak}'
    if args.launch is not None:
        probe += ' --launch' + (f'={args.launch}' if args.launch else '')
    seconds = 150 + 75 * args.soak + (500 if args.launch is not None and not args.soak else 0)
    return subprocess.run([sys.executable, str(ROOT / 'tools/run-console.py'), '--name', args.name, '--profile', 'mihawk-os', f'--args={probe}',
        '--marker', 'PS5_PROTON_DESKTOP_ITEMS', '--seconds', str(seconds), '--watchdog', str(seconds + 60)] + [x for e in args.env for x in ('--env', e)], cwd=ROOT).returncode


if __name__ == '__main__':
    sys.exit(main())
