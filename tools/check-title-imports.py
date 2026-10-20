#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Refuse a title whose import the console resolves to nothing: calling it jumps to address 0.

  tools/check-title-imports.py ELF --stub-dir SDK/target/lib [--stub FILE...] [--nm LLVM_NM]

ELF is the converted title (ps5-native-tool link's output: eboot.elf, or a probe title's). Its import table says what
the console's loader reads: each import's NID and the library it comes from. ps5/tools/link-title.sh runs this on every
title it links, before signing, so a probe title is checked as the production one is.

Why. On 2026-10-19 the first-install rehearsal's "Check drive" ended the title twice at rip=0: statfs, called by
ps5/src/proton_install.cpp, was bound to libkernel_sys, the system processes' libkernel, which a title does not load.
statvfs (phase 0) did the same, as strcasestr did in another title (PS5_Vulkan tools/radv-link.sh). The link accepts
all of them, because some stub of the payload SDK declares each name.

The rule, from what the console was measured and read to do (names and modules only; no firmware is in any repository):
  - the library must be one a title process loads. libkernel_sys (statfs, fstatfs, umask, openat and its family,
    readlink, link, symlink), libScePosixForWebKit (strcasestr, mkstemp, isatty, arc4random, gethostbyname, getnameinfo,
    gai_strerror, fnmatch), libSceGLSlimVSH, libSceFsInternalForVsh, libSceSysCore and libSceRegMgr are not: their
    imports resolve to nothing (the SDK's ps5platform/libc.h; PS5_Vulkan tools/radv-link.sh; the owner's static
    reading of the 10.01 modules, names only, docs/PROVENANCE.md "Local firmware reference (static reading)");
  - a few names are not exported, or are exported and fault: statvfs and fstatvfs (the console's libc builds them on
    statfs) and libkernel's sceKernelGetPrefixVersion and sceKernelIccGetThermalAlert (in no libkernel variant).
Every other library a title links is the console's: libSceLibcInternal exports all 3,015 names of the SDK's stub, and
libkernel and the Sce libraries all but system-only functions the reading lists (cursor, crash-report, settings and
device-control calls). Those are not named one by one there, so an import of one of them passes: shortcut: add them
when a title first needs to refuse one. What a function does once resolved (opendir and realpath are refused to a title
with an error, not a fault) is the platform layer's concern, ps5platform/libc.h.

Exit 0 when every import passes, 1 naming the ones that do not. tests/test_check_title_imports.py checks the rule on a
small converted ELF it writes itself. Read-only.
"""
import argparse
import base64
import hashlib
import struct
import subprocess
import sys
from pathlib import Path

NEVER_LOADED = {'libkernel_sys', 'libScePosixForWebKit', 'libSceGLSlimVSH', 'libSceFsInternalForVsh', 'libSceSysCore',
                'libSceRegMgr'}
UNRESOLVED_NAMES = {'statvfs', 'fstatvfs', 'sceKernelGetPrefixVersion', 'sceKernelIccGetThermalAlert'}
# tooling/native/libc_builder.cpp's kNidSuffix: the NID is sha1(name + suffix), 8 bytes reversed, base64, '/' as '-'.
NID_SUFFIX = bytes([0x51, 0x8D, 0x64, 0xA6, 0x35, 0xDE, 0xD8, 0xC1, 0xE6, 0xB0, 0x39, 0xB1, 0xC3, 0xE5, 0x52, 0x30])
ALPHABET = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-'
SCE_IMPORT_LIB, SCE_SYMTAB_SIZE, DT_STRTAB, DT_SYMTAB, DT_STRSZ = 0x61000049, 0x6100003f, 5, 6, 10


def nid(name):
    digest = hashlib.sha1(name.encode() + NID_SUFFIX).digest()
    return base64.b64encode(bytes(reversed(digest[:8]))).decode()[:11].replace('/', '-')


def imports(path):
    """[(NID, library)] of a converted title: its dynamic table's import libraries and its undefined symbols."""
    data = Path(path).read_bytes()
    if data[:6] != b'\x7fELF\x02\x01':
        raise SystemExit(f'{path}: not a little-endian ELF64')
    phoff = struct.unpack_from('<Q', data, 32)[0]
    phentsize, phnum = struct.unpack_from('<HH', data, 54)
    segments = [struct.unpack_from('<IIQQQQQQ', data, phoff + i * phentsize) for i in range(phnum)]

    def read(address, size):   # a virtual address through the loadable segments
        for kind, _, offset, vaddr, _, filesz, _, _ in segments:
            if kind == 1 and vaddr <= address and address + size <= vaddr + filesz:
                return data[offset + address - vaddr:offset + address - vaddr + size]
        raise SystemExit(f'{path}: {address:#x} is in no loadable segment')

    dynamic = next((s for s in segments if s[0] == 2), None)
    if dynamic is None:
        raise SystemExit(f'{path}: no dynamic table')
    entries = [struct.unpack_from('<qQ', data, i) for i in range(dynamic[2], dynamic[2] + dynamic[5], 16)]
    tags = {}
    for tag, value in entries:
        tags.setdefault(tag, value)
    strings = read(tags[DT_STRTAB], tags[DT_STRSZ])
    text = lambda offset: strings[offset:strings.index(b'\0', offset)].decode()
    libraries = {value >> 48: text(value & 0xffffffff) for tag, value in entries if tag == SCE_IMPORT_LIB}
    found = []
    for i in range(0, tags[SCE_SYMTAB_SIZE], 24):
        name, _, _, section, _, _ = struct.unpack('<IBBHQQ', read(tags[DT_SYMTAB] + i, 24))
        parts = text(name).split('#')
        if len(parts) == 3 and section == 0:
            token = sum(ALPHABET.index(c) * 64 ** k for k, c in enumerate(reversed(parts[1])))
            found.append((parts[0], libraries.get(token, '?')))
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('elf', type=Path, help='the converted title (ps5-native-tool link --out)')
    parser.add_argument('--stub-dir', type=Path, required=True, help="the SDK's target/lib, for the names")
    parser.add_argument('--stub', type=Path, action='append', default=[], help="the title's own link stubs")
    parser.add_argument('--nm', default='llvm-nm')
    args = parser.parse_args()
    names = {nid(name): name for name in UNRESOLVED_NAMES}
    for library in sorted(args.stub_dir.glob('*.so')) + args.stub:
        listing = subprocess.run([args.nm, '-D', '--defined-only', str(library)], capture_output=True, text=True).stdout
        for line in listing.splitlines():
            if line.strip():
                names.setdefault(nid(line.split()[-1].split('@')[0]), line.split()[-1].split('@')[0])
    found = imports(args.elf)
    refused = sorted(f'{names.get(n, n)} ({library})' for n, library in found
                     if library in NEVER_LOADED or library == '?' or names.get(n) in UNRESOLVED_NAMES)
    if refused:
        print(f'{args.elf}: {len(refused)} of {len(found)} imports resolve to nothing in a title, so calling one jumps '
              f'to address 0: {", ".join(refused)}', file=sys.stderr)
        return 1
    print(f'title imports: {len(found)}, from {", ".join(sorted({library for _, library in found}))}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
