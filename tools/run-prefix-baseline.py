#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the PE32/PE64 baseline probe in a fresh console-created prefix.

Owns only games/proton-baseline-test and its matching prefix. Refuses to remove
an existing entry without its exact ownership marker. Never edits game prefixes.
The ordinary console runner owns launch, collection, closure and its verdict.
"""
import argparse
import hashlib
import io
import json
import re
import runpy
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
support = runpy.run_path(str(ROOT / 'tools/run-prefix-test.py'))
connect, listing, remove_tree = (support[k] for k in ('connect', 'listing', 'remove_tree'))
console = support['ps5_console']
ENTRY = 'proton-baseline-test'
GAME = '/data/ps5proton/games/' + ENTRY
PREFIX = '/data/ps5proton/prefixes/' + ENTRY
MARKER = '.ps5-proton-test'
OWNER = b'tools/run-prefix-baseline.py\n'


def idle(settings):
    return ' count=0 ' in console.ps5vkctl_command(settings, 'procs', timeout=15) + ' '


def remove_owned(ftp):
    game, prefix = listing(ftp, GAME), listing(ftp, PREFIX)
    if game is not None:
        marker = io.BytesIO()
        ftp.retrbinary('RETR ' + GAME + '/' + MARKER, marker.write)
        if marker.getvalue() != OWNER:
            raise RuntimeError('baseline test entry belongs to another tool: nothing removed')
    elif prefix is not None:
        raise RuntimeError('baseline test prefix has no ownership marker: nothing removed')
    for path, contents in ((PREFIX, prefix), (GAME, game)):
        if contents is not None:
            remove_tree(ftp, path, {'files': 0, 'folders': 0})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--name', required=True)
    parser.add_argument('--arch', choices=('32', '64'), required=True)
    args = parser.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9_-]+', args.name):
        parser.error('use a plain run name')
    evidence = ROOT / 'evidence' / (args.name + '.json')
    if evidence.exists():
        parser.error('use a new run name')
    settings = console.load_settings()
    if not idle(settings):
        raise RuntimeError('console is occupied: nothing changed')
    name = 'prefix-baseline' + args.arch + '-probe.exe'
    probe = (ROOT / '.deps/probes' / name).read_bytes()
    with connect(settings) as ftp:
        remove_owned(ftp)
        ftp.mkd(GAME)
        game = {'name': 'Shared Proton baseline probe', 'prefix': ENTRY, 'desktop': False,
                'executable': 'Z:' + GAME.replace('/', '\\') + '\\' + name,
                'directory': 'Z:' + GAME.replace('/', '\\')}
        for filename, data in ((MARKER, OWNER), (name, probe),
                               ('game.json', (json.dumps(game) + '\n').encode())):
            ftp.storbinary('STOR ' + GAME + '/' + filename, io.BytesIO(data))
            readback = io.BytesIO()
            ftp.retrbinary('RETR ' + GAME + '/' + filename, readback.write)
            if readback.getvalue() != data:
                raise RuntimeError('baseline test upload differs: ' + filename)
    result = subprocess.run([sys.executable, str(ROOT / 'tools/run-console.py'), '--name', args.name,
                             '--profile', ENTRY, '--no-stdout', '--env', 'PROTON_LAYOUT=launcher',
                             '--marker', 'PS5_PROTON_PREFIX_BASELINE', '--seconds', '600',
                             '--pad-script', str(ROOT / 'tests/pad-scripts/prefix-text.txt'),
                             '--watchdog', '660', '--exit-stall', '60'], cwd=ROOT)
    record = json.loads(evidence.read_text())
    record.update(fresh_prefix=True, architecture=args.arch, probe_sha256=hashlib.sha256(probe).hexdigest())
    record['test_data_removed'] = False
    if record.get('returned_to_launcher') and idle(settings):
        with connect(settings) as ftp:
            remove_owned(ftp)
        record['test_data_removed'] = True
    evidence.write_text(json.dumps(record, indent=2) + '\n')
    return result.returncode


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Exception as error:
        # Addresses, credentials and user paths never enter a failure report.
        print('baseline runner failed: ' + re.sub(r'\b\d{1,3}(\.\d{1,3}){3}\b', '<console>', str(error)),
              file=sys.stderr)
        sys.exit(1)
