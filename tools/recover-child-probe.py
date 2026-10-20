#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Retrieve an interrupted probe's durable trace and restore its signed launcher backup. No launch."""
import argparse
import hashlib
import importlib.util
import io
import json
import re
import sys
import time
from ftplib import FTP, error_perm, error_reply
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT.parent / 'PS5_Vulkan/tools'))
sys.path.insert(0, str(ROOT / 'tools'))
import ps5_console
import console_guard
from console_log import redact

spec = importlib.util.spec_from_file_location('child_probe_runner', ROOT / 'tools/run-child-probe.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--name', required=True)
    args = parser.parse_args()
    if not re.fullmatch(r'[a-zA-Z0-9_-]+', args.name):
        parser.error('use a plain run name')
    path = ROOT / 'evidence' / (args.name + '.json')
    record = json.loads(path.read_text())
    probe_name = record['kind'] if record.get('kind') in ('service-preflight', 'service-native') else 'child-probe'
    run = ROOT / record['capture']
    production = (run / 'production-eboot.bin').read_bytes()
    if (hashlib.sha256(production).hexdigest() != record['production_sha256'] or
            production[:4] != bytes.fromhex('4f153d1d')):
        raise RuntimeError('Signed production backup does not match the run record')
    if (ROOT / 'dist/PPSA99170/eboot.bin').read_bytes() != production:
        raise RuntimeError('Current production build differs from the saved backup; recover its matching ELF first')
    expected = runner.executable_sha((ROOT / 'build/ps5/eboot.elf').read_bytes())
    settings = ps5_console.load_settings()

    def idle():
        return ' count=0 ' in ps5_console.ps5vkctl_command(settings, 'procs', timeout=15) + ' '

    if not idle():
        raise RuntimeError('Console is running an application; no restoration performed')
    output = run / 'recovery'
    output.mkdir(parents=True, exist_ok=False)
    base = '/data/homebrew/PPSA99170/'
    with console_guard.writing('probe', settings), FTP() as ftp:
        ftp.connect(settings['host'], settings['ftp_port'], timeout=30)
        ftp.login(settings['ftp_user'], settings['ftp_password'] or 'codex')

        def download(remote):
            sink = io.BytesIO()
            try:
                ftp.retrbinary('RETR ' + remote, sink.write, blocksize=1 << 20)
            except error_perm as error:
                if not str(error).startswith('550'):
                    raise
                return None
            return sink.getvalue()

        results = download(base + probe_name + '-results.txt')
        if results is not None:
            trace = redact(results.decode(errors='replace'))
            (output / 'results.txt').write_text(trace)
            fresh = (probe_name + ' identity=' + record['identity'] + ' ') in trace
            record['recovered_fresh_identity_seen'] = fresh
            if fresh:
                record['observations'] = trace.splitlines()
                print(trace, end='' if trace.endswith('\n') else '\n', flush=True)
        daemon = download('/data/ps5proton/logs/lapy-service.log')
        if daemon is not None:
            (output / 'daemon.log').write_text(redact(daemon.decode(errors='replace')))
        # Archive and clear every one-shot startup hook before ordinary launches.
        for name in ('child-probe.txt', 'service-preflight.txt', 'service-native.txt', 'test-run.txt', 'wine-run.txt'):
            old = download(base + name)
            if old is not None:
                (output / name).write_bytes(old)
                try:
                    ftp.delete(base + name)
                except error_reply as error:
                    if not str(error).startswith('226'):
                        raise
        if not idle():
            raise RuntimeError('Console became busy; no executable replacement performed')
        ftp.storbinary('STOR ' + base + 'eboot.bin', io.BytesIO(production), blocksize=1 << 20)
        readback = download(base + 'eboot.bin')
        record['production_restored'] = readback is not None and runner.executable_sha(readback) == expected
        record['restoration_pending'] = not record['production_restored']
        record['recovery_time_unix'] = time.time()
        record['recovery_capture'] = str(output.relative_to(ROOT))
        record['recovery_launched_title'] = False
        path.write_text(json.dumps(record, indent=2) + '\n')
        if record.get('helper_uploaded'):
            helper = download(base + 'service-native-helper.bin')
            if helper is not None:
                helper_path = 'helper/helper.elf' if 'helper_sha256' in record['build'] else 'build/eboot.elf'
                expected_helper = runner.executable_sha((ROOT / '.deps/child-probe' / record['build']['identity'][:16] / helper_path).read_bytes())
                if runner.executable_sha(helper) != expected_helper:
                    raise RuntimeError('Helper differs from our build; preserved for review')
                try:
                    ftp.delete(base + 'service-native-helper.bin')
                except error_reply as error:
                    if not str(error).startswith('226'):
                        raise
            record['helper_removed'] = True
        path.write_text(json.dumps(record, indent=2) + '\n')
        if not record['production_restored']:
            raise RuntimeError('Restored launcher executable verification failed')
    print('Signed production launcher restored and its executable segments verified. Startup hooks cleared. No title launched.')


if __name__ == '__main__':
    try:
        main()
    except BaseException as error:
        if isinstance(error, SystemExit) and isinstance(error.code, int):
            raise
        print('Probe recovery failed: ' + redact(error), file=sys.stderr)
        sys.exit(1)
