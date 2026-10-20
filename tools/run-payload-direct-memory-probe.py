#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run a bounded direct-memory payload alongside an already-running test owner.

The owner is supplied explicitly; this never launches or closes a title.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT.parent / 'PS5_Vulkan/tools'))
import ps5_console
sys.path.insert(0, str(ROOT / 'tools'))
import console_guard


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--owner', type=int, required=True)
    parser.add_argument('--name', required=True)
    parser.add_argument('--mib', type=int, choices=range(1, 2049))
    parser.add_argument('--wait-owner', action='store_true')
    parser.add_argument('--app', action='store_true')
    args = parser.parse_args()
    if not re.fullmatch(r'[a-zA-Z0-9_-]+', args.name) or args.wait_owner and args.mib:
        parser.error('use a plain run name; wait-owner tests only one page')
    # A drop or storage damage refuses; an unverified title does not (this runs before a deploy).
    message = console_guard.refusal(console_guard.load(), deploys=False)
    if message:
        raise SystemExit(message)
    evidence = ROOT / 'evidence' / (args.name + '.json')
    if evidence.exists():
        parser.error('use a new run name')
    settings = ps5_console.load_settings()
    state = ps5_console.ps5vkctl_command(settings, 'procs', timeout=5)
    pids = re.search(r'pids=([0-9, ]+)', state)
    if 'title=PPSA99170 ' not in state or not pids or str(args.owner) not in re.findall(r'\d+', pids.group(1)):
        raise SystemExit('The specified test owner is not running')
    elf = ROOT / 'build/payload-direct-memory-probe.elf'
    subprocess.run([str(ROOT / '.deps/native/ps5-payload-sdk/bin/prospero-clang'), '-std=c11', '-O2',
                    '-Wall', '-Wextra', '-Werror', '-o', str(elf),
                    str(ROOT / 'tools/payload-direct-memory-probe.c'), '-lkernel_sys'], check=True)
    words = ['direct-memory-probe', 'owner=' + str(args.owner)]
    if args.mib:
        words.append('mib=' + str(args.mib))
    if args.wait_owner:
        words.append('wait-owner')
    if args.app:
        words.append('app')
    boundary = 'DirectMemoryProbe'
    body = b''
    for name, data in (('args', ' '.join(words).encode()), ('pipe', b'1'), ('elf', elf.read_bytes())):
        header = f'--{boundary}\r\nContent-Disposition: form-data; name="{name}"'
        if name == 'elf':
            header += '; filename="direct-memory-probe.elf"'
        body += (header + '\r\n\r\n').encode() + data + b'\r\n'
    body += f'--{boundary}--\r\n'.encode()
    request = urllib.request.Request(f'http://{settings["host"]}:8080/elfldr', data=body,
                                     headers={'Content-Type': 'multipart/form-data; boundary=' + boundary})
    try:
        with urllib.request.urlopen(request, timeout=180 if args.wait_owner else 30) as response:
            text = response.read().decode(errors='replace')
    except OSError as error:
        raise SystemExit(str(error).replace(settings['host'], '<console>')) from None
    text = re.sub(r'\b(?:\d{1,3}\.){3}\d{1,3}\b', '<address>', text)
    (ROOT / 'klog' / (args.name + '.log')).write_text(text)
    passed = 'PS5_PROTON_PAYLOAD_DMEM_PROBE PASS' in text
    evidence.write_text(json.dumps({'run': args.name, 'owner': args.owner, 'mib': args.mib,
        'wait_owner': args.wait_owner, 'app': args.app, 'elf_sha256': hashlib.sha256(elf.read_bytes()).hexdigest(),
        'passed': passed, 'lines': text.splitlines()}, indent=1) + '\n')
    print(text, end='')
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
