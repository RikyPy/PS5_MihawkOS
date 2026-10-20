#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Send tools/helperhostprobe.c's payload to the console's payload loader with one step, and keep what it said.

  tools/run-helperhostprobe.py --name NAME [--step memory|dmem|ranges|module]

The payload runs as its own process, started by elfldr (websrv's POST /elfldr, pipe=1 streams its standard output
back). The console must be idle (no title running), so nothing else is measured with it. Afterwards the console is
asked for its process list, to record that it still answers. evidence/<name>.json keeps the lines.
"""
import argparse
import hashlib
import json
import re
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT.parent / 'PS5_Vulkan/tools'))
import ps5_console  # noqa: E402

ELF = ROOT / 'build/helperhostprobe.elf'


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--name', required=True)
    parser.add_argument('--step', choices=('memory', 'dmem', 'ranges', 'module'))
    args = parser.parse_args()
    if not re.fullmatch(r'[a-zA-Z0-9_-]+', args.name):
        parser.error('use a plain run name')
    evidence = ROOT / 'evidence' / (args.name + '.json')
    if evidence.exists():
        parser.error('use a new run name')
    subprocess.run(['bash', str(ROOT / 'tools/build-helperhostprobe.sh')], check=True, stdout=subprocess.DEVNULL)
    elf = ELF.read_bytes()
    settings = ps5_console.load_settings()
    if ' count=0 ' not in ps5_console.ps5vkctl_command(settings, 'procs', timeout=15) + ' ':
        raise SystemExit('the console is not idle: close the title first')
    record = {'run': args.name, 'step': args.step or 'baseline', 'elf_sha256': hashlib.sha256(elf).hexdigest(),
              'started': time.strftime('%Y-%m-%dT%H:%M:%S')}
    boundary = 'ProtonHelperHostProbe'
    body = b''
    for name, data in (('args', (args.step or '').encode()), ('pipe', b'1'), ('elf', elf)):
        header = f'--{boundary}\r\nContent-Disposition: form-data; name="{name}"'
        if name == 'elf':
            header += '; filename="helperhostprobe.elf"'
        body += (header + '\r\n\r\n').encode() + data + b'\r\n'
    body += f'--{boundary}--\r\n'.encode()
    request = urllib.request.Request('http://' + settings['host'] + ':8080/elfldr', data=body,
                                     headers={'Content-Type': 'multipart/form-data; boundary=' + boundary})
    output = b''
    try:
        with urllib.request.urlopen(request, timeout=120) as response:
            while chunk := response.read(4096):
                output += chunk
    except Exception as error:  # the lines up to a failure are the evidence
        record['transport'] = type(error).__name__
    record['lines'] = [line for line in output.decode(errors='replace').splitlines() if line.strip()]
    record['finished'] = any(line.startswith('helperhostprobe done') for line in record['lines'])
    time.sleep(2)
    try:
        record['console_after'] = 'answers' if 'procs' in ps5_console.ps5vkctl_command(
            settings, 'procs', timeout=15) else 'odd reply'
    except Exception as error:
        record['console_after'] = 'no answer: ' + type(error).__name__
    text = json.dumps(record, indent=2) + '\n'
    evidence.write_text(text.replace(settings['host'], '<console>'))
    print('\n'.join(record['lines']))
    print(f"finished={record['finished']} console_after={record['console_after']}")
    return 0 if record['finished'] else 1


if __name__ == '__main__':
    sys.exit(main())
