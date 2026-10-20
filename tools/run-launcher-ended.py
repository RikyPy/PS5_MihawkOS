#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The gate's launcher-ended case: the launcher says why the last session ended (docs/SELF_INSTALL.md, phase 1).

Two launches through tools/run-launcher.py, each with its own evidence/<name>-<leg>.json:
  ended   a session for a library entry that does not exist fails at once (native/proton_wine.cpp fail); the
          launcher it returns to shows "Last session ended: <that reason>"
  marker  a session's marker left in the title folder, as a crash, a freeze or a power cut leaves it; the launcher
          shows that the last session did not end normally, and removes the marker
In both, the launcher must find the runtime it was built with (no "games cannot start": runtime-stamp matched).

  tools/run-launcher-ended.py --name NAME
"""
import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MISSING = 'gate-no-such-entry'
LEGS = {
    'ended': (['--profile', MISSING],
              f'Last session ended: Wine: requested profile {MISSING} is not in the library'),
    'marker': (['--plant-marker', 'Gate marker'],
               'The last session (Gate marker) did not end normally: it crashed, froze or lost power. '
               'Its log: /data/ps5proton/logs/title-last.log'),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--name', required=True)
    args = parser.parse_args()
    if not re.fullmatch(r'[a-zA-Z0-9_-]+', args.name):
        parser.error('use a plain run name')
    evidence = ROOT / 'evidence' / (args.name + '.json')
    if evidence.exists():
        parser.error('use a new run name')
    record = {'case': 'launcher-ended', 'legs': {}}
    idle = True
    for leg, (options, expected) in LEGS.items():
        run = f'{args.name}-{leg}'
        result = subprocess.run([sys.executable, str(ROOT / 'tools/run-launcher.py'), '--name', run, '--frames', '120']
                                + options, cwd=ROOT)
        path = ROOT / 'evidence' / (run + '.json')
        launched = json.loads(path.read_text()) if path.exists() else {}
        said = launched.get('library') or []
        outcome = {'run': run, 'runner_status': result.returncode, 'launcher_passed': bool(launched.get('passed')),
                   'said': any(expected in line for line in said),
                   'runtime_matched': not any('games cannot start' in line for line in said),
                   'marker_left': launched.get('session_marker_left'), 'launcher_lines': said}
        outcome['passed'] = (result.returncode == 0 and outcome['launcher_passed'] and outcome['said'] and
                             outcome['runtime_matched'] and outcome['marker_left'] is False)
        record['legs'][leg] = outcome
        evidence.write_text(json.dumps(record, indent=2) + '\n')
        idle = bool(launched.get('shell_idle_after_run'))
        if not idle:
            break   # the title did not close: nothing more can run
    record['passed'] = len(record['legs']) == len(LEGS) and all(leg['passed'] for leg in record['legs'].values())
    # What the battery reads of every case (tools/run-battery.py launch_outcome).
    record['result'] = 'finished' if idle else 'title still running'
    record['returned_to_launcher'] = idle
    evidence.write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps(record, indent=2))
    return 0 if record['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
