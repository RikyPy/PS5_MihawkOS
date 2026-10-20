#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""VKD3D-Proton's own Direct3D 12 tests (its tests/d3d12.exe, built by tools/fetch-vkd3d-tests.sh), run one test per
process on the host with VKD3D-Proton's DLLs from the pinned GE-Proton, to give the baseline the console is compared with.

  tools/run-vkd3d-tests.py list
  tools/run-vkd3d-tests.py host [--timeout 120] [--force] [name...]
  tools/run-vkd3d-tests.py console --slices N [--from I] [--to J]     N launches, each a slice of the list

The program's own harness selects a test (VKD3D_TEST_MATCH, exact), prints "name:LINE: Test failed: ..." for a failure
and a summary line for the run. A test is `complete` when its summary line came, `crashed` when the process ended
without one, and `timeout` when it was killed after the time limit. Results go to evidence/wine-tests/vkd3d-host.json
in the same shape as the Wine test files', so `run-wine-tests.py compare vkd3d` reads them. The tests run inside a Wine
virtual desktop of the system Wine: a Direct3D program must not take the real screen.
"""
import argparse
import json
import os
import re
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXE = ROOT / '.deps/wine-tests/exe/vkd3d_d3d12.exe'
LIST = ROOT / '.deps/vkd3d-proton/tests/d3d12_tests.h'
EVIDENCE = Path(os.environ['PW_EVIDENCE_DIR']) if os.environ.get('PW_EVIDENCE_DIR') else ROOT / 'evidence/wine-tests'
OUT = EVIDENCE / 'vkd3d-host.json'
CONSOLE_OUT = EVIDENCE / 'vkd3d-console.json'
OVERRIDES = 'd3d11,dxgi,d3d10core,d3d9,d3d12,d3d12core=n'
BEGIN = re.compile(r'^\w+: ======== (\w+) begin ========$', re.M)
END = re.compile(r'^\w+: ======== (\w+) end =+$', re.M)
PREFIX = ROOT / '.deps/wine-tests/prefix-sys'

FAILED = re.compile(r'^(\w+):(\d+)(?:\[[^\]]*\])?: Test failed: (.*)$')
SUMMARY = re.compile(r'^\w+: (\d+) tests executed \((\d+) failures, (\d+) successful todo, (\d+) skipped, (\d+) todo, (\d+) bugs\)')


def names():
    return re.findall(r'decl_test\((\w+)\)', LIST.read_text())


def parse(text):
    failures, summary = [], None
    for line in text.splitlines():
        found = FAILED.match(line.rstrip('\r'))
        if found:
            failures.append({'file': found.group(1), 'line': int(found.group(2)), 'text': found.group(3)[:200]})
            continue
        found = SUMMARY.match(line)
        if found:
            summary = dict(zip(('tests', 'failures', 'todo_successes', 'skipped', 'todo', 'bugs'), map(int, found.groups())))
    lines = text.splitlines()
    return {'status': 'complete' if summary else 'crashed', 'summary': summary, 'failures': failures[:5000],
            'failure_count': len(failures), 'last_line': lines[-1][:200] if lines else ''}


def run_one(name, timeout):
    env = dict(os.environ, WINEPREFIX=str(PREFIX), WINEDEBUG='-all', VKD3D_DEBUG='none', DXVK_LOG_LEVEL='none',
               VKD3D_TEST_MATCH=name, WINEDLLOVERRIDES=OVERRIDES)
    started = time.monotonic()
    capture = Path(tempfile.mkstemp(prefix='vkd3d-', suffix='.txt')[1])
    command = ['wine', 'explorer', '/desktop=pwtests,1280x720', 'cmd', '/c',
               f'Z:{EXE} > Z:{str(capture).replace("/", chr(92))} 2>&1']
    command[-1] = f'Z:{str(EXE).replace("/", chr(92))} > Z:{str(capture).replace("/", chr(92))} 2>&1'
    child = subprocess.Popen(command, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
    timed_out = False
    try:
        child.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        timed_out = True
        os.killpg(child.pid, signal.SIGKILL)
        child.wait()
    text = capture.read_text(errors='replace')
    capture.unlink()
    record = parse(text)
    if timed_out:
        record['status'] = 'timeout'
    record['seconds'] = round(time.monotonic() - started, 1)
    return record


def console_slices(slices, first, last):
    """The console runs the suite in slices of its list (VKD3D_TEST_RANGE, tests/vkd3d-test-range.patch), one launch each.
    A test that crashes the title is recorded and the next launch starts after it; one that stops making progress ends
    the program itself (VKD3D_TEST_TIMEOUT) after naming itself."""
    order = names()
    total = len(order)
    last = min(last or total, total)
    step = -(-(last - first) // slices)
    record = json.loads(CONSOLE_OUT.read_text()) if CONSOLE_OUT.is_file() else {}
    start = first
    while start < last:
        stop = min(start + step, last)
        run = f'vkd3d-{start}-{stop}'
        command = [sys.executable, str(ROOT / ('tools/stub-console.py' if os.environ.get('PW_STUB_CONSOLE') else 'tools/run-console.py')), '--exe', 'vkd3d_d3d12.exe', '--seconds', '600',
                   '--watchdog', '700', '--name', run, '--env', f'VKD3D_TEST_RANGE={start}-{stop}',
                   '--env', 'VKD3D_TEST_TIMEOUT=120', '--env', 'VKD3D_DEBUG=none', '--env', 'DXVK_LOG_LEVEL=none',
                   '--env', f'WINEDLLOVERRIDES={OVERRIDES}']
        print(f'launch {run}', flush=True)
        result = subprocess.run(command, capture_output=True, text=True)
        combined = result.stdout + result.stderr
        if 'did not answer' in combined or 'no launch' in combined:
            raise SystemExit('stopping: ' + combined.strip().splitlines()[-1][:200])
        stdout_file = ROOT / f'klog/{run}-stdout.txt'
        text = stdout_file.read_text(errors='replace') if stdout_file.is_file() else ''
        begun = BEGIN.findall(text)
        ended = set(END.findall(text))
        for name in begun:
            body = text[text.index(f'======== {name} begin'):]
            body = body[:body.index(f'======== {name} end')] if name in ended else body
            entry = parse(body + ('\nsummary-not-needed' if name in ended else ''))
            entry['status'] = 'complete' if name in ended else 'crashed'
            entry['summary'] = None
            record[name] = entry
        timed_out = re.search(r'^vkd3d-batch: timeout (\w+) (\d+)$', text, re.M)
        if timed_out and timed_out.group(1) in record:
            record[timed_out.group(1)]['status'] = 'timeout'
        CONSOLE_OUT.write_text(json.dumps(record, indent=1) + '\n')
        print(f'  {len(begun)} tests begun, {len(ended)} ended', flush=True)
        if not begun:
            raise SystemExit('stopping: nothing ran in this launch')
        if begun[-1] not in ended or timed_out:
            start = order.index(begun[-1]) + 1   # the test that ended the program is recorded; go on after it
        else:
            start = stop


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('command', choices=['list', 'host', 'console'])
    parser.add_argument('--slices', type=int, default=0)
    parser.add_argument('--from', dest='first', type=int, default=0)
    parser.add_argument('--to', dest='last', type=int, default=0)
    parser.add_argument('tests', nargs='*')
    parser.add_argument('--timeout', type=int, default=120)
    parser.add_argument('--force', action='store_true')
    args = parser.parse_args()
    todo = args.tests or names()
    if args.command == 'list':
        print('\n'.join(todo))
        return 0
    if args.command == 'console':
        if not args.slices:
            raise SystemExit('--slices N is needed')
        console_slices(args.slices, args.first, args.last)
        return 0
    record = json.loads(OUT.read_text()) if OUT.is_file() else {}
    for index, name in enumerate(todo):
        if not args.force and record.get(name, {}).get('status') in ('complete', 'crashed', 'timeout'):
            continue
        record[name] = run_one(name, args.timeout)
        entry = record[name]
        print(f"[{index + 1}/{len(todo)}] {name}: {entry['status']}, {entry['failure_count']} failing lines, {entry['seconds']} s", flush=True)
        OUT.write_text(json.dumps(record, indent=1) + '\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
