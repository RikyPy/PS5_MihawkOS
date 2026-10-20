#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The gate's games-autodetect case: a game folder copied into C:\\Games gets its desktop shortcut, with no game.json
(src/pw_game_folders.h; docs/SELF_INSTALL.md, phase 2).

In the probe prefix, before one desktop session (tools/run-console.py --path mihawk.exe):
  C:\\Games\\GateAutodetect\\gate.exe      one x86-64 program (tests/games-autodetect-probe.c), beside unins000.exe
  C:\\Games\\GateSeveral\\one.exe, two.exe two programs: no shortcut, the title names the folder
The session runs gate.exe, which says it runs from that folder and closes the session; the title must have written
C:\\users\\Public\\Desktop\\GateAutodetect.lnk to that program, starting in that folder, and none for GateSeveral, which
it names in a PS5 system notification.
The planted folders are removed afterwards; the probe prefix's next desktop session removes the shortcut it owned.

  tools/run-games-autodetect.py --name NAME
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
import ps5_console  # noqa: E402

PREFIX = '/data/ps5proton/prefixes/probe/drive_c'
# The game: tests/games-autodetect-probe.c, which ends its session; the others are never run.
GAME = ROOT / '.deps/staged/prefixes/probe/drive_c/games-autodetect-probe.exe'
PROGRAM = ROOT / '.deps/staged/prefixes/probe/drive_c/exit-probe.exe'
PLANTED = {'Games/GateAutodetect/gate.exe': GAME, 'Games/GateAutodetect/unins000.exe': PROGRAM,
           'Games/GateSeveral/one.exe': PROGRAM, 'Games/GateSeveral/two.exe': PROGRAM}
LINKS = f'{PREFIX}/users/Public/Desktop'


def connect(settings):
    ftp = ftplib.FTP()
    ftp.connect(settings['host'], settings['ftp_port'], timeout=30)
    ftp.login(settings['ftp_user'], settings['ftp_password'])
    return ftp


def mkdirs(ftp, path):
    ftp.cwd('/')
    for part in path.strip('/').split('/'):
        try:
            ftp.cwd(part)
        except ftplib.error_perm:
            ftp.mkd(part)
            ftp.cwd(part)


def fetch(ftp, path):
    data = io.BytesIO()
    try:
        ftp.retrbinary(f'RETR {path}', data.write)
    except ftplib.error_perm as error:
        if str(error).startswith('550'):
            return None
        raise
    return data.getvalue()


def clean(ftp):
    # The console's FTP server answers DELE with 226, which ftplib raises (tools/run-launcher.py's remove does the same).
    for relative in PLANTED:
        try:
            ftp.delete(f'{PREFIX}/{relative}')
        except ftplib.error_perm:
            pass
        except ftplib.error_reply as error:
            if not str(error).startswith('226'):
                raise
    for folder in ('Games/GateAutodetect', 'Games/GateSeveral'):
        try:
            ftp.rmd(f'{PREFIX}/{folder}')
        except (ftplib.error_perm, ftplib.error_reply):
            pass


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--name', required=True)
    args = parser.parse_args()
    if not re.fullmatch(r'[a-zA-Z0-9_-]+', args.name):
        parser.error('use a plain run name')
    evidence = ROOT / 'evidence' / (args.name + '.json')
    if evidence.exists():
        parser.error('use a new run name')
    settings = ps5_console.load_settings()
    if ' count=0 ' not in ps5_console.ps5vkctl_command(settings, 'procs', timeout=15) + ' ':
        raise SystemExit('the console is running an application; nothing planted')
    with connect(settings) as ftp:
        clean(ftp)
        for relative, source in PLANTED.items():
            mkdirs(ftp, f'{PREFIX}/{relative.rsplit("/", 1)[0]}')
            with source.open('rb') as data:
                ftp.storbinary(f'STOR {PREFIX}/{relative}', data)
    run = args.name + '-session'
    try:
        result = subprocess.run([sys.executable, str(ROOT / 'tools/run-console.py'), '--name', run,
                                 '--path', r'C:\windows\system32\mihawk.exe',
                                 '--args', r'C:\Games\GateAutodetect\gate.exe',
                                 '--marker', 'PS5_PROTON_GAMES_AUTODETECT', '--env', 'PROTON_LAYOUT=launcher'], cwd=ROOT)
    finally:
        with connect(settings) as ftp:
            link = fetch(ftp, f'{LINKS}/GateAutodetect.lnk')
            several = fetch(ftp, f'{LINKS}/GateSeveral.lnk')
            clean(ftp)
    session = ROOT / 'evidence' / (run + '.json')
    record = {'case': 'games-autodetect', 'run': run, 'runner_status': result.returncode}
    if session.exists():
        launched = json.loads(session.read_text())
        record.update({k: launched.get(k) for k in ('result', 'probe_verdict', 'returned_to_launcher', 'exit_stall')})
        record['session_passed'] = bool(launched.get('passed'))
    log = ROOT / 'klog' / (run + '.log')
    text = log.read_text(errors='replace') if log.exists() else ''
    record['folder_lines'] = re.findall(r'Mihawk: games folder(?:s in C:\\\\Games)?:? (.*)', text)
    target = rb'C:\Games\GateAutodetect\gate.exe' + b'\0'
    record['shortcut'] = {'written': link is not None,
                          'target': bool(link) and target in link,
                          'working_directory': bool(link) and r'C:\Games\GateAutodetect'.encode('utf-16-le') in link}
    record['several_has_no_shortcut'] = several is None
    record['several_named'] = any(line.startswith('GateSeveral: several programs') for line in record['folder_lines'])
    # And said on screen, as a PS5 system notification (native/proton_wine.cpp notices, phase 3).
    record['several_noticed'] = bool(re.search(r'notice: C:\\Games\\GateSeveral holds several programs.*\(posted 0\)',
                                               text))
    record['passed'] = (result.returncode == 0 and record.get('session_passed', False) and
                        all(record['shortcut'].values()) and record['several_has_no_shortcut'] and
                        record['several_named'] and record['several_noticed'])
    evidence.write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps(record, indent=2))
    return 0 if record['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
