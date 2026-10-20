#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Every payload ELF the title ships (the Lapy daemon, the Wine helper loader) must accept the same set of console
firmwares in its crt `__kernel_init`, and that set must include 10.01 (the proven Pro) and 13.40 (the base PS5 that
showed "No access to /data" when the daemon's v0.40 crt faulted with SIGILL in `_start`). A payload whose crt rejects
the running firmware dies before `main`, so a mismatch here is a console that silently never gets /data.

The firmware gate in `__kernel_init` is a chain of `cmpl $0x<MM><mm>0000, %reg` / `je|jne <handler>` against the
kernel's software version word (0x13400000 is 13.40: each byte's hex digits read as the decimal version). This reads
that set straight out of the linked crt, so no console and no run is needed.

    tools/check-payload-firmware.py --check <elf>...   # all must share one set incl. 10.01 and 13.40; else exit 1
    tools/check-payload-firmware.py <elf>              # print one payload's set
"""
import argparse
import re
import shutil
import subprocess
import sys

REQUIRED = ('10.01', '13.40')
_CMP = re.compile(r'cmpl\s+\$0x([0-9a-f]+),')
_JEQ = re.compile(r'j(?:e|ne)\b')


def _decode(imm):
    """0x13400000 -> '13.40'; low 16 bits must be zero (a firmware word, not some other constant)."""
    if imm & 0xffff:
        return None
    return '%x.%02x' % (imm >> 24, (imm >> 16) & 0xff)


def parse_firmware(disassembly):
    """The firmware versions compared for exact equality in a `__kernel_init` disassembly."""
    lines = [line.split('\t', 1)[1].strip() for line in disassembly.splitlines() if '\t' in line]
    found = set()
    for cmp_line, next_line in zip(lines, lines[1:]):
        m = _CMP.match(cmp_line)
        if m and _JEQ.match(next_line):
            version = _decode(int(m.group(1), 16))
            if version:
                found.add(version)
    return frozenset(found)


def _objdump():
    for name in ('llvm-objdump', 'llvm-objdump-18', 'objdump'):
        path = shutil.which(name)
        if path:
            return path
    sys.exit('check-payload-firmware: no llvm-objdump/objdump on PATH')


def firmware_set(elf, objdump=None):
    out = subprocess.run([objdump or _objdump(), '-d', '--no-show-raw-insn',
                          '--disassemble-symbols=__kernel_init', str(elf)],
                         capture_output=True, text=True, check=True).stdout
    versions = parse_firmware(out)
    if not versions:
        sys.exit('check-payload-firmware: no __kernel_init firmware gate in ' + str(elf))
    return versions


def _ordered(versions):
    return ' '.join(sorted(versions, key=lambda v: tuple(int(p) for p in v.split('.'))))


def verdict(sets):
    """The reason {name: firmware_set} fails the rule, or None when it passes. Pure, so the host test drives it."""
    reference = next(iter(sets.values()))
    for name, versions in sets.items():
        missing = [v for v in REQUIRED if v not in versions]
        if missing:
            return '%s rejects firmware %s (crt accepts %d: %s)' % (name, ', '.join(missing), len(versions), _ordered(versions))
        if versions != reference:
            return 'payload firmware sets differ (e.g. %s is not in every payload); rebuild them from one SDK pin' % _ordered(versions ^ reference)
    return None


def check(elfs, objdump=None):
    """Exit non-zero (with the reason) unless every payload shares one set that includes REQUIRED."""
    sets = {str(elf): firmware_set(elf, objdump) for elf in elfs}
    reason = verdict(sets)
    if reason:
        sys.exit(reason)
    print('payload firmware ok: %d payloads share %d firmwares incl. %s' %
          (len(sets), len(next(iter(sets.values()))), ', '.join(REQUIRED)))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('elfs', nargs='+', metavar='elf')
    p.add_argument('--check', action='store_true', help='require every payload to share one set incl. %s' % ', '.join(REQUIRED))
    p.add_argument('--objdump')
    a = p.parse_args()
    if a.check:
        check(a.elfs, a.objdump)
    else:
        for elf in a.elfs:
            versions = firmware_set(elf, a.objdump)
            print(elf, len(versions), ' '.join(sorted(versions, key=lambda v: tuple(int(x) for x in v.split('.')))))


if __name__ == '__main__':
    main()
