#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Qualify physical SCM_RIGHTS between two independent payloads (2 MiB maximum).

The same source/roles run on Linux POSIX shm first. One guarded logical console
job uses two payloads; no title is launched or production changed. A missing
marker, failed check, unreleased backing or unavailable control fails the run.
"""
import argparse
import concurrent.futures
import hashlib
import importlib.util
import json
import re
import subprocess
import sys
import threading
import urllib.request
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(ROOT.parent / 'PS5_Vulkan/tools'))
import console_guard
import ps5_console

def mask(value):
    return re.sub(r'\b(?:\d{1,3}\.){3}\d{1,3}\b', '[console]', str(value))


def review(role, output):
    cases = {name: {'passed': result == 'PASS', 'errno': int(error)}
             for name, result, error in re.findall(r'check=(\S+) result=(PASS|FAIL) errno=(\d+)', output)}
    census = {phase: {'status': int(status, 0), 'available': int(available),
                      'resident_status': int(resident, 0), 'pages': int(pages)}
              for phase, status, available, resident, pages in re.findall(
                  r'phase=(\S+) flexible_status=(\S+) available=(\d+) resident_status=(\S+) pages=(\d+)', output)}
    return {'role': role, 'checks': cases, 'census': census,
            'summary': re.findall(r'PS5_PHYSICAL_TRANSFER .*', output),
            'passed': bool(cases) and all(c['passed'] for c in cases.values()) and
                      f'PS5_PHYSICAL_TRANSFER PASS role={role}' in output}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--name', required=True)
    args = parser.parse_args()
    if not re.fullmatch(r'[a-zA-Z0-9_-]+', args.name): parser.error('Invalid run name')
    evidence = ROOT / 'evidence' / (args.name + '.json')
    if evidence.exists(): parser.error('Run name already exists; evidence is immutable')
    build = ROOT / '.deps/physical-shared-transfer'
    manifest = json.loads((build / 'build.json').read_text())
    for path, key in ((ROOT / 'tests/physical-shared-transfer.c', 'source_sha256'),
                      (build / 'host', 'host_sha256'), (build / 'probe.elf', 'elf_sha256')):
        if hashlib.sha256(path.read_bytes()).hexdigest() != manifest[key]:
            parser.error('Probe differs from build manifest')
    tag = 'ps5phys-' + uuid.uuid4().hex[:12]
    record = {'run': args.name, 'build': manifest, 'max_bytes': 2 << 20,
              'question': 'Do transferred physical descriptors preserve coherence, exact fixed offset maps and last-map lifetime?',
              'production_changed': False, 'game_launched': False}
    creator = subprocess.Popen([str(build / 'host'), 'creator', tag], stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True)
    try:
        reader = subprocess.run([str(build / 'host'), 'reader', tag], capture_output=True, text=True, timeout=20)
        creator_output = creator.communicate(timeout=20)[0]
    except BaseException:
        creator.kill(); creator.communicate(); raise
    host_output = {'creator': creator_output, 'reader': reader.stdout + reader.stderr}
    record['host'] = [review(role, output) for role, output in host_output.items()]
    for role, output in host_output.items():
        (ROOT / 'klog' / (args.name + '-host-' + role + '.log')).write_text(output)
    settings = ps5_console.load_settings()
    creator_ready = threading.Event()

    def launch(role):
        boundary = 'PhysicalSharedTransfer'
        body = bytearray()
        for field, data in (('args', f'{role} {tag}'.encode()), ('pipe', b'1'),
                            ('elf', (build / 'probe.elf').read_bytes())):
            header = f'--{boundary}\r\nContent-Disposition: form-data; name="{field}"'
            if field == 'elf': header += '; filename="physical-transfer.elf"'
            body += (header + '\r\n\r\n').encode() + data + b'\r\n'
        body += f'--{boundary}--\r\n'.encode()
        request = urllib.request.Request(f'http://{settings["host"]}:8080/elfldr', data=bytes(body),
            headers={'Content-Type': 'multipart/form-data; boundary=' + boundary})
        try:
            with urllib.request.urlopen(request, timeout=35) as response:
                lines = []
                for line in response:
                    decoded = mask(line.decode(errors='replace'))
                    lines.append(decoded)
                    if role == 'creator' and 'check=listen result=PASS' in decoded:
                        creator_ready.set()
                output = ''.join(lines)
            (ROOT / 'klog' / (args.name + '-native-' + role + '.log')).write_text(output)
            return review(role, output)
        except BaseException as error:
            return {'role': role, 'passed': False, 'error': mask(error)}
    try:
        if creator.returncode or reader.returncode or not all(r['passed'] for r in record['host']):
            raise RuntimeError('Host oracle failed; console run refused')
        console_guard.before_launch()
        before = ps5_console.ps5vkctl_command(settings, 'procs', timeout=5)
        if 'count=0' not in before: raise RuntimeError('Console is occupied; probe refused')
        spec = importlib.util.spec_from_file_location('transfer_console_runner', ROOT / 'tools/run-console.py')
        runner = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(runner)
        leftovers = runner.leftover_helpers()
        if leftovers is None: raise RuntimeError('Payload census unavailable; probe refused')
        if leftovers: raise RuntimeError('Earlier payloads still present; probe refused until a fresh boot')
        console_guard.after_launch()
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            creator_job = pool.submit(launch, 'creator')
            # Wait for actual native listener readiness in the streamed output;
            # arbitrary sleeps do not establish that elfldr finished its upload.
            if creator_ready.wait(8):
                reader_job = pool.submit(launch, 'reader')
                record['native'] = [creator_job.result(), reader_job.result()]
            else:
                record['native'] = [creator_job.result(),
                                    {'role': 'reader', 'passed': False, 'error': 'Creator never announced readiness; upload refused'}]
        record['post_control'] = mask(ps5_console.ps5vkctl_command(settings, 'procs', timeout=5))
        record['passed'] = 'count=0' in record['post_control'] and all(r['passed'] for r in record['native'])
        for native, host in zip(record['native'], record['host']):
            record['passed'] &= native.get('checks', {}).keys() == host['checks'].keys()
            stats = native.get('census', {})
            baseline, released = stats.get('baseline', {}), stats.get('released', {})
            reclaimed = (baseline.get('status') == 0 and released.get('status') == 0 and
                         baseline.get('resident_status') == 0 and released.get('resident_status') == 0 and
                         released.get('available', 0) >= baseline.get('available', 1) and
                         released.get('pages', 1) <= baseline.get('pages', 0))
            native['backing_reclaimed'] = reclaimed
            record['passed'] &= reclaimed
    except BaseException as error:
        record['passed'] = False; record['error'] = mask(error)
        try: record['post_control'] = mask(ps5_console.ps5vkctl_command(settings, 'procs', timeout=5))
        except BaseException: console_guard.record_drop('Physical shared transfer control unavailable')
    evidence.write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps(record, indent=2), flush=True)
    return 0 if record['passed'] else 1


if __name__ == '__main__': sys.exit(main())
