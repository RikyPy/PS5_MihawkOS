#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A stand-in for tools/run-console.py that runs the test program on the host, for dry runs of the console runners.

The runners (run-wine-tests.py console, run-vkd3d-tests.py console) pick this launcher when PW_STUB_CONSOLE is set: the
slicing, the resuming after a crash, the merging and the comparison are then exercised end to end without a console, and
a bug in them costs nothing. It takes the options run-console.py takes that matter (--exe, --args, --env, --name) and
writes klog/<name>-stdout.txt as the console run would. The program runs in a Wine virtual desktop of the system Wine
with the DXVK and VKD3D-Proton DLLs, as the host baselines do.
"""
import argparse
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--exe')
    parser.add_argument('--args', default='')
    parser.add_argument('--env', action='append', default=[])
    parser.add_argument('--name', required=True)
    parser.add_argument('--seconds')
    parser.add_argument('--watchdog')
    args = parser.parse_args()
    env = dict(os.environ, WINEPREFIX=str(ROOT / '.deps/wine-tests/prefix-sys'), WINEDEBUG='-all')
    for item in args.env:
        if '=' in item:
            key, value = item.split('=', 1)
            env[key] = value
    out = ROOT / 'klog' / f'{args.name}-stdout.txt'
    out.parent.mkdir(exist_ok=True)
    exe = str(ROOT / '.deps/wine-tests/exe' / args.exe).replace('/', '\\')
    command = ['wine', 'explorer', '/desktop=pwtests,1280x720', 'cmd', '/c',
               f'Z:{exe} {args.args} > Z:{str(out).replace("/", chr(92))} 2>&1']
    subprocess.run(command, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=3000, start_new_session=True)
    print(f'stub run {args.name} finished')
    return 0


if __name__ == '__main__':
    sys.exit(main())
