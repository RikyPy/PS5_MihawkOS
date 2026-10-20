#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Extract the PE64 D3D11 runtime from the checksum-pinned GE-Proton release."""
import hashlib
import tarfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
NAME = 'GE-Proton11-7-x86_64'
SHA256 = 'c5448b76a230384e2d7bc6beb5ccb97bafb7e2c3b6c527cb03a1a546bbcb00a0'
archive = ROOT / '.deps' / f'{NAME}.tar.gz'
with archive.open('rb') as data:
    assert hashlib.file_digest(data, 'sha256').hexdigest() == SHA256, 'GE archive checksum mismatch'
out = ROOT / '.deps/dxvk-release'
out.mkdir(exist_ok=True)


def put(path, data):
    """Write a file only when it is missing or differs: a worktree's .deps/dxvk-release may be another checkout's."""
    if not path.is_file() or path.read_bytes() != data:
        path.write_bytes(data)


with tarfile.open(archive) as tar:
    for name in ('d3d11.dll', 'dxgi.dll', 'd3d10core.dll', 'd3d9.dll', 'version'):
        member = f'{NAME}/files/lib/wine/dxvk/' + (name if name == 'version' else f'x86_64-windows/{name}')
        with tar.extractfile(member) as data:
            put(out / name, data.read())
with tarfile.open(archive) as tar:
    for name in ('d3d12.dll', 'd3d12core.dll', 'version'):
        member = f'{NAME}/files/lib/wine/vkd3d-proton/' + (name if name == 'version' else f'x86_64-windows/{name}')
        with tar.extractfile(member) as data:
            put(out / ('vkd3d-' + name if name == 'version' else name), data.read())
# The i386 builds for 32-bit programs under WoW64 (docs/IA32_DESIGN.md, M4 step 3), d3d8 among them (a 32-bit API).
(out / 'i386').mkdir(exist_ok=True)
with tarfile.open(archive) as tar:
    for folder, names in (('dxvk', ('d3d11.dll', 'dxgi.dll', 'd3d10core.dll', 'd3d9.dll', 'd3d8.dll')),
                          ('vkd3d-proton', ('d3d12.dll', 'd3d12core.dll'))):
        for name in names:
            with tar.extractfile(f'{NAME}/files/lib/wine/{folder}/i386-windows/{name}') as data:
                put(out / 'i386' / name, data.read())
print('DXVK:', (out / 'version').read_text().strip())
print('VKD3D-Proton:', (out / 'vkd3d-version').read_text().strip())
print('Extracted the PE64 DLLs and their i386 builds from the verified GE-Proton11-7 archive')
