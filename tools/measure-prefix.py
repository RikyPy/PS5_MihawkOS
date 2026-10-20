#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""docs/SELF_INSTALL.md phase 0, probe 9: what a Wine prefix on the console holds, read over FTP, nothing written.

  tools/measure-prefix.py --name RUN [--prefix mihawk]

Sums /data/ps5proton/prefixes/<prefix> by part: what wineboot makes in a new prefix (drive_c/windows, the
Program Files folders Mono and Gecko install into, the registry files, the users' folders) and what a session adds
later (drive_c/Games and anything else), so the first prefix's share of an install's free-space check can be read off
a prefix that has been used. Writes evidence/<RUN>.json; names inside drive_c/Games are counted, never recorded.
"""
import argparse
import ftplib
import json
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT.parent / 'PS5_Vulkan/tools'))
import ps5_console  # noqa: E402

FIRST_SESSION = ('drive_c/windows', 'drive_c/Program Files', 'drive_c/Program Files (x86)', 'drive_c/ProgramData',
                 'drive_c/users', 'system.reg', 'user.reg', 'userdef.reg', '.update-timestamp')


def entries(ftp, directory):
    """(name, is a folder, bytes) for each entry, from ftpsrv's LIST (Unix long format: the name may hold spaces)."""
    lines = []
    ftp.retrlines(f'LIST {directory}', lines.append)
    return [(f[8], f[0].startswith('d'), int(f[4]) if f[4].isdigit() else 0)
            for f in (line.split(None, 8) for line in lines) if len(f) == 9 and f[8] not in ('.', '..')]


def walk(ftp, directory, depth=0):
    """(bytes, files) of every regular file under directory. A drive letter (dosdevices' z: is /) is never followed."""
    total = files = 0
    if depth > 24:
        raise SystemExit(f'{directory}: deeper than 24 folders; a link may be listed as a folder')
    for name, folder, size in entries(ftp, directory):
        if name.endswith(':') or name == 'dosdevices':
            continue
        sub = walk(ftp, f'{directory}/{name}', depth + 1) if folder else (size, 1)
        total, files = total + sub[0], files + sub[1]
    return total, files


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--name', required=True)
    parser.add_argument('--prefix', default='mihawk')
    args = parser.parse_args()
    evidence = ROOT / 'evidence' / (args.name + '.json')
    if evidence.exists():
        raise SystemExit('evidence already exists; use a new run name')
    settings = ps5_console.load_settings()
    base = f'/data/ps5proton/prefixes/{args.prefix}'
    started = time.monotonic()
    with ftplib.FTP() as ftp:
        ftp.connect(settings['host'], settings['ftp_port'], timeout=30)
        ftp.login(settings['ftp_user'], settings['ftp_password'])
        listed = [(n, d, s) for n, d, s in entries(ftp, base) if n not in ('drive_c', 'dosdevices')]
        listed += [('drive_c/' + n, d, s) for n, d, s in entries(ftp, base + '/drive_c')]
        parts = {}
        for name, folder, size in listed:
            size, count = walk(ftp, f'{base}/{name}') if folder else (size, 1)
            key = name if name in FIRST_SESSION else 'later: drive_c/Games' if name == 'drive_c/Games' else 'later: other'
            parts.setdefault(key, [0, 0])
            parts[key][0] += size
            parts[key][1] += count
    first = sum(v[0] for k, v in parts.items() if not k.startswith('later'))
    record = {'prefix': args.prefix, 'parts': {k: {'bytes': v[0], 'files': v[1]} for k, v in sorted(parts.items())},
              'first_session_bytes': first, 'seconds': round(time.monotonic() - started, 1),
              'measured': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}
    evidence.write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps(record, indent=2))


if __name__ == '__main__':
    main()
