#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Game mode on the Mihawk desktop, end to end (docs/MIHAWK_OS.md): tests/game-mode-probe.c in a desktop session.

  tools/run-game-mode.py --name RUN [--api opengl|dxvk]

The probe goes full-screen, checks from a broker capture that the screen is exactly its frame, sees the taskbar go
(Explorer unloaded) and come back with its other window's button when it leaves. The run passes only if the title's
own records agree: the broker entered game mode for a client (graphics patch 0006), the session unloaded the desktop
and restored it (src/pw_desktop_session.h), and nothing refused or failed on the way. The title's unload and restore
times and the probe's bench go into evidence/RUN.json.
"""
import argparse
import ftplib
import io
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT.parent / 'PS5_Vulkan/tools'))
SETTING = '/data/ps5proton/game-mode.txt'


class GameModeOn:
    """Game mode is the player's setting (src/pw_game_mode.h) and the player may have it off (2026-10-10: to rule it out
    when the screen goes black), which made both gate cases fail on 2026-10-11 ("Game Mode: off" in the run). The case
    tests game mode, so it sets the setting on for its run and puts the player's own value back, or removes the file
    when there was none."""

    def __enter__(self):
        import ps5_console
        self.settings = ps5_console.load_settings()
        with self._ftp() as ftp:
            lines = []
            try:
                ftp.retrlines('RETR ' + SETTING, lines.append)
                self.before = ('\n'.join(lines) + '\n').encode()
            except ftplib.error_perm:
                self.before = None
            ftp.storbinary('STOR ' + SETTING, io.BytesIO(b'game-mode on\n'))
        return self

    def __exit__(self, *unused):
        with self._ftp() as ftp:
            if self.before is None:
                try:
                    ftp.delete(SETTING)
                except ftplib.all_errors:
                    pass
            else:
                ftp.storbinary('STOR ' + SETTING, io.BytesIO(self.before))
        return False

    def _ftp(self):
        ftp = ftplib.FTP()
        ftp.connect(self.settings['host'], self.settings['ftp_port'], timeout=20)
        ftp.login(self.settings['ftp_user'], self.settings['ftp_password'])
        return ftp


def title_records(text):
    """What the title and the broker said about game mode in this run."""
    return {
        'direct_entries': len(re.findall(r'gpu-direct enter pid=\d+', text)),
        'unloads': [int(m) for m in re.findall(r'Mihawk: desktop unloaded in (\d+) ms', text)],
        'restores': [{'status': int(s), 'ms': int(ms)} for s, ms in
                     re.findall(r'Mihawk: desktop restored \((-?\d+)\) in (\d+) ms', text)],
        'kept': len(re.findall(r'Mihawk: the desktop did not end', text)),
        'could_not_start': len(re.findall(r'Mihawk: the desktop could not start again', text)),
        'session_root': bool(re.search(r'Mihawk: desktop session; Explorer is its root', text)),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--name', required=True)
    parser.add_argument('--api', choices=('opengl', 'dxvk'), default='opengl')
    parser.add_argument('--env', action='append', default=[], help='NAME=VALUE for this run only, as run-console.py')
    args = parser.parse_args()
    probe = 'C:\\game-mode64-probe.exe' + (' --dxvk' if args.api == 'dxvk' else '')
    with GameModeOn():
        result = subprocess.run([sys.executable, str(ROOT / 'tools/run-console.py'), '--name', args.name,
            '--path', r'C:\windows\system32\mihawk.exe', '--args', probe, '--marker', 'PS5_PROTON_GAME_MODE',
            '--env', 'PROTON_LAYOUT=launcher', '--env', 'WINE_PS5_WINDOW_SURFACES=1', '--env', 'MIHAWK_DESKTOP=1',
            '--seconds', '150', '--watchdog', '200'] + [word for value in args.env for word in ('--env', value)], cwd=ROOT)
    path = ROOT / 'evidence' / (args.name + '.json')
    if not path.exists():
        return result.returncode or 1
    record = json.loads(path.read_text())
    log = ROOT / 'klog' / (args.name + '.log')
    records = title_records(log.read_text(errors='replace') if log.exists() else '')
    title_passed = (records['session_root'] and records['direct_entries'] >= 1 and len(records['unloads']) >= 1 and
                    len(records['restores']) >= 1 and all(r['status'] == 0 for r in records['restores']) and
                    not records['kept'] and not records['could_not_start'])
    record['api'] = args.api
    record['game_mode'] = records
    record['probe_passed'] = record['passed']
    record['passed'] = record['passed'] and title_passed
    path.write_text(json.dumps(record, indent=1) + '\n')
    print(json.dumps({'api': args.api, 'game_mode': records, 'bench': record.get('bench'),
                      'passed': record['passed']}, indent=1))
    return 0 if record['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
