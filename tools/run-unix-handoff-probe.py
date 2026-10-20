#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the unix-socket handoff probe on the console, compared with its Linux oracle.

Question: does the console's kernel flush a unix socket that is in transit through
SCM_RIGHTS when its sender has already closed its own copy (tests/unix-handoff-probe.c)?
Known console services only; no title, no Wine, production untouched.

  tools/build-unix-handoff-probe.sh
  tools/run-unix-handoff-probe.py --name <run>
"""
import argparse
import hashlib
import json
import re
import subprocess
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(ROOT.parent / 'PS5_Vulkan/tools'))
import console_guard
import ps5_console

FIELDS = ('handoffs', 'healthy', 'flushed', 'flushed_readable_without_hangup', 'delivered_after_flush',
          'anomalies', 'errors')


def modes(text):
    found = {}
    for line in re.findall(r'unix-handoff mode=\S+ .*', text):
        mode = re.search(r'mode=(\S+)', line).group(1)
        found[mode] = {key: int(re.search(rf'\b{key}=(-?\d+)', line).group(1)) for key in FIELDS}
        found[mode]['first_error'] = re.search(r'first_error=(\S+) errno=(\d+)', line).group(0)
        found[mode]['ms'] = int(re.search(r'ms=(\d+)', line).group(1))
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--name', required=True)
    parser.add_argument('--which', choices=('both', 'early', 'held'), default='both')
    args = parser.parse_args()
    if not re.fullmatch(r'[a-zA-Z0-9_-]+', args.name): parser.error('Invalid run name')
    build = ROOT / '.deps/unix-handoff-probe'
    manifest = json.loads((build / 'build.json').read_text())
    for path, key in ((ROOT / 'tests/unix-handoff-probe.c', 'source_sha256'),
                      (build / 'probe.elf', 'elf_sha256'), (build / 'host', 'host_sha256')):
        if hashlib.sha256(path.read_bytes()).hexdigest() != manifest[key]:
            parser.error('Probe differs from its build manifest; run tools/build-unix-handoff-probe.sh')
    host = subprocess.run([str(build / 'host'), args.which, 's=6'], capture_output=True, text=True, timeout=60)
    (ROOT / 'klog' / (args.name + '-host.log')).write_text(host.stdout + host.stderr)
    record = {'run': args.name, 'build': manifest, 'which': args.which,
              'question': 'Does the console flush a unix socket in SCM_RIGHTS transit once its sender closed its copy, '
                          'and does holding the copy until the receiver acknowledges prevent it?',
              'host': modes(host.stdout), 'production_changed': False, 'title_launched': False}
    settings = ps5_console.load_settings()
    mask = lambda value: re.sub(r'\b(?:\d{1,3}\.){3}\d{1,3}\b', '[console]', str(value))
    try:
        if host.returncode or 'UNIX_HANDOFF' not in host.stdout: raise RuntimeError('Host oracle failed; console run refused')
        if any(m['flushed'] or m['errors'] or m['anomalies'] for m in record['host'].values()):
            raise RuntimeError('Host oracle saw flushes or errors; console run refused')
        console_guard.before_launch()
        before = ps5_console.ps5vkctl_command(settings, 'procs', timeout=5)
        if 'count=0' not in before: raise RuntimeError('Console is occupied; probe refused')
        boundary = 'UnixHandoffProbe'
        body = bytearray()
        for field, data in (('args', args.which.encode()), ('pipe', b'1'), ('elf', (build / 'probe.elf').read_bytes())):
            header = f'--{boundary}\r\nContent-Disposition: form-data; name="{field}"'
            if field == 'elf': header += '; filename="unix-handoff.elf"'
            body += (header + '\r\n\r\n').encode() + data + b'\r\n'
        body += f'--{boundary}--\r\n'.encode()
        request = urllib.request.Request(f'http://{settings["host"]}:8080/elfldr', data=bytes(body),
            headers={'Content-Type': 'multipart/form-data; boundary=' + boundary})
        console_guard.after_launch()
        with urllib.request.urlopen(request, timeout=90) as response:
            output = mask(response.read().decode(errors='replace'))
        (ROOT / 'klog' / (args.name + '-native.log')).write_text(output)
        record['native'] = modes(output)
        record['native_summary'] = re.findall(r'UNIX_HANDOFF .*', output)
        record['post_control'] = mask(ps5_console.ps5vkctl_command(settings, 'procs', timeout=5))
        native = record['native']
        completed = bool(record['native_summary']) and all(
            mode in native and native[mode]['handoffs'] > 0 and not native[mode]['errors']
            for mode in (('early', 'held') if args.which == 'both' else (args.which,)))
        record['completed'] = completed
        record['early_flushes'] = native.get('early', {}).get('flushed')
        record['held_flushes'] = native.get('held', {}).get('flushed')
        # The hypothesis needs flushes when the sender closes early and none when it holds its copy.
        record['hypothesis_shown'] = completed and bool(record['early_flushes']) and record['held_flushes'] == 0
        record['passed'] = completed and record['held_flushes'] in (0, None)
    except BaseException as error:
        record['error'] = mask(error)
        record['passed'] = False
        try: record['post_control'] = mask(ps5_console.ps5vkctl_command(settings, 'procs', timeout=5))
        except BaseException: console_guard.record_drop('Unix handoff probe control unavailable')
    (ROOT / 'evidence' / (args.name + '.json')).write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps(record, indent=2), flush=True)
    return 0 if record['passed'] else 1


if __name__ == '__main__': sys.exit(main())
