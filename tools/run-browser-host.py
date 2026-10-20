#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""MihawkBrowser on the pinned Wine on Linux (docs/MIHAWK_BROWSER.md, H3): one scripted run that ends in a verdict.

Runs .deps/mihawk-browser/app/MihawkBrowser.exe (tools/build-mihawk-browser.sh) in a host prefix copied from the staged
console prefix (fonts, theme and DXVK as the console has them), drives it with MIHAWK_BROWSER_SCRIPT, samples every
MihawkBrowser process of that prefix from /proc, and writes evidence/<name>.json: the browser's own "browser:" lines
(they carry no query, path or address), each step's outcome, the download's size and SHA-256, the hand-off, and per
process its type and peak RSS, PSS and private memory. Host memory is a PC oracle, not console proof.

The host Wine is built without a display driver, so it runs with Wine's null driver and Chromium's software compositor
(--disable-gpu --disable-gpu-compositing --disable-software-rasterizer: with no GL at all, anything else retries a GPU
context in a loop, 1.4 million times in 3 minutes in browser-host-21). The graphics paths are proven on the console.

    tools/run-browser-host.py --name browser-host-2026-10-16-01 [--script "load ...; download URL; open; exit"]

--snapshots puts the script's "snapshot NAME" bitmaps in klog/<name>-snapshots (the window's client at the prototype's
1278 x 766, docs/MIHAWK_BROWSER.md §4.1) for tools/check-browser-reference.py; --dark runs the dark theme.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from console_log import redact  # noqa: E402
import browser_lines  # noqa: E402

WINE = ROOT / '.deps/wine-runtime/build/wine'
APP = ROOT / '.deps/mihawk-browser/app'
PREFIX = ROOT / '.deps/browser-host-prefix'
DEFAULT_SCRIPT = ('load https://example.com/; load https://www.winehq.org/; load wine download; '
                  'load https://en.wikipedia.org/wiki/Web_browser; wait 3000; exit')
SOFTWARE = ['--disable-gpu', '--disable-gpu-compositing', '--disable-software-rasterizer']
# Utility processes the Chrome bootstrap starts (named transients, docs/MIHAWK_BROWSER.md §3.1).
TRANSIENTS = {'chrome.mojom.ProcessorMetrics', 'quarantine.mojom.Quarantine'}


def prepare_prefix(template, dark):
    if PREFIX.exists(): shutil.rmtree(PREFIX)
    shutil.copytree(template, PREFIX, symlinks=True)
    # the icons this build draws with, newer than the staged prefix's until the next stage
    shutil.copy2(ROOT / '.deps/mihawk-icons/icons.dll', PREFIX / 'drive_c/windows/resources/mihawk/icons.dll')
    devices = PREFIX / 'dosdevices'
    for name, target in (('c:', '../drive_c'), ('z:', '/')):  # the staged prefix records them in .pw-symlinks
        if not (devices / name).exists(): os.symlink(target, devices / name)
    environment = dict(os.environ, WINEPREFIX=str(PREFIX), WINEDEBUG='-all')
    subprocess.run([str(WINE), 'reg', 'add', r'HKCU\Software\Wine\Drivers', '/v', 'Graphics', '/d', 'null', '/f'],
                   env=environment, check=True, capture_output=True, timeout=120)
    subprocess.run([str(WINE), 'reg', 'add', r'HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize',
                    '/v', 'AppsUseLightTheme', '/t', 'REG_DWORD', '/d', '0' if dark else '1', '/f'],
                   env=environment, check=True, capture_output=True, timeout=120)
    subprocess.run([str(WINE.parent / 'server/wineserver'), '-w'], env=environment, timeout=120)


def ours(pid):
    try:
        environ = Path(f'/proc/{pid}/environ').read_bytes().split(b'\0')
        command = Path(f'/proc/{pid}/cmdline').read_bytes().replace(b'\0', b' ').decode(errors='replace')
    except OSError:
        return None
    if f'WINEPREFIX={PREFIX}'.encode() not in environ or 'MihawkBrowser.exe' not in command: return None
    kind = re.search(r'--type=(\S+)', command)
    service = re.search(r'--utility-sub-type=(\S+)', command)
    return (kind.group(1) if kind else 'browser'), (service.group(1) if service else None)


