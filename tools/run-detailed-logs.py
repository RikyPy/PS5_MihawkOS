#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Settings > Logging on the console (src/pw_detailed_logs.h): the same session with Logging off, then on.

  tools/run-detailed-logs.py --name RUN [--only on] [-- run-console.py's options for the session]

The session is the Mihawk desktop (--profile mihawk-os --seconds 30) unless options follow "--" (a game at 120 Hz:
-- --profile ID --seconds 60 --env PROTON_120HZ=1). Each leg is one tools/run-console.py run, evidence/RUN-off.json and
RUN-on.json; logging.txt is set for it and the player's own value put back after. After the on leg the launcher has
filled the session's folder; it is downloaded to klog/RUN-detailed/ (ignored) and must hold:
  one new folder, named for the session's start, with README.txt;
  the session's logs: title-early.log, title-last.log, wine-last.log, flight-recorder.log, at least one wine-child log;
  klog.txt, since a run's klog comes from klogsrv (its PC client and the title's share it);
  no unmasked IPv4 address in any file, README.txt included, and nowhere the console's own;
  at most PW_DETAILED_LOGS_FOLDER_BYTES.
evidence/RUN.json has, per leg, the session's log bytes (title-last, wine-last and sessions/0, as the launcher leaves
them) and its display pacing, so the cost of the detailed WINEDEBUG is measured against the same session without it;
and the folder's files with their sizes.
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
DATA = '/data/ps5proton'
SETTING = f'{DATA}/logging.txt'
DETAILED = f'{DATA}/logs/detailed'   # PW_DETAILED_LOGS_ROOT
FOLDER_BYTES = 96 << 20              # PW_DETAILED_LOGS_FOLDER_BYTES
EXPECTED = ['README.txt', 'title-early.log', 'title-last.log', 'wine-last.log', 'flight-recorder.log', 'klog.txt']
# Names the evidence may hold; any other file (DXVK's logs carry a program's name) is only counted (no game's name
# goes into a committed record).
KNOWN = re.compile(r'(README\.txt|title-(early|last)\.log|wine-last\.log|wine-stdout\.txt|lapy-service\.log|setup\.txt|'
                   r'klog\.txt|flight-recorder(\.1)?\.log|wine-child-\d+\.log)')
# A dotted IPv4 address as src/pw_detailed_logs.c masks it: four groups of 1-3 digits, no digit or dot-digit around.
ADDRESS = re.compile(rb'(?<!\d)(?<!\d\.)\d{1,3}(?:\.\d{1,3}){3}(?!\.?\d)')
DEFAULT = ['--profile', 'mihawk-os', '--seconds', '30']


def connect(settings):
    ftp = ftplib.FTP()
    ftp.connect(settings['host'], settings['ftp_port'], timeout=20)
    ftp.login(settings['ftp_user'], settings['ftp_password'])
    return ftp


def listing(ftp, folder):
    """{name: size} of a folder's regular files; the console's FTP has no NLST (tools/run-console.py)."""
    lines = []
    try:
        ftp.retrlines(f'LIST {folder}', lines.append)
    except ftplib.error_perm:
        return {}
    files = {}
    for line in lines:
        parts = line.split(None, 8)
        if len(parts) == 9 and line.startswith('-'):
            files[parts[8]] = int(parts[4])
    return files


def folders(ftp):
    lines = []
    try:
        ftp.retrlines(f'LIST {DETAILED}', lines.append)
    except ftplib.error_perm:
        return set()
    names = {line.split(None, 8)[8] for line in lines if line.startswith('d') and len(line.split(None, 8)) == 9}
    return names - {'.', '..'}   # the console's LIST shows them


def session_bytes(ftp):
    """What the session wrote where the launcher leaves it: its title and Wine logs and its helpers' folder."""
    logs = listing(ftp, f'{DATA}/logs')
    helpers = listing(ftp, f'{DATA}/logs/sessions/0')
    return {'title_last': logs.get('title-last.log'), 'wine_last': logs.get('wine-last.log'),
            'helpers': sum(size for name, size in helpers.items() if name.endswith('.log')),
            'helper_files': len(helpers)}


