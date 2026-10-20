#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the desktop input gate with the player's actual saved left-click mapping."""
import argparse
import json
import re
import runpy
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def mapped_script(text, button):
    if button == 'none':
        raise ValueError('left click is unassigned: this input check needs a real mapped button')
    replacement = button + (' 1 0.2' if button in ('l2', 'r2') else '')
    return re.sub(r'(?m)^(\d+(?:\.\d+)?) touchpad[ \t]*$', lambda m: m[1] + ' ' + replacement, text)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--name', required=True)
    args = parser.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9_-]+', args.name):
        parser.error('use a plain run name')
    evidence = ROOT / 'evidence' / (args.name + '.json')
    if evidence.exists():
        parser.error('use a new run name')
    support = runpy.run_path(str(ROOT / 'tools/run-mouse-controls.py'))
    settings = support['console'].load_settings()
    with support['connect'](settings) as ftp:
        original = support['read'](ftp)
    with tempfile.TemporaryDirectory() as directory:
        saved = Path(directory) / 'controls.txt'
        if original is not None:
            saved.write_bytes(original)
        output = subprocess.check_output([str(ROOT / '.deps/controls-id'), str(saved)], text=True).split()
        button, parser_status = output[0], int(output[1])
        script = Path(directory) / 'desktop-pointer.txt'
        script.write_text(mapped_script((ROOT / 'tests/pad-scripts/desktop-pointer.txt').read_text(), button))
        result = subprocess.run([sys.executable, str(ROOT / 'tools/run-console.py'), '--name', args.name,
            '--exe', 'desktop-probe.exe', '--marker', 'PS5_PROTON_OWNER_BOOTSTRAP', '--watchdog', '200',
            '--pad-script', str(script), '--env', 'PROTON_LAYOUT=launcher'], cwd=ROOT)
    with support['connect'](settings) as ftp:
        unchanged = support['read'](ftp) == original
    if evidence.exists():
        record = json.loads(evidence.read_text())
        record['input_mapping'] = {'left': button, 'parser_status': parser_status,
                                   'source': 'saved' if parser_status == 0 else 'production defaults',
                                   'preferences_unchanged': unchanged}
        record['passed'] = record.get('passed', False) and unchanged
        evidence.write_text(json.dumps(record, indent=1) + '\n')
        return 0 if record['passed'] else 1
    return result.returncode or 1


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Exception as error:
        print('desktop input runner failed: ' + re.sub(r'\b\d{1,3}(\.\d{1,3}){3}\b', '<console>', str(error)), file=sys.stderr)
        sys.exit(1)
