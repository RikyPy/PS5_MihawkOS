#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A prefix made on the console, services and an installer (docs/WINDOWS_PROCESSES.md, "Prefixes, services and
installers").

It adds a throwaway library entry, games/proton-prefix-test, holding tests/prefix-probe.c's program and 7-Zip's 64-bit
MSI package (pinned by hash), whose prefix does not exist. The title makes the prefix and Wine's wineboot fills it;
the probe then checks the prefix, the service control manager, installs the package with msiexec and runs the
program it installed. The run is an ordinary launch through tools/run-console.py; afterwards the entry and its
prefix are removed (only those two folders, both named by this tool, and only when the entry carries its marker),
and evidence/<name>.json records what the prefix held and what the probe said.

  tools/run-prefix-test.py --name NAME [--layout launcher|title] [--trace CHANNELS] [--env NAME=VALUE]... [--keep]

--trace adds Wine debug channels (e.g. ole,process) to msiexec alone; its helper log carries them.
"""
import argparse
import hashlib
import io
import json
import re
import subprocess
import sys
from ftplib import FTP, error_perm
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT.parent / 'PS5_Vulkan/tools'))
import ps5_console  # noqa: E402

ENTRY = 'proton-prefix-test'
GAME = f'/data/ps5proton/games/{ENTRY}'
PREFIX = f'/data/ps5proton/prefixes/{ENTRY}'
MARKER = '.ps5-proton-test'
PACKAGE = ROOT / '.deps/installers/7z2409-x64.msi'
PACKAGE_SHA256 = 'ec6af1ea0367d16dde6639a89a080a524cebc4d4bedfe00ed0cac4b865a918d8'
PACKAGE_URL = 'https://www.7-zip.org/a/7z2409-x64.msi'
PROBE = ROOT / '.deps/probes/prefix-probe.exe'


def connect(settings):
    ftp = FTP()
    ftp.connect(settings['host'], settings['ftp_port'], timeout=30)
    ftp.login(settings['ftp_user'], settings['ftp_password'] or 'x')
    return ftp


def listing(ftp, path):
    lines = []
    try:
        ftp.retrlines('LIST ' + path, lines.append)
    except error_perm:
        return None
    entries = []
    for line in lines:
        fields = line.split(None, 8)
        if len(fields) < 9:
            continue
        name = fields[8].rsplit('/', 1)[-1]
        if name in ('.', '..'):
            continue
        entries.append((name, line.startswith('d'), int(fields[4])))
    return entries


def remove_tree(ftp, path, counts):
    """Deletes path and everything below it; counts files and folders."""
    for name, folder, _ in listing(ftp, path) or []:
        child = path + '/' + name
        if folder:
            remove_tree(ftp, child, counts)
        else:
            ftp.sendcmd('DELE ' + child)
            counts['files'] += 1
    ftp.sendcmd('RMD ' + path)
    counts['folders'] += 1


def census(ftp, path, totals):
    for name, folder, size in listing(ftp, path) or []:
        if folder:
            census(ftp, path + '/' + name, totals)
        else:
            totals['files'] += 1
            totals['bytes'] += size


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--name', required=True)
    parser.add_argument('--layout', choices=('launcher', 'title'), default='launcher')
    parser.add_argument('--trace', help='Wine debug channels for msiexec, e.g. ole,process')
    parser.add_argument('--env', action='append', default=[], help='NAME=VALUE for the entry, e.g. '
                        'WINE_PS5_WAIT_WATCHDOG=1')
    parser.add_argument('--keep', action='store_true', help='leave the entry and its prefix on the console')
    args = parser.parse_args()
    if not re.fullmatch(r'[a-zA-Z0-9_-]+', args.name):
        parser.error('use a plain run name')
    if args.trace and not re.fullmatch(r'[a-z0-9_]+(,[a-z0-9_]+)*', args.trace):
        parser.error('--trace takes channel names separated by commas')
    if any(not re.fullmatch(r'[A-Z][A-Z0-9_]*=[A-Za-z0-9_.,+-]*', word) for word in args.env):
        parser.error('--env takes NAME=VALUE')
    evidence = ROOT / 'evidence' / (args.name + '.json')
    if evidence.exists():
        parser.error('use a new run name')
    if not PACKAGE.is_file():
        PACKAGE.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(['curl', '-sSfL', '-o', str(PACKAGE), PACKAGE_URL], check=True)
    package = PACKAGE.read_bytes()
    if hashlib.sha256(package).hexdigest() != PACKAGE_SHA256:
        raise SystemExit(f'{PACKAGE} does not match its pinned SHA-256')
    probe = PROBE.read_bytes()
    settings = ps5_console.load_settings()
    record = {'run': args.name, 'layout': args.layout, 'entry': ENTRY, 'package_sha256': PACKAGE_SHA256,
              'probe_sha256': hashlib.sha256(probe).hexdigest(), 'passed': False}
    with connect(settings) as ftp:
        for path in (GAME, PREFIX):
            if listing(ftp, path) is not None:
                if path == GAME and any(name == MARKER for name, _, _ in listing(ftp, GAME)):
                    counts = {'files': 0, 'folders': 0}
                    remove_tree(ftp, GAME, counts)
                    record.setdefault('stale_removed', {})[path] = counts
                elif path == PREFIX and 'stale_removed' in record:
                    counts = {'files': 0, 'folders': 0}
                    remove_tree(ftp, PREFIX, counts)
                    record['stale_removed'][path] = counts
                else:
                    raise SystemExit(f'{path} exists and is not this tool\'s: nothing changed')
        ftp.mkd(GAME)
        # Its own throwaway prefix, named: a catalog game otherwise lives in Mihawk OS (src/pw_wine_launch.h).
        game = {'name': 'Prefix, services and installer test', 'prefix': ENTRY, 'desktop': False,
                'detail': 'tools/run-prefix-test.py: removed after the run',
                'executable': 'Z:' + GAME.replace('/', '\\') + '\\prefix-probe.exe',
                'arguments': '--run 7-Zip\\7z.exe' + (' --no-services' if args.layout == 'title' else '') +
                             (' --trace ' + ','.join('+' + c for c in args.trace.split(',')) if args.trace else ''),
                'directory': 'Z:' + GAME.replace('/', '\\'),
                'env': [f'PROTON_LAYOUT={args.layout}'] + args.env}
        for name, data in ((MARKER, b'tools/run-prefix-test.py\n'), ('prefix-probe.exe', probe),
                           ('7z2409-x64.msi', package), ('game.json', (json.dumps(game, indent=2) + '\n').encode())):
            ftp.storbinary(f'STOR {GAME}/{name}', io.BytesIO(data))
            check = io.BytesIO()
            ftp.retrbinary(f'RETR {GAME}/{name}', check.write)
            if check.getvalue() != data:
                raise SystemExit(f'readback differs: {name}')
    record['deployed'] = True
    evidence.write_text(json.dumps(record, indent=2) + '\n')
    marker = 'PS5_PROTON_OWNER_BOOTSTRAP' if args.layout == 'launcher' else 'PS5_PROTON_PREFIX_PROBE'
    launch = args.name + '-launch'   # run-console.py's own record and logs
    run = subprocess.run([sys.executable, str(ROOT / 'tools/run-console.py'), '--name', launch, '--profile', ENTRY,
                          '--seconds', '600', '--marker', marker, '--exit-stall', '60'], cwd=ROOT)
    record['runner_status'] = run.returncode
    run_record = ROOT / 'evidence' / (launch + '.json')
    try:
        console_run = json.loads(run_record.read_text())
        record.update({k: console_run.get(k) for k in ('result', 'probe_verdict', 'seconds', 'returned_to_launcher')})
    except (OSError, ValueError):
        pass
    with connect(settings) as ftp:
        texts = []
        base = '/data/ps5proton/logs/sessions/0/'   # this session's helper logs (src/pw_log_sessions.h)
        for name, folder, _ in listing(ftp, base) or []:
            if not folder and re.fullmatch(r'wine-child-\d+\.log', name):
                data = io.BytesIO()
                ftp.retrbinary('RETR ' + base + name, data.write)
                (ROOT / 'klog' / f'{args.name}-{name}').write_bytes(data.getvalue())
                texts.append(data.getvalue().decode(errors='replace'))
        wine = ROOT / 'klog' / f'{launch}-wine.log'
        if wine.is_file():
            texts.append(wine.read_text(errors='replace'))
        log = io.BytesIO()
        try:
            ftp.retrbinary(f'RETR {PREFIX}/drive_c/prefix-probe-msi.log', log.write)
            (ROOT / 'klog' / f'{args.name}-msi.log').write_bytes(log.getvalue())
        except error_perm:
            pass
        steps = sorted(set(re.findall(r'prefix-probe: ([^"\\]+?)(?:\\n|")', '\n'.join(texts))))
        record['probe_lines'] = steps
        record['probe_passed'] = any('PS5_PROTON_PREFIX_PROBE PASS' in text for text in texts)
        record['helpers'] = sorted({m for text in texts for m in re.findall(r'Loaded L"([^"]+\.exe)" at', text)})
        totals = {'files': 0, 'bytes': 0}
        census(ftp, PREFIX, totals)
        record['prefix'] = totals
        record['prefix_registry'] = sorted(n for n, folder, _ in listing(ftp, PREFIX) or [] if n.endswith('.reg'))
        # Nothing is removed while the title runs: its helpers may still have the prefix's files open.
        idle = False
        for _ in range(30):
            if ' count=0 ' in ps5_console.ps5vkctl_command(settings, 'procs', timeout=15) + ' ':
                idle = True
                break
            __import__('time').sleep(2)
        record['console_idle_before_removal'] = idle
        if not idle:
            record['removed'] = 'left in place: the title was still running'
        elif not args.keep:
            removed = {}
            for path in (GAME, PREFIX):
                counts = {'files': 0, 'folders': 0}
                if listing(ftp, path) is not None:
                    remove_tree(ftp, path, counts)
                removed['game' if path == GAME else 'prefix'] = counts
            record['removed'] = removed
    record['passed'] = record['probe_passed'] and record.get('result') == 'finished'
    evidence.write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps({k: record.get(k) for k in ('run', 'passed', 'prefix', 'prefix_registry', 'helpers', 'removed')},
                     indent=2))
    return 0 if record['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