def pacing(record):
    windows = (record.get('pacing') or {}).get('windows') or []
    return [{k: w.get(k) for k in ('fps', 'p50', 'p95', 'p99', 'max', 'frames', 'seconds')} for w in windows]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--name', required=True)
    parser.add_argument('--only', choices=('on',), help='skip the off leg')
    parser.add_argument('session', nargs=argparse.REMAINDER, help="after --: run-console.py's options")
    args = parser.parse_args()
    if not re.fullmatch(r'[a-zA-Z0-9_-]+', args.name):
        parser.error('use a plain run name')
    evidence = ROOT / 'evidence' / (args.name + '.json')
    if evidence.exists():
        parser.error('use a new run name')
    session = [word for word in args.session if word != '--'] or DEFAULT
    import ps5_console
    settings = ps5_console.load_settings()
    with connect(settings) as ftp:
        lines = []
        try:
            ftp.retrlines('RETR ' + SETTING, lines.append)
            before = ('\n'.join(lines) + '\n').encode()
        except ftplib.error_perm:
            before = None
    record = {'case': 'detailed-logs', 'session': session, 'legs': {}}
    try:
        for leg in ([] if args.only else ['off']) + ['on']:
            with connect(settings) as ftp:
                ftp.storbinary('STOR ' + SETTING, io.BytesIO(f'logging {leg}\n'.encode()))
                existing = folders(ftp)
            run = f'{args.name}-{leg}'
            result = subprocess.run([sys.executable, str(ROOT / 'tools/run-console.py'), '--name', run] + session, cwd=ROOT)
            path = ROOT / 'evidence' / (run + '.json')
            launched = json.loads(path.read_text()) if path.exists() else {}
            log = ROOT / 'klog' / (run + '.log')
            text = log.read_text(errors='replace') if log.exists() else ''
            with connect(settings) as ftp:
                outcome = {'run': run, 'runner_status': result.returncode, 'session_passed': bool(launched.get('passed')),
                           'seconds': launched.get('seconds'), 'returned_to_launcher': launched.get('returned_to_launcher'),
                           'log_bytes': session_bytes(ftp), 'pacing': pacing(launched),
                           'title_said': re.findall(r'Logging: [^\r\n]*', text)[:8]}
                if leg == 'on':
                    new = sorted(folders(ftp) - existing)
                    outcome['folders_made'] = new
                    if len(new) == 1:
                        folder = f'{DETAILED}/{new[0]}'
                        files = listing(ftp, folder)
                        local = ROOT / 'klog' / (args.name + '-detailed') / new[0]
                        local.mkdir(parents=True, exist_ok=True)
                        unmasked, own = [], False
                        for name in files:
                            buffer = io.BytesIO()
                            ftp.retrbinary(f'RETR {folder}/{name}', buffer.write)
                            data = buffer.getvalue()
                            (local / name).write_bytes(data)
                            unmasked += [name] if ADDRESS.search(data) else []
                            own = own or settings['host'].encode() in data
                        wine_children = [n for n in files if re.fullmatch(r'wine-child-\d+\.log', n)]
                        named = {n: s for n, s in files.items() if KNOWN.fullmatch(n)}
                        outcome.update({
                            'folder': new[0], 'files': named, 'other_files': len(files) - len(named),
                            'other_bytes': sum(files.values()) - sum(named.values()), 'total_bytes': sum(files.values()),
                            'missing': [n for n in EXPECTED if n not in files], 'wine_children': len(wine_children),
                            'unmasked_files': [n if KNOWN.fullmatch(n) else '(other)' for n in unmasked],
                            'console_address_found': own,
                            'readme_says_mask': b'IP address' in (local / 'README.txt').read_bytes()
                            if 'README.txt' in files else False})
                        outcome['folder_passed'] = (not outcome['missing'] and wine_children != [] and not unmasked and
                                                    not own and outcome['readme_says_mask'] and
                                                    outcome['total_bytes'] <= FOLDER_BYTES)
                    else:
                        outcome['folder_passed'] = False
            outcome['passed'] = (result.returncode == 0 and outcome['session_passed'] and
                                 outcome.get('folder_passed', True))
            record['legs'][leg] = outcome
            evidence.write_text(json.dumps(record, indent=1) + '\n')
            if not launched.get('returned_to_launcher'):
                break   # the title did not come back: nothing more can run
    finally:
        with connect(settings) as ftp:
            if before is None:
                try:
                    ftp.delete(SETTING)
                except ftplib.error_reply as error:   # the console answers DELE with 226
                    if not str(error).startswith('226'):
                        raise
                except ftplib.error_perm:
                    pass
            else:
                ftp.storbinary('STOR ' + SETTING, io.BytesIO(before))
    legs = record['legs']
    record['passed'] = 'on' in legs and all(leg['passed'] for leg in legs.values())
    record['result'] = 'finished' if all(leg.get('returned_to_launcher') for leg in legs.values()) else 'title still running'
    record['returned_to_launcher'] = record['result'] == 'finished'
    evidence.write_text(json.dumps(record, indent=1) + '\n')
    print(json.dumps({leg: {k: v for k, v in o.items() if k not in ('files', 'title_said')} for leg, o in legs.items()},
                     indent=1))
    print('passed' if record['passed'] else 'FAILED')
    return 0 if record['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
