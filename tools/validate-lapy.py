#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Capture one owned-root lifecycle over websrv stdout and klog, with no forced kill."""
import argparse
import hashlib
import json
import re
import socket
import subprocess
import sys
import threading
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ACTIVE_EVIDENCE = None
sys.path.insert(0, str(ROOT.parent / 'PS5_Vulkan/tools'))
import ps5_console
import console_guard
from lapy_capture import validate


from console_log import LineLog, redact


def main():
    global ACTIVE_EVIDENCE
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--name', required=True)
    parser.add_argument('--library', action='store_true', help='capture the main game library instead of the picker')
    parser.add_argument('--probe', choices=['exit-probe.exe', 'wine-probe.exe', 'd3d11-probe.exe', 'xinput-probe.exe'])
    args = parser.parse_args()
    if not re.fullmatch(r'[a-zA-Z0-9_-]+', args.name):
        parser.error('use a plain run name')
    console_guard.before_launch()
    settings = ps5_console.load_settings()
    if ' count=0 ' not in ps5_console.ps5vkctl_command(settings, 'procs', timeout=15) + ' ':
        raise RuntimeError('Console is not idle')
    service = bool(args.probe)
    requests = 2 if service else 1
    build = ROOT / '.deps/lapy-validation/source/build' / (
        'owned_root_daemon-service-verified-client-max2' if service else 'owned_root_daemon')
    manifest = json.loads((build / 'manifest.json').read_text())
    elf = (build / 'lapy-root-daemon.elf').read_bytes()
    if manifest['elf_sha256'] != hashlib.sha256(elf).hexdigest():
        raise RuntimeError('Payload ELF does not match the manifest')
    if (manifest['mode'] != ('service' if service else 'one-shot') or manifest['target_title'] != 'PPSA99170'
            or manifest['max_requests'] != requests or not manifest['require_client_result']):
        raise RuntimeError('Payload mode or title does not match this validation run')
    out = ROOT / 'klog' / args.name
    out.mkdir(parents=True, exist_ok=False)
    evidence_path = ROOT / 'evidence' / (args.name + '.json')
    if evidence_path.exists():
        raise RuntimeError('Evidence already exists; use a new run name')
    ACTIVE_EVIDENCE = evidence_path
    evidence_path.write_text(json.dumps({'passed': False, 'status': 'incomplete',
        'capture': str(out.relative_to(ROOT)), 'daemon_build_id': manifest['build_id']}, indent=2) + '\n')
    (out / 'lapy-root-daemon.elf').write_bytes(elf)
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    done = threading.Event()
    errors = []
    stdout = bytearray()
    kernel = bytearray()
    sock = socket.create_connection((settings['host'], settings['klog_port']), timeout=10)
    sock.settimeout(0.5)
    deadline = time.monotonic() + 20
    replay = bytearray()
    while True:
        try:
            chunk = sock.recv(65536)
            if not chunk:
                raise RuntimeError('Klog closed before payload')
            replay.extend(chunk)
        except socket.timeout:
            break
        if time.monotonic() > deadline:
            raise RuntimeError('Klog replay never became quiet')
    (out / 'replay.log').write_text(redact(replay.decode(errors='replace')))
    def capture_kernel():
        try:
            with sock, (out / 'klog.log').open('w') as log:
                stream = LineLog(log)
                while not done.is_set():
                    try:
                        chunk = sock.recv(65536)
                    except socket.timeout:
                        continue
                    if not chunk:
                        raise RuntimeError('Klog disconnected')
                    kernel.extend(chunk)
                    stream.write(chunk)
                stream.finish()
        except BaseException as e:
            errors.append('klog: ' + redact(e))
    def capture_payload():
        try:
            boundary = 'ProtonLapyValidationBoundary'
            body = b''
            for name, data in [('args', b''), ('pipe', b'1'), ('elf', elf)]:
                header = f'--{boundary}\r\nContent-Disposition: form-data; name="{name}"'
                if name == 'elf':
                    header += '; filename="lapy-root-daemon.elf"'
                body += (header + '\r\n\r\n').encode() + data + b'\r\n'
            body += f'--{boundary}--\r\n'.encode()
            request = urllib.request.Request('http://' + settings['host'] + ':8080/elfldr', data=body,
                      headers={'Content-Type': 'multipart/form-data; boundary=' + boundary})
            with urllib.request.urlopen(request, timeout=180) as response, (out / 'daemon.log').open('w') as log:
                stream = LineLog(log)
                while True:
                    chunk = response.read1(8192)
                    if not chunk:
                        break
                    stdout.extend(chunk)
                    stream.write(chunk)
                stream.finish()
        except BaseException as e:
            errors.append('payload: ' + redact(e))
    capture = threading.Thread(target=capture_kernel)
    payload = threading.Thread(target=capture_payload)
    capture.start()
    payload.start()
    fault = re.compile(r'fatal signal|GPU_FAULT|gpu fault|mDBG: Sending signal', re.I)
    launched = False
    try:
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline and payload.is_alive():
            text = stdout.decode(errors='replace')
            if ('phase=service_baseline' if service else 'phase=baseline') in text:
                # Let the kernel deliver preflight child-exit records before using a launch slot.
                time.sleep(1)
                if errors or fault.search(kernel.decode(errors='replace')):
                    break
                print('Lapy preflight reached baseline; starting one title test.', flush=True)
                launched = True
                result = subprocess.run([sys.executable, '-B', str(ROOT / 'tools/run-launcher.py'),
                                         '--name', args.name + '-title'] + ([] if args.library else ['--themes']) +
                                        (['--probe', args.probe] if args.probe else []),
                                        cwd=ROOT, capture_output=True, text=True)
                (out / 'title-runner.log').write_text(redact(result.stdout + result.stderr))
                print('Title runner exit:', result.returncode, flush=True)
                break
            if 'daemon_result' in text or errors:
                break
            time.sleep(0.1)
        if not launched:
            print('No title launch: preflight did not establish a clean baseline.', flush=True)
        # The bounded daemon exits itself after request/exit timeouts. Do not interrupt ownership cleanup.
        while payload.is_alive():
            payload.join(1)
        time.sleep(3)
    finally:
        done.set()
        capture.join(2)
    text = stdout.decode(errors='replace')
    klog = kernel.decode(errors='replace')
    lifecycle = validate(text, manifest['build_id'], requests=requests, service=service)
    complete = lifecycle['passed']
    try:
        console_idle = ' count=0 ' in ps5_console.ps5vkctl_command(settings, 'procs', timeout=15) + ' '
    except BaseException as error:
        console_idle = False
        errors.append('control after run: ' + redact(error))
        console_guard.record_drop('Control channel unreachable after Lapy validation')
    title_path = ROOT / 'evidence' / (args.name + '-title.json')
    title = json.loads(title_path.read_text()) if launched and title_path.exists() else {}
    report = {'daemon_revision': '5b8397b9f2b5f12a7bc2f9c8745a00d1c2dd01ad',
              'daemon_elf_sha256': manifest['elf_sha256'], 'daemon_build_id': manifest['build_id'],
              'mode': manifest['mode'], 'patches': manifest.get('proton_patches', ['platform/lapy/stdout-mirror.patch', 'platform/lapy/donor-wait.patch', 'platform/lapy/donor-release.patch'] + (['platform/lapy/service-lifecycle.patch'] if service else [])),
              'title_launched': launched, 'complete_balanced_lifecycle': complete, 'lifecycle_validation': lifecycle,
              'kernel_fault_records': len(re.findall(r'A user thread receives a fatal signal', klog)),
              'kernel_fault_signatures': len(fault.findall(klog)), 'capture_errors': errors,
              'lifecycle_records': [line for line in text.splitlines() if any(key in line for key in
                  ['daemon_start', 'root_sample', 'root_layout ', 'donor_balance', 'request_result',
                   'client_result ', 'donor_reaped', 'daemon_result', 'daemon_held'])],
              'title_passed': title.get('passed', False), 'console_idle': console_idle, 'capture': str(out.relative_to(ROOT)),
              'passed': complete and title.get('passed', False) and console_idle and not fault.search(klog) and not errors}
    evidence_path.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k: report[k] for k in ('mode', 'complete_balanced_lifecycle', 'lifecycle_validation',
        'kernel_fault_records', 'capture_errors', 'title_passed', 'passed', 'capture')}, indent=2), flush=True)
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    try:
        sys.exit(main())
    except BaseException as error:
        if isinstance(error, SystemExit) and isinstance(error.code, int):
            raise
        if ACTIVE_EVIDENCE is not None and ACTIVE_EVIDENCE.exists():
            record = json.loads(ACTIVE_EVIDENCE.read_text())
            record.update(passed=False, error=redact(error))
            ACTIVE_EVIDENCE.write_text(json.dumps(record, indent=2) + '\n')
        print('Lapy validation failed: ' + redact(error), file=sys.stderr)
        sys.exit(1)
