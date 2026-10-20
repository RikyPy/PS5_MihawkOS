#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Can a payload helper give back its carrier's 0x400000 image? (tools/carrier-image-probe.c)

Step 1, read only:   tools/run-carrier-image-probe.py --name carrier-image-2026-10-xx-01 --modes list
Step 2, after step 1 was read:
                     tools/run-carrier-image-probe.py --name carrier-image-2026-10-xx-02 --modes list,release

Builds the probe with the payload SDK (the execute grant wrapped and refused, its disassembly checked, as
tools/build-payload-loader.sh does) and sends it to websrv/elfldr once per mode, each its own process, with pipe=1
so its lines come back. Refuses unless the console is idle and its guard is clear; launches no title. The klog is
read during each mode: a fatal signal fails the mode, and a maxpipekva, TTY(STDIN) leak or storage-damage line
stops the run before the next mode. Writes klog/<name>-<mode>.log and evidence/<name>.json (addresses masked).

Verdict: list passes when it ends and names the module at 0x400000. release passes when every segment was given
back, each after-check (thread, signal, module load) said what the list control said in the same run, nothing lay
below 4 GiB after the load, 0x400000 was then taken and read back, and it ends.
"""
import argparse
import hashlib
import json
import os
import re
import socket
import subprocess
import sys
import threading
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT.parent / 'PS5_Vulkan/tools'))
import ps5_console  # noqa: E402
sys.path.insert(0, str(ROOT / 'tools'))
import console_guard  # noqa: E402

SDK = Path(os.environ.get('PS5_PAYLOAD_SDK', ROOT / '.deps/native/ps5-payload-sdk'))
ELF = ROOT / 'build/carrier-image-probe.elf'
MODES = ('list', 'release')
# Why 32-bit programs at 0x400000 were stopped (tools/carrier-image-probe.c): the control first, the forget mode last.
XO = ['xo-control', 'xo-text', 'xo-rest', 'xo-forget']
# 32-bit code runs from a JIT page at 0x400000 after the release, the control at 0x590000 first (xo32 mode).
XO32 = ['xo32']
# mDBG: the kernel's debug-event stop (signo 0xa0020101) that left run -01's probe stopped in a system module's load.
FATAL = re.compile(r'A user thread receives a fatal signal|mDBG: Sending signal|App Crash', re.I)
STOP = re.compile(r'maxpipekva|LEAKED OPEN|TTY\(STDIN\)', re.I)
AFTER = ('CARRIER thread status=', 'CARRIER signal status=', 'CARRIER load handle_ok=')
IPV4 = re.compile(r'\b(?:\d{1,3}\.){3}\d{1,3}\b')


def mask(text, host):
    return IPV4.sub('<address>', text.replace(host, '<console>'))


def build():
    ELF.parent.mkdir(exist_ok=True)
    subprocess.run([str(SDK / 'bin/prospero-clang'), '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                    f'-I{ROOT / "src"}', f'-I{ROOT / "wine/ps5"}',
                    '-Wl,--wrap=kernel_mprotect', '-Wl,--wrap=kernel_set_vmem_protection', '-o', str(ELF),
                    str(ROOT / 'tools/carrier-image-probe.c'), str(ROOT / 'src/pw_vm_census.c'),
                    str(ROOT / 'wine/ps5/pw_wine_prx.c'), '-lkernel_sys'], check=True)
    listing = subprocess.run([str(SDK / 'bin/llvm-objdump') if (SDK / 'bin/llvm-objdump').exists() else 'llvm-objdump',
                              '-d', '--no-show-raw-insn', str(ELF)], check=True, capture_output=True, text=True).stdout
    if (re.search(r'(call|jmp)[a-z]*\s.*<kernel_(mprotect|set_vmem_protection)>', listing)
            or '<__wrap_kernel_mprotect>:' not in listing):
        ELF.unlink()
        raise SystemExit('the probe can still call the SDK kernel_mprotect; removed')


class Klog(threading.Thread):
    """Every klog line from when the replayed backlog falls quiet until stop is set."""

    def __init__(self, settings):
        super().__init__(daemon=True)
        self.settings, self.lines, self.error = settings, [], None
        self.armed, self.stop = threading.Event(), threading.Event()

    def run(self):
        try:
            with socket.create_connection((self.settings['host'], self.settings['klog_port']), timeout=10) as c:
                c.settimeout(1.0)
                drained = time.monotonic() + 10
                try:
                    while time.monotonic() < drained and c.recv(65536):   # the backlog; one quiet second ends it
                        pass
                except socket.timeout:
                    pass
                self.armed.set()
                pending = b''
                while not self.stop.is_set():
                    try:
                        chunk = c.recv(65536)
                    except socket.timeout:
                        continue
                    if not chunk:
                        break
                    *done, pending = (pending + chunk).split(b'\n')
                    self.lines.extend(line.decode(errors='replace') for line in done)
        except OSError as error:
            self.error = str(error)
        self.armed.set()


def send(settings, mode, extra=''):
    boundary = 'CarrierImageProbe'
    body = b''
    for name, data in (('args', f'carrier mode={mode}{extra}'.encode()), ('pipe', b'1'), ('elf', ELF.read_bytes())):
        header = f'--{boundary}\r\nContent-Disposition: form-data; name="{name}"'
        if name == 'elf':
            header += '; filename="carrier-image-probe.elf"'
        body += (header + '\r\n\r\n').encode() + data + b'\r\n'
    body += f'--{boundary}--\r\n'.encode()
    request = urllib.request.Request(f'http://{settings["host"]}:8080/elfldr', data=body,
                                     headers={'Content-Type': 'multipart/form-data; boundary=' + boundary})
    chunks = []
    try:   # read as it arrives: a mode that ends the process keeps what it printed
        with urllib.request.urlopen(request, timeout=60) as response:
            while chunk := response.read1(4096):
                chunks.append(chunk)
        return b''.join(chunks).decode(errors='replace')
    except OSError as error:
        return b''.join(chunks).decode(errors='replace') + '\ncarrier runner: ' + str(error)


def verdict(mode, lines, fatal, control):
    ended = f'CARRIER end mode={mode}' in lines
    if mode == 'xo32':
        # A stop is an answer: the last line says which run did not return. Both must return 42.
        runs = [line for line in lines if line.startswith('CARRIER xo32 ') and ' returned ' in line]
        returned = {line.split()[2]: line for line in runs}
        both = all(key in returned and 'cs=0x33 ' in returned[key] and 'ss=0x3b ' in returned[key]
                   and returned[key].endswith('value=42 want=42') for key in ('control', 'low'))
        return {'passed': ended and not fatal and both, 'ended': ended, 'stopped': bool(fatal) and not ended,
                'last_step': lines[-1] if lines else None, 'returned': runs}
    if mode.startswith('xo-'):
        # The answer is where each process got to: every step line, the last one, and a stop the kernel logged.
        steps = [line for line in lines if line.startswith('CARRIER xo ')]
        faults = [line for line in steps if ' fault ' in line]
        return {'passed': ended and not fatal and (mode != 'xo-control' or not faults), 'ended': ended,
                'stopped': bool(fatal) and not ended, 'last_step': steps[-1] if steps else None, 'faults': faults}
    found = any(re.fullmatch(r'CARRIER carrier before module=\d+', line) for line in lines)
    after = [line for line in lines if line.startswith(AFTER)]
    if mode == 'list':
        return {'passed': ended and found and not fatal, 'ended': ended, 'carrier_found': found, 'after': after}
    unmapped = [line for line in lines if line.startswith('CARRIER unmapped ')]
    taken = 'CARRIER released ok=1' in lines
    same = control is not None and after == control
    low_after_load = [line for line in lines if line.startswith('CARRIER map after-load low ')]
    return {'passed': ended and found and taken and same and not fatal and not low_after_load
                      and all('status=0 ' in l for l in unmapped),
            'ended': ended, 'carrier_found': found, 'released': taken, 'unmapped': unmapped,
            'after': after, 'after_matches_list': same, 'low_after_load': low_after_load}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--name', required=True)
    parser.add_argument('--modes', default='list',
                        help='list, list,release (the control runs first), xo, or xo32 (32-bit code at 0x400000)')
    parser.add_argument('--stop', type=int, help='end a probe left running, by pid (only a process named carrier)')
    args = parser.parse_args()
    if args.stop:
        build()
        settings = ps5_console.load_settings()
        print(mask(send(settings, 'stop', f' pid={args.stop}'), settings['host']))
        return 0
    modes = XO if args.modes == 'xo' else XO32 if args.modes == 'xo32' else args.modes.split(',')
    if not re.fullmatch(r'[a-zA-Z0-9_-]+', args.name) or modes not in (['list'], ['list', 'release'], XO, XO32):
        parser.error('a plain run name, and --modes list, list,release, xo or xo32')
    evidence = ROOT / 'evidence' / (args.name + '.json')
    if evidence.exists():
        parser.error('use a new run name')
    # A drop or storage damage refuses; an unverified title does not (no title runs here).
    message = console_guard.refusal(console_guard.load(), deploys=False)
    if message:
        raise SystemExit(message)
    settings = ps5_console.load_settings()
    if ' count=0 ' not in ps5_console.ps5vkctl_command(settings, 'procs', timeout=5) + ' ':
        raise SystemExit('The console is not idle; nothing was sent')
    build()
    results, control, stopped = {}, None, None
    for mode in modes:
        klog = Klog(settings)
        klog.start()
        klog.armed.wait(timeout=20)
        text = mask(send(settings, mode), settings['host'])
        time.sleep(3)                         # the kernel's lines about the process's exit
        klog.stop.set()
        klog.join(timeout=5)
        klog_lines = [mask(line, settings['host']) for line in klog.lines]
        (ROOT / 'klog' / f'{args.name}-{mode}.log').write_text(f'# probe\n{text}\n\n# klog during the mode\n'
                                                              + '\n'.join(klog_lines) + '\n')
        lines = [line.strip() for line in text.splitlines() if line.startswith('CARRIER ')]
        fatal = [line for line in klog_lines if FATAL.search(line)]
        record = verdict(mode, lines, fatal, control)
        record.update({'lines': lines, 'klog_fatal': fatal[:20], 'klog_error': klog.error})
        results[mode] = record
        if mode == 'list':
            control = record['after']
        print(f'== {mode}: ' + ('PASS' if record['passed'] else 'FAIL'))
        for line in lines:
            print('  ' + line)
        damage = console_guard.storage_damage(klog_lines)
        if damage:
            console_guard.record_storage(args.name, damage)
        halt = [line for line in klog_lines if STOP.search(line)] + damage
        if halt:
            stopped = halt[:5]
            print('stopped: ' + halt[0][:200])
            break
        if not record['passed'] and not mode.startswith('xo-') and mode != 'xo32':
            break   # an xo mode's stop is its answer: the next mode is its own process
    evidence.write_text(json.dumps({
        'run': args.name, 'elf_sha256': hashlib.sha256(ELF.read_bytes()).hexdigest(),
        'source_sha256': hashlib.sha256((ROOT / 'tools/carrier-image-probe.c').read_bytes()).hexdigest(),
        'process': 'payload through websrv/elfldr in the NPXS40112 carrier, no title running',
        'passed': len(results) == len(modes) and all(r['passed'] for r in results.values()),
        'stopped_by': stopped, 'modes': results}, indent=1) + '\n')
    return 0 if all(r['passed'] for r in results.values()) and len(results) == len(modes) else 1


if __name__ == '__main__':
    raise SystemExit(main())
