#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The channel test: can the title and a process the jailbreak's payload loader starts hand each other descriptors?

  tools/run-channel-test.py --name NAME [--build ISOLATED_TITLE_BUILD]

Both halves run at once. First tools/helperhostprobe.c goes to elfldr with its channel step (websrv's POST /elfldr,
pipe=1, so its lines stream back): it listens on a Unix-domain socket under /data. Once it says so, the isolated
native title (tools/build-child-probe.py --kind service-native) runs its channel mode through
tools/run-child-probe.py, which deploys it, launches it, keeps its results and restores production: the title connects,
sends a socket end and a shared page, and takes a socket end back (native/proton_channel_probe.cpp).
evidence/NAME.json keeps the payload's lines and the title run's verdict; the title run's own record is
evidence/NAME-title.json.
"""
import argparse
import json
import re
import subprocess
import sys
import threading
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT.parent / 'PS5_Vulkan/tools'))
import ps5_console  # noqa: E402


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
    if ' count=0 ' not in ps5_console.ps5vkctl_command(settings, 'procs', timeout=15) + ' ':
        raise SystemExit('the console is not idle: close the title first')
    lines, listening, finished = [], threading.Event(), threading.Event()

    def payload():
        boundary = 'ProtonChannelTest'
        body = b''
        for name, data in (('args', b'channel'), ('pipe', b'1'), ('elf', elf)):
            header = f'--{boundary}\r\nContent-Disposition: form-data; name="{name}"'
            if name == 'elf':
                header += '; filename="helperhostprobe.elf"'
            body += (header + '\r\n\r\n').encode() + data + b'\r\n'
        body += f'--{boundary}--\r\n'.encode()
        request = urllib.request.Request('http://' + settings['host'] + ':8080/elfldr', data=body,
                                         headers={'Content-Type': 'multipart/form-data; boundary=' + boundary})
        pending = b''
        try:
            with urllib.request.urlopen(request, timeout=240) as response:
                while chunk := response.read1(4096):
                    pending += chunk
                    *complete, pending = pending.split(b'\n')
                    for raw in complete:
                        text = raw.decode(errors='replace').rstrip('\r')
                        if text.strip():
                            lines.append(text)
                            print('payload: ' + text, flush=True)
                            if text.startswith('channel: listening'):
                                listening.set()
        except Exception as error:
            lines.append('transport: ' + type(error).__name__)
        if pending.strip():
            lines.append(pending.decode(errors='replace'))
        finished.set()
        listening.set()

    thread = threading.Thread(target=payload, daemon=True)
    thread.start()
    listening.wait(60)
    record = {'run': args.name, 'payload_listening': any(l.startswith('channel: listening') for l in lines)}
    title = None
    if record['payload_listening']:
        title = subprocess.run([sys.executable, str(ROOT / 'tools/run-child-probe.py'), '--kind', 'service-native',
                                '--mode', 'channel', '--build', str(args.build), '--name', args.name + '-title'], cwd=ROOT)
        record['title_runner_status'] = title.returncode
    finished.wait(200)
    record['payload_lines'] = lines
    record['payload_verdict'] = next((l.split('channel: ', 1)[1] for l in lines if 'payload verdict' in l), None)
    title_record = ROOT / 'evidence' / (args.name + '-title.json')
    if title_record.exists():
        title_json = json.loads(title_record.read_text())
        record['title_observations'] = [l for l in title_json.get('observations', []) if l.startswith('channel')]
        record['title_passed'] = title_json.get('passed')
    record['passed'] = bool(record.get('title_passed') and record['payload_verdict'] and
                            'answer=1 page=1 reply=1' in record['payload_verdict'])
    time.sleep(2)
    record['console_after'] = 'answers' if 'procs' in ps5_console.ps5vkctl_command(settings, 'procs', timeout=15) \
        else 'odd reply'
    evidence.write_text((json.dumps(record, indent=2) + '\n').replace(settings['host'], '<console>'))
    print(json.dumps({k: record.get(k) for k in ('passed', 'payload_verdict', 'title_passed', 'console_after')}))
    return 0 if record['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
