#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Test the saved global mapping through PE32 Windows mouse messages.

Requires an idle console. Backs up the player's exact file locally and restores
it in finally; test input uses R1/L1/R3 for left/right/middle clicks.
"""
import argparse
import io
import json
import re
import runpy
import subprocess
import sys
from ftplib import error_perm, error_reply
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
support = runpy.run_path(str(ROOT / 'tools/run-prefix-test.py'))
connect, console = support['connect'], support['ps5_console']
PATH = '/data/ps5proton/controls.txt'
MAPPING = b'left r1\nright l1\nmiddle r3\n'


def read(ftp):
    data = io.BytesIO()
    try:
        ftp.retrbinary('RETR ' + PATH, data.write)
    except error_perm as error:
        if str(error).startswith('550'):
            return None
        raise
    return data.getvalue()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--name', required=True)
    args = parser.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9_-]+', args.name):
        parser.error('use a plain run name')
    evidence = ROOT / 'evidence' / (args.name + '.json')
    if evidence.exists():
        parser.error('use a new run name')
    settings = console.load_settings()
    if ' count=0 ' not in console.ps5vkctl_command(settings, 'procs', timeout=15) + ' ':
        raise RuntimeError('console is occupied: nothing changed')
    with connect(settings) as ftp:
        original = read(ftp)
    (ROOT / 'klog' / (args.name + '-controls-backup.json')).write_text(
        json.dumps({'path': PATH, 'original': None if original is None else original.hex()}) + '\n')
    restored = False
    result = 1
    try:
        with connect(settings) as ftp:
            ftp.storbinary('STOR ' + PATH, io.BytesIO(MAPPING))
            if read(ftp) != MAPPING:
                raise RuntimeError('mapping upload differs')
        result = subprocess.run([sys.executable, str(ROOT / 'tools/run-console.py'), '--name', args.name,
            '--exe', 'mouse-controls32-probe.exe', '--marker', 'PS5_PROTON_MOUSE_CONTROLS', '--no-stdout',
            '--pad-script', str(ROOT / 'tests/pad-scripts/mouse-controls.txt')], cwd=ROOT).returncode
    finally:
        with connect(settings) as ftp:
            if original is None:
                try:
                    ftp.delete(PATH)
                except error_reply as error:
                    # The console's FTP server also uses 226 for successful deletion.
                    if not str(error).startswith('226'):
                        raise
            else:
                ftp.storbinary('STOR ' + PATH, io.BytesIO(original))
            restored = read(ftp) == original
        if evidence.exists():
            record = json.loads(evidence.read_text())
            record['controls_mapping'] = {'left': 'r1', 'right': 'l1', 'middle': 'r3'}
            record['controls_restored'] = restored
            record['passed'] = record['passed'] and restored
            evidence.write_text(json.dumps(record, indent=1) + '\n')
    return result if restored else 1


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Exception as error:
        print('controls runner failed: ' + re.sub(r'\b\d{1,3}(\.\d{1,3}){3}\b', '<console>', str(error)), file=sys.stderr)
        sys.exit(1)
