#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Two payloads, one direct-memory range: can the second map what the first allocated (tools/dmem-share-probe.c)?

With --exec, first one payload asks the kernel's own mprotect for execute on each memory type a helper uses (the
application's direct memory, flexible memory, a shared file on /data, a staged PRX's text), then the second side of the
pair asks it on the range the first granted it: the answers that choose the executable-grant design before a full build.

Needs the application's title running (the pool is the application's). Bounded: MIB at most 512, 70 s (with --exec, a
further 60 s). Writes evidence/<name>.json and klog/<name>.log."""
import argparse, hashlib, json, re, subprocess, sys, threading, time, urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT.parent / 'PS5_Vulkan/tools'))
import ps5_console
sys.path.insert(0, str(ROOT / 'tools'))
import console_guard


def send(settings, elf, words, results, key):
    boundary = 'DmemShare'
    body = b''
    for name, data in (('args', ' '.join(words).encode()), ('pipe', b'1'), ('elf', elf.read_bytes())):
        header = f'--{boundary}\r\nContent-Disposition: form-data; name="{name}"'
        if name == 'elf':
            header += '; filename="dmem-share.elf"'
        body += (header + '\r\n\r\n').encode() + data + b'\r\n'
    body += f'--{boundary}--\r\n'.encode()
    request = urllib.request.Request(f'http://{settings["host"]}:8080/elfldr', data=body,
                                     headers={'Content-Type': 'multipart/form-data; boundary=' + boundary})
    try:
        with urllib.request.urlopen(request, timeout=90) as response:
            results[key] = response.read().decode(errors='replace')
    except OSError as error:
        results[key] = 'ERROR ' + str(error).replace(settings['host'], '<console>')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--name', required=True)
    parser.add_argument('--mib', type=int, default=64, choices=range(1, 513))
    parser.add_argument('--title', action='store_true', help='the second process is the title itself (PROTON_DMEM_TITLE_TEST=1 on its launch)')
    parser.add_argument('--exec', action='store_true', help='measure execute grants through the kernel per memory type')
    parser.add_argument('--exec2', action='store_true', help='measure the routes left when the kernel drops execute: JIT '
                        'shared memory, and the title\'s credentials for one grant (one payload; no pair)')
    parser.add_argument('--exec3', action='store_true', help='JIT shared memory limits: in-place execute, RWX, pool, count')
    parser.add_argument('--exec4', action='store_true', help='the kernel\'s own answers only (K1-K9), built with the SDK\'s '
                        'kernel write wrapped to refuse; refused to send if the disassembly still calls it')
    parser.add_argument('--prx', default='/data/ps5proton/runtime/ge11-7/lib/wine/x86_64-unix/libfreetype.prx',
                        help='a staged PRX for --exec (libfreetype needs only libc)')
    args = parser.parse_args()
    if not re.fullmatch(r'[a-zA-Z0-9_-]+', args.name):
        parser.error('use a plain run name')
    if args.exec and args.title:
        parser.error('--exec needs the second payload, not the title')
    if not re.fullmatch(r'/data/[A-Za-z0-9_./-]+\.prx', args.prx) or '..' in args.prx:
        parser.error('--prx is a .prx path under /data')
    # A drop or storage damage refuses; an unverified title does not (this runs before a deploy).
    message = console_guard.refusal(console_guard.load(), deploys=False)
    if message:
        raise SystemExit(message)
    evidence = ROOT / 'evidence' / (args.name + '.json')
    if evidence.exists():
        parser.error('use a new run name')
    settings = ps5_console.load_settings()
    state = ps5_console.ps5vkctl_command(settings, 'procs', timeout=5)
    if 'title=PPSA99170 ' not in state:
        raise SystemExit('The application title is not running')
    elf = ROOT / 'build/dmem-share-probe.elf'
    # The SDK's libc mmap and mprotect with PROT_EXEC write the kernel's map entry; no mode asks them for execute, the
    # wrap turns any write left into a refusal counted in the DONE line, and the disassembly check proves no call to it.
    wrap = ['-Wl,--wrap=kernel_mprotect', '-Wl,--wrap=kernel_set_vmem_protection']
    subprocess.run([str(ROOT / '.deps/native/ps5-payload-sdk/bin/prospero-clang'), '-std=gnu11', '-O2', '-Wall', '-Wextra',
                    '-Werror', *wrap, '-o', str(elf), str(ROOT / 'tools/dmem-share-probe.c'),
                    str(ROOT / 'wine/ps5/pw_wine_prx.c'), '-lkernel_sys'], check=True)
    listing = subprocess.run(['llvm-objdump', '-d', '--no-show-raw-insn', str(elf)], check=True, capture_output=True,
                             text=True).stdout
    if re.search(r'call.*<kernel_(mprotect|set_vmem_protection)>', listing) or '__wrap_kernel_mprotect' not in listing:
        raise SystemExit('the probe can still reach the SDK kernel write; not sent')
    results = {}
    keys = []
    if args.exec2 or args.exec3 or args.exec4:
        pid = re.search(r'pids=(\d+)', state)
        if not pid:
            raise SystemExit('no title pid in: ' + state)
        words = (['dmem-share', 'exec2', pid.group(1)] if args.exec2 else ['dmem-share', 'exec4', args.prx] if args.exec4
                 else ['dmem-share', 'exec3'])
        alone = threading.Thread(target=send, args=(settings, elf, words, results, 'exec2'))
        alone.start()
        alone.join(100)
        text = '--- exec2\n' + results.get('exec2', 'no answer') + '\n'
        (ROOT / 'klog' / (args.name + '.log')).write_text(text)
        evidence.write_text(json.dumps({'run': args.name, 'elf_sha256': hashlib.sha256(elf.read_bytes()).hexdigest(),
                                        'passed': 'DONE ' in text, 'lines': text.splitlines()}, indent=1) + '\n')
        print(text, end='')
        return 0 if 'DONE ' in text else 1
    if args.exec:
        alone = threading.Thread(target=send, args=(settings, elf, ['dmem-share', 'exec', args.prx], results, 'exec'))
        alone.start()
        alone.join(100)
        keys.append('exec')
    words = ['dmem-share', 'hold', str(args.mib)] + (['keep'] if args.title else [])
    first = threading.Thread(target=send, args=(settings, elf, words, results, 'hold'))
    first.start()
    keys.append('hold')
    if not args.title:
        time.sleep(4)
        second = threading.Thread(target=send, args=(settings, elf, ['dmem-share', 'map'] + (['exec'] if args.exec else []),
                                                     results, 'map'))
        second.start()
        second.join(100)
        keys.append('map')
    first.join(100)
    text = ''
    for key in keys:
        text += f'--- {key}\n' + re.sub(r'\b(?:\d{1,3}\.){3}\d{1,3}\b', '<address>', results.get(key, 'no answer')) + '\n'
    (ROOT / 'klog' / (args.name + '.log')).write_text(text)
    # Each type's answer: its grant line's fields, then whether its code ran ('used').
    grants = {}
    for line in text.splitlines():
        fields = dict(re.findall(r'(\w+)=(\S+)', line)) if line.startswith('PS5_PROTON_EXEC_GRANT type=') else {}
        if fields:
            grants.setdefault(fields.pop('type'), {}).update(fields)
    passed = text.count('PS5_PROTON_DMEM_SHARE PASS') == len([key for key in keys if key != 'exec'])
    if args.exec:
        passed = passed and 'PS5_PROTON_EXEC_GRANT DONE' in text and 'granted-rx' in grants
    evidence.write_text(json.dumps({'run': args.name, 'mib': args.mib, 'elf_sha256': hashlib.sha256(elf.read_bytes()).hexdigest(),
                                    'passed': passed, 'exec_grants': grants if args.exec else None,
                                    'lines': text.splitlines()}, indent=1) + '\n')
    print(text, end='')
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
