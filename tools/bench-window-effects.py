#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Measure the retained compositor's host cost with DWM effects, comparing the
working tree with a revision, and write evidence/<name>.json.

The scenes (tests/window-effects-bench.c) are cumulative: two opaque windows,
rounded shadowed frames, acrylic Start, a Mica taskbar, a full-screen Mica
window. Each figure is the minimum of 60 compositions on this host; the PS5's
CPU is slower, so only ratios transfer. Console timing comes from the broker's
compose statistics in each run record.
"""
import argparse
import json
import platform
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ('pw_window_compose.c', 'pw_window_effects.c', 'pw_window_effects.h', 'pw_window_snapshot.h')


def measure(source_dir, work):
    binary = work / 'bench'
    subprocess.run(['cc', '-std=gnu11', '-O2', f'-I{source_dir}', str(ROOT / 'tests/window-effects-bench.c'),
                    str(source_dir / 'pw_window_compose.c'), str(source_dir / 'pw_window_effects.c'),
                    '-lm', '-o', str(binary)], check=True)
    output = subprocess.run([str(binary)], check=True, capture_output=True, text=True).stdout
    return {name: float(ms) for name, ms in (line.split('\t') for line in output.strip().splitlines())}


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--name', required=True)
    parser.add_argument('--against', default='HEAD', help='revision to compare with the working tree')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as folder:
        work, old = Path(folder), Path(folder) / 'old'
        old.mkdir()
        for name in SOURCES:
            (old / name).write_text(subprocess.check_output(['git', 'show', f'{args.against}:src/{name}'], cwd=ROOT, text=True))
        before, after = measure(old, work), measure(ROOT / 'src', work)
    record = {'name': args.name, 'host': platform.processor() or platform.machine(), 'against': args.against,
              'revision': subprocess.check_output(['git', 'rev-parse', args.against], cwd=ROOT, text=True).strip(),
              'unit': 'ms per 1920x1080 composition, minimum of 60',
              'scenes': [{'scene': scene, 'before': before[scene], 'after': after[scene]} for scene in before]}
    (ROOT / 'evidence' / (args.name + '.json')).write_text(json.dumps(record, indent=1) + '\n')
    print(json.dumps(record, indent=1))


if __name__ == '__main__':
    main()
