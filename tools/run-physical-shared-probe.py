#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the 2 MiB descriptor-contract probe, compared with its POSIX host oracle.

Known console services only. Production stays untouched. Probe namespace is
unlinked immediately, and failure records are retained. No game or title launch.
"""
import argparse
import hashlib
import json
import re
import subprocess
import sys
import urllib.request
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(ROOT.parent / 'PS5_Vulkan/tools'))
import console_guard
import ps5_console


def checks(text):
    return {name: {'passed': verdict == 'PASS', 'errno': int(error)}
            for name, verdict, error in re.findall(r'check=(\S+) result=(PASS|FAIL) errno=(\d+)', text)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--name', required=True)
    args = parser.parse_args()
    if not re.fullmatch(r'[a-zA-Z0-9_-]+', args.name): parser.error('Invalid run name')
    build = ROOT / '.deps/physical-shared-probe'
    manifest = json.loads((build / 'build.json').read_text())
    for path, key in ((ROOT / 'tests/physical-shared-probe.c', 'source_sha256'),
                      (build / 'probe.elf', 'elf_sha256'), (build / 'host', 'host_sha256')):
        if hashlib.sha256(path.read_bytes()).hexdigest() != manifest[key]:
            parser.error('Probe differs from its build manifest')
    tag = 'ps5phys-' + uuid.uuid4().hex[:12]
    host = subprocess.run([str(build / 'host'), tag], capture_output=True, text=True, timeout=15)
    (ROOT / 'klog' / (args.name + '-host.log')).write_text(host.stdout + host.stderr)
    record = {'run': args.name, 'build': manifest, 'max_bytes': 2 << 20,
              'host_checks': checks(host.stdout), 'host_passed': host.returncode == 0,
              'question': 'Can a physical shared descriptor satisfy Wine byte-I/O, resize, alias, protection and lifetime contracts?',
              'production_changed': False, 'game_launched': False}
    settings = ps5_console.load_settings()
    mask = lambda value: re.sub(r'\b(?:\d{1,3}\.){3}\d{1,3}\b', '[console]', str(value))
    try:
        if host.returncode: raise RuntimeError('Host oracle failed; console run refused')
        console_guard.before_launch()
        before = ps5_console.ps5vkctl_command(settings, 'procs', timeout=5)
        if 'count=0' not in before: raise RuntimeError('Console is occupied; probe refused')
        boundary = 'PhysicalSharedContract'
        body = bytearray()
        for field, data in (('args', tag.encode()), ('pipe', b'1'), ('elf', (build / 'probe.elf').read_bytes())):
            header = f'--{boundary}\r\nContent-Disposition: form-data; name="{field}"'
            if field == 'elf': header += '; filename="shared-contract.elf"'
            body += (header + '\r\n\r\n').encode() + data + b'\r\n'
        body += f'--{boundary}--\r\n'.encode()
        request = urllib.request.Request(f'http://{settings["host"]}:8080/elfldr', data=bytes(body),
            headers={'Content-Type': 'multipart/form-data; boundary=' + boundary})
        console_guard.after_launch()
        with urllib.request.urlopen(request, timeout=45) as response:
            output = mask(response.read().decode(errors='replace'))
        (ROOT / 'klog' / (args.name + '-native.log')).write_text(output)
        record['native_checks'] = checks(output)
        record['native_summary'] = re.findall(r'PS5_PHYSICAL_CONTRACT (?:PASS|FAIL).*', output)
        record['native_census'] = re.findall(r'physical-contract phase=.*', output)
        record['post_control'] = mask(ps5_console.ps5vkctl_command(settings, 'procs', timeout=5))
        record['passed'] = ('PS5_PHYSICAL_CONTRACT PASS' in output and
                            record['native_checks'].keys() == record['host_checks'].keys() and
                            all(c['passed'] for c in record['native_checks'].values()))
    except BaseException as error:
        record['error'] = mask(error)
        record['passed'] = False
        try: record['post_control'] = mask(ps5_console.ps5vkctl_command(settings, 'procs', timeout=5))
        except BaseException: console_guard.record_drop('Physical shared probe control unavailable')
    (ROOT / 'evidence' / (args.name + '.json')).write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps(record, indent=2), flush=True)
    return 0 if record['passed'] else 1


if __name__ == '__main__': sys.exit(main())
