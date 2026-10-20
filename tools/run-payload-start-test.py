#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The title starts a payload-loader process itself, then hands it descriptors.

  tools/run-payload-start-test.py --name NAME [--build ISOLATED_TITLE_BUILD]

tools/helperhostprobe.c's payload is put at /data/ps5proton/run/helperhostprobe.elf; the isolated native title
(tools/build-child-probe.py --kind service-native) runs its payload-start mode through tools/run-child-probe.py, which
deploys it, launches it, keeps its results and restores production. The title posts the payload to the console's own
payload launcher (websrv on 127.0.0.1:8080) with the channel step, then does the channel exchange with the process
that starts (native/proton_channel_probe.cpp). The payload file is removed afterwards. evidence/NAME.json keeps the
title's lines and the payload's from the console log; the title run's own record is evidence/NAME-title.json.
"""
import argparse
import io
import json
import re
import subprocess
import sys
from ftplib import FTP, error_perm
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT.parent / 'PS5_Vulkan/tools'))
sys.path.insert(0, str(ROOT / 'tools'))
import ps5_console  # noqa: E402
import console_guard  # noqa: E402

TARGET = '/data/ps5proton/run/helperhostprobe.elf'


def connect(settings):
    ftp = FTP()
    ftp.connect(settings['host'], settings['ftp_port'], timeout=30)
    ftp.login(settings['ftp_user'], settings['ftp_password'] or 'x')
    return ftp


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--name', required=True)
    parser.add_argument('--build', type=Path, help='an isolated service-native title build (built when omitted)')
    args = parser.parse_args()
    if not re.fullmatch(r'[a-zA-Z0-9_-]+', args.name):
        parser.error('use a plain run name')
    evidence = ROOT / 'evidence' / (args.name + '.json')
    if evidence.exists():
        parser.error('use a new run name')
    if not args.build:
        built = subprocess.run([sys.executable, str(ROOT / 'tools/build-child-probe.py'), '--kind', 'service-native'],
                               check=True, capture_output=True, text=True).stdout
        args.build = Path(re.search(r'Child probe build: (\S+)', built)[1])
    subprocess.run(['bash', str(ROOT / 'tools/build-helperhostprobe.sh')], check=True, stdout=subprocess.DEVNULL)
    elf = (ROOT / 'build/helperhostprobe.elf').read_bytes()
    settings = ps5_console.load_settings()
    with console_guard.writing('probe', settings), connect(settings) as ftp:
        try:
            ftp.mkd('/data/ps5proton/run')
        except error_perm:
            pass
        ftp.storbinary('STOR ' + TARGET, io.BytesIO(elf))
        check = io.BytesIO()
        ftp.retrbinary('RETR ' + TARGET, check.write)
        if check.getvalue() != elf:
            raise SystemExit('payload readback differs')
    record = {'run': args.name}
    try:
        title = subprocess.run([sys.executable, str(ROOT / 'tools/run-child-probe.py'), '--kind', 'service-native',
                                '--mode', 'payload-start', '--build', str(args.build), '--name', args.name + '-title'],
                               cwd=ROOT)
        record['title_runner_status'] = title.returncode
    finally:
        with connect(settings) as ftp:
            try:
                ftp.sendcmd('DELE ' + TARGET)
                record['payload_file_removed'] = True
            except error_perm:
                record['payload_file_removed'] = False
    title_record = ROOT / 'evidence' / (args.name + '-title.json')
    if title_record.exists():
        title_json = json.loads(title_record.read_text())
        record['title_lines'] = [l for l in title_json.get('observations', [])
                                 if l.startswith(('payload-start', 'channel'))]
        record['title_passed'] = title_json.get('passed')
        record['production_restored'] = title_json.get('production_restored')
    capture = ROOT / 'klog' / (args.name + '-title') / 'capture.log'
    if capture.exists():
        record['payload_lines'] = [l.strip() for l in capture.read_text(errors='replace').splitlines()
                                   if 'helperhostprobe' in l or 'channel: ' in l][:60]
    record['passed'] = bool(record.get('title_passed'))
    evidence.write_text((json.dumps(record, indent=2) + '\n').replace(settings['host'], '<console>'))
    print('\n'.join(record.get('title_lines', [])))
    print(json.dumps({k: record.get(k) for k in ('passed', 'production_restored', 'payload_file_removed')}))
    return 0 if record['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
