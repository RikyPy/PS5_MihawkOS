#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Report what a game's PE files are: architecture, managed or native, CLR flags, imports.

Reads headers only; never copies or modifies game files. Usage:
  tools/audit-pe.py [--json] FILE_OR_DIRECTORY...
"""
import argparse
import json
import struct
import sys
from pathlib import Path

MACHINES = {0x14c: 'i386', 0x8664: 'x86-64', 0xaa64: 'arm64', 0x1c4: 'armnt'}
COR_ILONLY, COR_32BITREQUIRED, COR_STRONGNAMESIGNED, COR_32BITPREFERRED = 0x1, 0x2, 0x8, 0x20000
PE_SUFFIXES = {'.exe', '.dll', '.drv', '.sys'}


def rva_to_offset(sections, rva):
    for virtual, size, raw, raw_size in sections:
        if virtual <= rva < virtual + max(size, raw_size):
            return raw + rva - virtual
    return None


def c_string(data, offset):
    end = data.index(b'\0', offset)
    return data[offset:end].decode('ascii', 'replace')


def clr_metadata(data, sections, rva):
    """The CLR runtime header and the metadata version string, plus #Strings
    entries that look like assembly and module references (cheap and enough for
    an audit: the full metadata tables are not decoded)."""
    offset = rva_to_offset(sections, rva)
    if offset is None:
        return None
    (_cb, major, minor, meta_rva, _meta_size, flags) = struct.unpack_from('<IHHIII', data, offset)
    info = {'runtime_header': f'{major}.{minor}', 'flags': flags,
            'ilonly': bool(flags & COR_ILONLY),
            '32bitrequired': bool(flags & COR_32BITREQUIRED),
            '32bitpreferred': bool(flags & COR_32BITPREFERRED)}
    meta = rva_to_offset(sections, meta_rva)
    if meta is not None and data[meta:meta + 4] == b'BSJB':
        length = struct.unpack_from('<I', data, meta + 12)[0]
        info['metadata_version'] = data[meta + 16:meta + 16 + length].rstrip(b'\0').decode()
    return info


def imports(data, sections, rva, pe32plus):
    names = []
    offset = rva_to_offset(sections, rva)
    while offset is not None:
        entry = struct.unpack_from('<IIIII', data, offset)
        if not any(entry):
            break
        name = rva_to_offset(sections, entry[3])
        if name is not None:
            names.append(c_string(data, name))
        offset += 20
    return names


def delay_imports(data, sections, rva):
    names = []
    offset = rva_to_offset(sections, rva)
    while offset is not None:
        entry = struct.unpack_from('<IIIIIIII', data, offset)
        if not any(entry):
            break
        name = rva_to_offset(sections, entry[1])
        if name is not None:
            names.append(c_string(data, name))
        offset += 32
    return names


def audit(path):
    data = path.read_bytes()
    if data[:2] != b'MZ':
        return None
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    if data[pe:pe + 4] != b'PE\0\0':
        return None
    machine, count, _stamp, _sym, _nsym, opt_size, characteristics = struct.unpack_from('<HHIIIHH', data, pe + 4)
    opt = pe + 24
    magic = struct.unpack_from('<H', data, opt)[0]
    pe32plus = magic == 0x20b
    subsystem = struct.unpack_from('<H', data, opt + 68)[0]
    dirs = opt + (112 if pe32plus else 96)
    ndirs = struct.unpack_from('<I', data, dirs - 4)[0]
    directories = [struct.unpack_from('<II', data, dirs + 8 * i) for i in range(min(ndirs, 16))]
    sections = []
    table = opt + opt_size
    for i in range(count):
        _name, vsize, vaddr, raw_size, raw = struct.unpack_from('<8sIIII', data, table + 40 * i)
        sections.append((vaddr, vsize, raw, raw_size))
    result = {'file': str(path), 'machine': MACHINES.get(machine, hex(machine)),
              'format': 'PE32+' if pe32plus else 'PE32', 'dll': bool(characteristics & 0x2000),
              'subsystem': {2: 'gui', 3: 'console'}.get(subsystem, subsystem)}
    if len(directories) > 1 and directories[1][0]:
        result['imports'] = imports(data, sections, directories[1][0], pe32plus)
    if len(directories) > 13 and directories[13][0]:
        result['delay_imports'] = delay_imports(data, sections, directories[13][0])
    if len(directories) > 14 and directories[14][0]:
        result['clr'] = clr_metadata(data, sections, directories[14][0])
        clr = result['clr'] or {}
        if machine == 0x14c and clr.get('ilonly'):
            # AnyCPU unless a flag asks for 32-bit; 32BITPREFERRED without
            # 32BITREQUIRED is still AnyCPU-capable on a 64-bit runtime.
            clr['process'] = ('x86 only' if clr['32bitrequired'] and not clr['32bitpreferred'] else
                              'AnyCPU, prefers 32-bit' if clr['32bitpreferred'] else 'AnyCPU')
        elif machine == 0x8664:
            clr['process'] = 'x64 only'
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--json', action='store_true')
    parser.add_argument('paths', nargs='+', type=Path)
    args = parser.parse_args()
    results = []
    for root in args.paths:
        files = [root] if root.is_file() else sorted(p for p in root.rglob('*') if p.suffix.lower() in PE_SUFFIXES)
        for path in files:
            try:
                found = audit(path)
            except (struct.error, ValueError, IndexError) as error:
                found = {'file': str(path), 'error': str(error)}
            if found:
                results.append(found)
    if args.json:
        json.dump(results, sys.stdout, indent=1)
        print()
        return
    for item in results:
        if 'error' in item:
            print(f"{item['file']}: unreadable ({item['error']})")
            continue
        line = f"{item['file']}: {item['format']} {item['machine']} {'dll' if item['dll'] else 'exe'} {item['subsystem']}"
        clr = item.get('clr')
        if clr:
            line += (f" managed {clr.get('metadata_version', '?')} flags={clr['flags']:#x}"
                     f" 32BITREQUIRED={int(clr['32bitrequired'])} 32BITPREFERRED={int(clr['32bitpreferred'])}"
                     f" -> {clr.get('process', 'native machine')}")
        print(line)
        for key in ('imports', 'delay_imports'):
            if item.get(key):
                print(f"  {key}: {', '.join(item[key])}")


if __name__ == '__main__':
    main()
