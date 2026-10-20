#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Verify static spawn landmarks against user-supplied local ELF files.

Read-only, offline: no console access, extraction, disassembly or firmware output.
Hashes pin the manually reviewed inputs. This verifies selected symbols and call
edges, not the complete ABI, permissions, or runtime viability of spawning.
"""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[1]
REFERENCE = ROOT / 'evidence/service-spawn-static-10.01.json'


def nid(name):
    salt = bytes.fromhex('518d64a635ded8c1e6b039b1c3e55230')
    digest = hashlib.sha1(name.encode() + salt).digest()[:8][::-1]
    return base64.b64encode(digest).decode().rstrip('=').replace('/', '-')


class Elf:
    def __init__(self, data):
        self.data = data
        if data[:6] != b'\x7fELF\x02\x01':
            raise ValueError('expected little-endian ELF64')
        offset = struct.unpack_from('<Q', data, 32)[0]
        stride, count = struct.unpack_from('<HH', data, 54)
        self.segments = [struct.unpack_from('<IIQQQQQQ', data, offset + i * stride)
                         for i in range(count)]
        dynamic = next(p for p in self.segments if p[0] == 2)
        tags = dict(struct.unpack_from('<qQ', data, i)
                    for i in range(dynamic[2], dynamic[2] + dynamic[5], 16))
        strings = self.read(tags[5], tags[10])
        # A symbol NID alone is insufficient: private exports can live in
        # another library inside the same loaded module.
        alphabet = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-'
        libraries = {}
        for i in range(dynamic[2], dynamic[2] + dynamic[5], 16):
            tag, value = struct.unpack_from('<qQ', data, i)
            if tag == 0x61000047:
                name = value & 0xffffffff
                token_id = value >> 48
                if token_id < 64: # Selected libraries in this reference use one character.
                    libraries[alphabet[token_id]] = strings[name:strings.index(b'\0', name)].decode()
        self.export_libraries = {}
        self.symbols = []
        for i in range(0, tags[0x6100003f], 24):
            name, _, _, section, address, _ = struct.unpack(
                '<IBBHQQ', self.read(tags[6] + i, 24))
            end = strings.index(b'\0', name)
            raw = strings[name:end].decode().split('#')
            if section and len(raw) == 3:
                self.export_libraries[raw[0]] = libraries.get(raw[1])
            self.symbols.append((raw[0],
                                 bool(section), address))
        self.exports = {name: address for name, defined, address in self.symbols if defined}
        self.import_slots = {}
        for tag, size_tag in ((7, 8), (23, 2)):
            if tag not in tags:
                continue
            for i in range(0, tags[size_tag], 24):
                slot, info, _ = struct.unpack('<QQq', self.read(tags[tag] + i, 24))
                if info >> 32:
                    self.import_slots[slot] = self.symbols[info >> 32][0]

    def read(self, address, size):
        for kind, _, offset, start, _, length, _, _ in self.segments:
            if kind == 1 and start <= address and address + size <= start + length:
                return self.data[offset + address - start:offset + address - start + size]
        raise ValueError('address outside file-backed load segments')

    def check_call(self, checkpoint):
        site = int(checkpoint['site_rva'], 16)
        code = self.read(site, 5)
        if code[0] != 0xe8:
            return False
        target = site + 5 + struct.unpack('<i', code[1:])[0]
        expected = checkpoint['target']
        kind = checkpoint['target_kind']
        if kind == 'internal':
            return target == int(expected, 16)
        if kind == 'export':
            return self.exports.get(nid(expected)) == target
        if kind != 'import':
            raise ValueError('unknown call checkpoint type')
        jump = self.read(target, 6)
        if jump[:2] != b'\xff\x25':
            return False
        slot = target + 6 + struct.unpack('<i', jump[2:])[0]
        return self.import_slots.get(slot) == nid(expected)


def verify(system, reference):
    checks = 0
    modules = []
    for module in reference['modules']:
        relative = Path(module['path'])
        if relative.is_absolute() or '..' in relative.parts:
            raise ValueError('reference path must stay inside the system directory')
        data = (system / relative).read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        if digest != module['sha256']:
            raise ValueError(str(relative) + ': input hash differs; review this revision first')
        elf = Elf(data)
        for name, address in module['exports'].items():
            if elf.exports.get(nid(name)) != int(address, 16):
                raise ValueError(str(relative) + ': export mismatch for ' + name)
            checks += 1
        for name, library in module.get('export_libraries', {}).items():
            if elf.export_libraries.get(nid(name)) != library:
                raise ValueError(str(relative) + ': export library mismatch for ' + name)
            checks += 1
        for name in module.get('absent_exports', []):
            if nid(name) in elf.exports:
                raise ValueError(str(relative) + ': unexpected export ' + name)
            checks += 1
        for call in module['calls']:
            if not elf.check_call(call):
                raise ValueError(str(relative) + ': call mismatch at ' + call['site_rva'])
            checks += 1
        modules.append({'path': str(relative), 'sha256': digest})
    return {'kind': 'static_metadata_verification', 'modules': modules,
            'checks_passed': checks, 'runtime_child_proven': False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--system', type=Path, required=True,
                        help='local unpacked system directory; never fetched')
    parser.add_argument('--output', type=Path, help='optional metadata-only JSON report')
    parser.add_argument('--reference',type=Path,default=REFERENCE,help='reviewed metadata-only reference JSON')
    args = parser.parse_args()
    report = verify(args.system, json.loads(args.reference.read_text()))
    if args.output:
        args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(f"PASS: {len(report['modules'])} pinned inputs; {report['checks_passed']} static checks. "
          'No runtime spawn was attempted.')


if __name__ == '__main__':
    main()