def memory(pid):
    values = {}
    try:
        for line in Path(f'/proc/{pid}/smaps_rollup').read_text().splitlines()[1:]:
            key, value = line.split(':', 1)
            values[key] = int(value.split()[0])
    except (OSError, ValueError):
        return None
    return {'rss_kib': values.get('Rss', 0), 'pss_kib': values.get('Pss', 0),
            'private_kib': values.get('Private_Clean', 0) + values.get('Private_Dirty', 0)}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--name', required=True)
    parser.add_argument('--script', default=DEFAULT_SCRIPT)
    parser.add_argument('--expect-sha256', help="the download's SHA-256, when --script downloads (download URL; open)")
    parser.add_argument('--template', default=str(ROOT / '.deps/staged/prefixes/probe'),
                        help='the staged console prefix (tools/stage-wine.py)')
    parser.add_argument('--timeout', type=int, default=300)
    parser.add_argument('--snapshots', action='store_true')
    parser.add_argument('--dark', action='store_true')
    parser.add_argument('flags', nargs='*', help='extra switches for MihawkBrowser.exe')
    args = parser.parse_args()
    if not re.fullmatch(r'[a-zA-Z0-9_-]+', args.name): parser.error('use a plain name')
    evidence = ROOT / 'evidence' / f'{args.name}.json'
    if evidence.exists(): parser.error(f'{evidence} exists')
    template = Path(args.template)
    if not (template / 'system.reg').is_file(): parser.error(f'no staged prefix at {template}')
    prepare_prefix(template, args.dark)
    log_path = ROOT / 'klog' / f'{args.name}.log'
    log_path.parent.mkdir(exist_ok=True)
    environment = dict(os.environ, WINEPREFIX=str(PREFIX), WINEDEBUG='-all', MIHAWK_BROWSER_SCRIPT=args.script,
                       WINEDLLOVERRIDES='winemenubuilder.exe=d;mscoree,mshtml=')
    if args.snapshots:
        snapshots = ROOT / 'klog' / f'{args.name}-snapshots'
        snapshots.mkdir(parents=True, exist_ok=True)
        environment.update(MIHAWK_BROWSER_SNAPSHOTS='Z:' + str(snapshots).replace('/', '\\'), MIHAWK_BROWSER_CLIENT='0,0')
    started = time.monotonic()
    processes = {}  # pid -> record
    with open(log_path, 'wb') as log:
        browser = subprocess.Popen([str(WINE), str(APP / 'MihawkBrowser.exe'), *SOFTWARE, *args.flags], env=environment,
                                   stdout=log, stderr=log)
        while browser.poll() is None and time.monotonic() - started < args.timeout:
            now = round(time.monotonic() - started, 2)
            for entry in os.listdir('/proc'):
                if not entry.isdigit(): continue
                kind = ours(entry)
                sample = kind and memory(entry)
                if not sample: continue
                record = processes.setdefault(entry, {'type': kind[0], 'service': kind[1], 'first_s': now})
                record['last_s'] = now
                for key, value in sample.items(): record['peak_' + key] = max(record.get('peak_' + key, 0), value)
            time.sleep(0.2)
        timed_out = browser.poll() is None
        if timed_out: browser.kill()
    subprocess.run([str(WINE.parent / 'server/wineserver'), '-k'], env=environment, timeout=60)
    lines = [redact(line) for line in log_path.read_text(errors='replace').splitlines() if line.startswith('browser: ')]
    rows = sorted(processes.values(), key=lambda r: r['first_s'])
    for row in rows: row['lifetime_s'] = round(row.pop('last_s') - row['first_s'], 2)
    exe = APP / 'MihawkBrowser.exe'
    record = {
        'run': args.name, 'oracle': 'pinned Wine on Linux, null display driver, software compositing',
        'wine_revision': (ROOT / '.deps/wine-runtime/revision').read_text().strip(),
        'cef': json.loads(next((ROOT / '.deps/cef').glob('cef_binary_*/ship.json')).read_text())['name'],
        'exe_sha256': hashlib.sha256(exe.read_bytes()).hexdigest(), 'flags': SOFTWARE + args.flags,
        'script': args.script, 'prefix_template': os.path.relpath(template, ROOT),
        'seconds': round(time.monotonic() - started, 1), 'timed_out': timed_out, 'exit_code': browser.returncode,
        **browser_lines.summary(lines, args.script, args.expect_sha256),
        'processes': rows, 'process_count_max': len(rows),
        'persistent_processes': len([r for r in rows if r['service'] not in TRANSIENTS]),
        'browser_lines': lines,
    }
    record['passed'] = record['passed'] and not timed_out and browser.returncode == 0
    evidence.write_text(json.dumps(record, indent=1) + '\n')
    print(json.dumps({k: record[k] for k in ('passed', 'startup', 'download', 'handoff', 'process_count_max')}, indent=1))
    for row in rows:
        print(f"  {row['type']:9} {row['service'] or '':32} rss {row['peak_rss_kib'] // 1024:4} MiB  pss "
              f"{row['peak_pss_kib'] // 1024:4} MiB  private {row['peak_private_kib'] // 1024:4} MiB  {row['lifetime_s']} s")
    return 0 if record['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
