#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The registry reaches the disk while a session runs (the title asks the wineserver for a save every 30 s).

Runs tests/registry-flush-probe.c through tools/run-console.py and, while it still runs, reads the probe prefix's user.reg
over FTP at 12 s (before the first save: the value on disk must still be the one from before the run, so the check
can tell) and at 45 s (after it: it must be this run's). Nothing is killed. The answer goes to evidence/<name>.json.

  tools/run-registry-flush.py --name registry-flush-2026-10-11-01
"""
import argparse
import json
import re
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT.parent / 'PS5_Vulkan/tools'))
import ps5_console  # noqa: E402
import ftplib  # noqa: E402

HIVE = '/data/ps5proton/prefixes/probe/user.reg'
KEY = r'[Software\\MihawkOS\\FlushProbe]'


def on_disk(settings):
    lines = []
    ftp = ftplib.FTP()
    ftp.connect(settings['host'], settings['ftp_port'], timeout=20)
    ftp.login(settings['ftp_user'], settings['ftp_password'])
    ftp.retrlines('RETR ' + HIVE, lines.append)
    ftp.quit()
    text = '\n'.join(lines)
    at = text.find(KEY)
    return (re.search(r'"stamp"=dword:([0-9a-f]+)', text[at:at + 300]).group(1) if at >= 0 and '"stamp"' in text[at:at + 300] else None)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--name', required=True)
    args = parser.parse_args()
    settings = ps5_console.load_settings()
    run = subprocess.Popen([sys.executable, str(ROOT / 'tools/run-console.py'), '--exe', 'registry-flush64-probe.exe',
                            '--seconds', '75', '--name', args.name], cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    output = []
    threading.Thread(target=lambda: [output.append(line) for line in run.stdout], daemon=True).start()
    before = on_disk(settings)
    started = None
    while started is None and run.poll() is None:
        if any('PS5_PROTON_REGISTRY_FLUSH' in line or 'alive stage' in line for line in output):
            started = time.monotonic()
        time.sleep(0.5)
    samples = {}
    for label, at in (('early', 12), ('late', 45)):
        while started is not None and time.monotonic() - started < at and run.poll() is None:
            time.sleep(0.5)
        try:
            samples[label] = {'seconds': round(time.monotonic() - started, 1) if started else None, 'stamp': on_disk(settings)}
        except Exception as error:
            samples[label] = {'error': str(error)[:120]}
    run.wait()
    early, late = samples.get('early', {}).get('stamp'), samples.get('late', {}).get('stamp')
    passed = early == before and late is not None and late != before
    record = {'run': args.name, 'what': 'registry value on disk during a session', 'before_run': before, 'samples': samples, 'passed': passed,
              'runner_exit': run.returncode}
    (ROOT / 'evidence' / (args.name + '.json')).write_text(json.dumps(record, indent=1) + '\n')
    print(json.dumps(record, indent=1))
    return 0 if passed else 1


if __name__ == '__main__':
    sys.exit(main())
