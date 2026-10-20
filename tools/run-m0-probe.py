#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""M0 of docs/IA32_DESIGN.md: native 32-bit code in a payload process, on the console.

Builds tools/m0-native32-probe.c with the payload SDK and sends it to websrv/elfldr once per mode (each mode its
own process, as every Windows program of ours is), with pipe=1 so its output comes back. Refuses unless the
console is idle and its drop guard is clear; launches no title. Writes klog/<name>-<mode>.log and
evidence/<name>.json (every line, per mode; addresses masked).

The 32-bit code in the probe is this, assembled with llvm-mc --triple=i386 (results at 0x11111100,
exits to 0x22222200, both patched at run time) and checked with tools/ia32-dump.sh:
  compute:  mov ecx,cs; mov [R],ecx; mov eax,ss; mov [R+4],eax; mov eax,6; push eax; call f; add esp,4;
            mov [R+8],eax; ljmp 0x43:B          f: mov eax,[esp+4]; imul eax,eax,7; ret
  fault:    mov eax,[0]; ljmp 0x43:B
  fsread:   mov eax,0x13; mov fs,eax; mov eax,fs:[0]; mov [R+12],eax; ljmp 0x43:B
  fault_fs: mov eax,fs:[0x18]; mov [R+16],eax; mov eax,[0]; ljmp 0x43:B

  tools/run-m0-probe.py --name m0-2026-10-07-01 [--modes ident,low4g,switch,fs,signal32,fsselector,signal32fs]
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
import ps5_console  # noqa: E402
sys.path.insert(0, str(ROOT / 'tools'))
import console_guard  # noqa: E402

MODES = ['ident', 'low4g', 'switch', 'fs', 'signal32', 'fsselector', 'signal32fs', 'fsguest', 'sigpath', 'signal32fsraw', 'ldt', 'ldtsignal', 'rawsig', 'fsreload', 'fsalias', 'tcbs']


def send(settings, elf, mode, extra=''):
    boundary = 'M0Probe'
    body = b''
    for name, data in (('args', ('m0 mode=' + mode + extra).encode()), ('pipe', b'1'), ('elf', elf.read_bytes())):
        header = f'--{boundary}\r\nContent-Disposition: form-data; name="{name}"'
        if name == 'elf':
            header += '; filename="m0-native32-probe.elf"'
        body += (header + '\r\n\r\n').encode() + data + b'\r\n'
    body += f'--{boundary}--\r\n'.encode()
    request = urllib.request.Request(f'http://{settings["host"]}:8080/elfldr', data=body,
                                     headers={'Content-Type': 'multipart/form-data; boundary=' + boundary})
    chunks = []
    try:   # read as it arrives: a mode that hangs keeps what it printed before the timeout
        with urllib.request.urlopen(request, timeout=60) as response:
            while True:
                chunk = response.read1(4096)
                if not chunk:
                    break
                chunks.append(chunk)
        text = b''.join(chunks).decode(errors='replace')
    except OSError as error:
        text = b''.join(chunks).decode(errors='replace') + '\nM0 runner: ' + str(error).replace(settings['host'], '<console>')
    return re.sub(r'\b(?:\d{1,3}\.){3}\d{1,3}\b', '<address>', text)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--name', required=True)
    parser.add_argument('--modes', default=','.join(MODES))
    parser.add_argument('--stop', type=int, help='end an M0 probe left running, by pid (only a process named m0)')
    args = parser.parse_args()
    modes = args.modes.split(',')
    if not re.fullmatch(r'[a-zA-Z0-9_-]+', args.name) or any(m not in MODES for m in modes):
        parser.error('a plain run name and known modes')
    evidence = ROOT / 'evidence' / (args.name + '.json')
    if evidence.exists() and not args.stop:
        parser.error('use a new run name')
    # A drop or storage damage refuses; an unverified title does not (this runs before a deploy).
    message = console_guard.refusal(console_guard.load(), deploys=False)
    if message:
        raise SystemExit(message)
    settings = ps5_console.load_settings()
    state = ps5_console.ps5vkctl_command(settings, 'procs', timeout=5)
    if ' count=0 ' not in state + ' ' and not args.stop:
        raise SystemExit('The console is not idle; nothing was sent')
    elf = ROOT / 'build/m0-native32-probe.elf'
    if args.stop:
        subprocess.run([str(ROOT / '.deps/native/ps5-payload-sdk/bin/prospero-clang'), '-std=gnu11', '-O2', '-Wall',
                        '-Wextra', '-Werror', '-o', str(elf), str(ROOT / 'tools/m0-native32-probe.c'), '-lkernel_sys'],
                       check=True)
        print(send(settings, elf, 'stop', f' pid={args.stop}'))
        return 0
    elf.parent.mkdir(exist_ok=True)
    subprocess.run([str(ROOT / '.deps/native/ps5-payload-sdk/bin/prospero-clang'), '-std=gnu11', '-O2', '-Wall',
                    '-Wextra', '-Werror', '-o', str(elf), str(ROOT / 'tools/m0-native32-probe.c'), '-lkernel_sys'],
                   check=True)
    results = {}
    for mode in modes:
        text = send(settings, elf, mode)
        (ROOT / 'klog' / f'{args.name}-{mode}.log').write_text(text)
        lines = [l for l in text.splitlines() if l.startswith('M0 ')]
        results[mode] = {'lines': lines, 'ended': any(l == 'M0 end' or l.startswith('M0 ident') or
                                                      l.startswith('M0 low4g') for l in lines)}
        last = {'sigpath': 'M0 sigpath', 'fsreload': 'M0 fsreload amd64_after', 'fsalias': 'M0 fsalias reload=',
                'tcbs': 'M0 tcbs joined'}
        if mode in last:
            results[mode]['ended'] = any(l.startswith(last[mode]) for l in lines)
        print(f'== {mode}: ' + ('ended' if results[mode]['ended'] else 'DID NOT END'))
        for l in lines:
            print('  ' + l)
    evidence.write_text(json.dumps({'run': args.name, 'elf_sha256': hashlib.sha256(elf.read_bytes()).hexdigest(),
                                    'process': 'payload through websrv/elfldr, no title running',
                                    'modes': results}, indent=1) + '\n')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
